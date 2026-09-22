/* kernel.c - Nucleo de TinyOS */
#include <stdint.h>
#include "uart.h"
#include "exception.h"
#include "irq.h"
#include "timer.h"
#include "mm.h"

#define TICK_HZ      100

/* Ventana virtual para los experimentos: 2 GB, donde no hay nada fisico.
 * Que funcione es justamente la demostracion de que la traduccion existe. */
#define TEST_VA_A    0x80000000UL
#define TEST_VA_B    0x80001000UL
#define TEST_VA_RO   0x80010000UL

/* Area para el benchmark de memoria: 256 KB en .bss */
#define BENCH_WORDS  (256 * 1024 / 8)
/* 'volatile': sin el, el compilador ve que nadie escribe aqui, deduce que
 * son ceros y borra el bucle de medida entero. */
static volatile uint64_t bench_area[BENCH_WORDS];

static uint64_t current_el(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return el >> 2;
}

static int mmu_is_on(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (int)(sctlr & 1);
}

/* Recorre 256 KB varias veces y devuelve los microsegundos que ha costado.
 * Con la MMU apagada no hay caches, asi que cada lectura va a la RAM. */
static uint64_t bench_memory(void)
{
    uint64_t flags = irq_save();       /* que los ticks no falseen la medida */
    uint64_t sum = 0;
    uint64_t t0 = timer_now();

    for (int pass = 0; pass < 16; pass++)
        for (uint64_t i = 0; i < BENCH_WORDS; i++)
            sum += bench_area[i];

    uint64_t elapsed = timer_now() - t0;
    irq_restore(flags);

    __asm__ volatile("" :: "r"(sum));  /* que el compilador no borre el bucle */
    return timer_us(elapsed);
}

static void show_translation(const char *what, uint64_t va)
{
    uart_puts("   ");
    uart_puts(what);
    uart_puts("  VA 0x");
    uart_hex64(va);
    uart_puts(" -> PA 0x");
    uint64_t pa = vmm_translate(va);
    if (pa) uart_hex64(pa);
    else    uart_puts("(sin traduccion)");
    uart_puts("\n");
}

static void mem_stats(void)
{
    uart_puts("\n  Memoria fisica:\n    total : ");
    uart_dec(pmm_total_pages());
    uart_puts(" paginas (");
    uart_dec(pmm_total_pages() * PAGE_SIZE / 1024 / 1024);
    uart_puts(" MB)\n    usadas: ");
    uart_dec(pmm_used_pages());
    uart_puts("\n    libres: ");
    uart_dec(pmm_free_pages());
    uart_puts(" (");
    uart_dec(pmm_free_pages() * PAGE_SIZE / 1024 / 1024);
    uart_puts(" MB)\n");
}

/* Dos direcciones virtuales distintas apuntando a la MISMA pagina fisica. */
static void demo_aliasing(void)
{
    uint64_t pa = pmm_alloc();
    if (!pa) { uart_puts("sin memoria\n"); return; }

    uart_puts("\n  Pagina fisica obtenida: 0x");
    uart_hex64(pa);
    uart_puts("\n  La mapeo en DOS direcciones virtuales distintas:\n");

    vmm_map_page(TEST_VA_A, pa, MM_RAM_RW);
    vmm_map_page(TEST_VA_B, pa, MM_RAM_RW);
    show_translation("A:", TEST_VA_A);
    show_translation("B:", TEST_VA_B);

    volatile uint64_t *a = (volatile uint64_t *)TEST_VA_A;
    volatile uint64_t *b = (volatile uint64_t *)TEST_VA_B;

    *a = 0xCAFEBABEDEADBEEFUL;
    uart_puts("  Escribo 0xCAFEBABEDEADBEEF en A... y leo B: 0x");
    uart_hex64(*b);
    uart_puts(*b == 0xCAFEBABEDEADBEEFUL ? "  <- misma pagina\n" : "  <- ERROR\n");

    *b = 0x1111222233334444UL;
    uart_puts("  Escribo 0x1111222233334444 en B... y leo A: 0x");
    uart_hex64(*a);
    uart_puts("\n");
}

/* Cambiar los permisos de una pagina ya mapeada. */
static void demo_readonly(void)
{
    uint64_t pa = pmm_alloc();
    if (!pa) { uart_puts("sin memoria\n"); return; }

    volatile uint64_t *p = (volatile uint64_t *)TEST_VA_RO;

    vmm_map_page(TEST_VA_RO, pa, MM_RAM_RW);
    *p = 0x5A5A5A5A;
    uart_puts("\n  Mapeada RW: escribo 0x5A5A5A5A y leo 0x");
    uart_hex64(*p);
    uart_puts("  OK\n");

    vmm_map_page(TEST_VA_RO, pa, MM_RAM_RO);
    uart_puts("  Remapeada RO (misma pagina fisica): leo 0x");
    uart_hex64(*p);
    uart_puts("  sigue OK\n");
    uart_puts("  Ahora escribo... deberia saltar un fallo de permisos:\n");
    *p = 0;
    uart_puts("  !!! No ha saltado: la proteccion NO funciona\n");
}

static void menu(void)
{
    uart_puts("\nComandos:\n");
    uart_puts("  p - estado de la memoria fisica\n");
    uart_puts("  v - dos direcciones virtuales, una pagina fisica\n");
    uart_puts("  r - pagina de solo lectura (FATAL: fallo de permisos)\n");
    uart_puts("  x - traducciones VA -> PA del kernel\n");
    uart_puts("  b - medir velocidad de la memoria\n");
    uart_puts("  t - tiempo e interrupciones\n");
    uart_puts("  h - ayuda\n");
    uart_puts("  4 - direccion invalida (FATAL: data abort)\n");
    uart_puts("  resto: eco\n");
}

static void command(char c)
{
    switch (c) {
    case 'p':
        mem_stats();
        break;

    case 'v':
        demo_aliasing();
        break;

    case 'r':
        demo_readonly();
        break;

    case 'x':
        uart_puts("\n  Lo que ve la MMU (instruccion 'at s1e1r'):\n");
        show_translation("kernel  ", 0x80000UL);
        show_translation("UART0   ", 0x3F201000UL);
        show_translation("timers  ", 0x40000000UL);
        show_translation("sin map ", 0x70000000UL);
        break;

    case 'b': {
        uart_puts("\n  Recorriendo 4 MB (16 pasadas sobre 256 KB)...\n    ");
        uart_dec(bench_memory());
        uart_puts(" us con la MMU ");
        uart_puts(mmu_is_on() ? "ENCENDIDA (caches activas)\n" : "apagada\n");
        break;
    }

    case 't':
        uart_puts("\n  uptime: ");
        uart_dec(timer_uptime_ms());
        uart_puts(" ms   ticks: ");
        uart_dec(timer_ticks());
        uart_puts("   IRQs: ");
        uart_dec(irq_count());
        uart_puts("\n");
        break;

    case 'h':
        menu();
        break;

    case '4': {
        volatile uint32_t *p = (volatile uint32_t *)0x0000FF0000000000UL;
        uint32_t v = *p;
        (void)v;
        break;
    }

    default:
        uart_putc(c);
        break;
    }
}

void kernel_main(uint64_t dtb_ptr)
{
    uart_init();
    exception_init();
    irq_init();

    uart_puts("\n");
    uart_puts("+--------------------------------------+\n");
    uart_puts("|  TinyOS  -  microkernel RPi 3B       |\n");
    uart_puts("|  paso 4: MMU y memoria virtual       |\n");
    uart_puts("+--------------------------------------+\n\n");

    uart_puts("  Exception Level : EL");
    uart_putc((char)('0' + current_el()));
    uart_puts("\n  Device tree en  : 0x");
    uart_hex64(dtb_ptr);
    uart_puts("\n");

    timer_init(TICK_HZ);
    uart_enable_rx_irq();
    irq_enable();

    pmm_init();
    mem_stats();

    uart_puts("\n  Midiendo la memoria SIN MMU (sin caches)...\n    ");
    uint64_t before = bench_memory();
    uart_dec(before);
    uart_puts(" us\n");

    uart_puts("\n  Construyendo las tablas de traduccion...\n");
    vmm_init();
    uart_puts("  Encendiendo MMU + caches (el suelo cambia aqui)...\n");
    vmm_enable();
    uart_puts("  Seguimos vivos. MMU: ");
    uart_puts(mmu_is_on() ? "ON\n" : "OFF\n");

    uart_puts("\n  Midiendo la memoria CON MMU y caches...\n    ");
    uint64_t after = bench_memory();
    uart_dec(after);
    uart_puts(" us\n");
    if (after > 0 && before > after) {
        uart_puts("    mejora: x");
        uart_dec(before / after);
        uart_puts("\n");
    }

    menu();

    uint64_t last_second = 0;
    for (;;) {
        __asm__ volatile("wfi");

        char c;
        while (uart_read(&c))
            command(c);

        uint64_t secs = timer_ticks() / TICK_HZ;
        if (secs != last_second)
            last_second = secs;
    }
}
