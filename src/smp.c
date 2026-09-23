/* smp.c - Despertar los nucleos 1, 2 y 3
 *
 * Hasta aqui, tres cuartas partes de la maquina estaban aparcadas en un
 * 'wfe' desde el paso 1. Este paso las enciende: cada nucleo baja a EL1,
 * coge su propia pila y enciende su MMU con las MISMAS tablas que el
 * nucleo 0. Compartir TTBR1 es lo que hace que el kernel sea uno solo
 * visto desde los cuatro.
 *
 * Lo que este paso NO hace todavia es dejarles tocar nada compartido: ni
 * la UART, ni el planificador, ni el gestor de memoria. Hoy por hoy todo
 * eso se protege con irq_save(), que solo tapa las interrupciones del
 * nucleo propio y a los otros tres no les dice absolutamente nada. Hasta
 * que haya cerrojos de verdad, los secundarios rellenan su ficha y se
 * duermen; de imprimirla se encarga el nucleo 0.
 */
#include <stdint.h>
#include "smp.h"
#include "mm.h"
#include "uart.h"
#include "timer.h"

extern char              secondary_entry[];   /* boot.S */
extern volatile uint64_t smp_go;              /* boot.S, en .data */

void dcache_clean_range(const void *addr, uint64_t bytes);   /* cache.S */

/* Buzones de la spin-table del firmware de la Pi 3B: ocho bytes por nucleo
 * en la primera pagina de RAM (0xE0 el nucleo 1, 0xE8 el 2, 0xF0 el 3).
 * El stub que deja el firmware tiene a los tres dando vueltas en un 'wfe'
 * hasta que alguien les escribe ahi una direccion; entonces saltan a ella.
 *
 * En QEMU no los mira nadie, porque alli los cuatro nucleos llegan solos a
 * _start y acaban esperando en nuestra propia spin-table. Escribirlos no
 * molesta, y en la placa es el unico camino. */
#define FW_SPIN_CPU1   0xE0UL

struct core_info {
    uint64_t alive;
    uint64_t mpidr;
    uint64_t el;
    uint64_t sp;
    uint64_t ttbr1;
};

static volatile struct core_info cores[CORES];

static void fill_info(uint64_t core)
{
    uint64_t mpidr, el, sp, ttbr1;

    __asm__ volatile("mrs %0, mpidr_el1"  : "=r"(mpidr));
    __asm__ volatile("mrs %0, CurrentEL"  : "=r"(el));
    __asm__ volatile("mrs %0, ttbr1_el1"  : "=r"(ttbr1));
    __asm__ volatile("mov %0, sp"         : "=r"(sp));

    cores[core].mpidr = mpidr;
    cores[core].el    = el >> 2;
    cores[core].sp    = sp;
    cores[core].ttbr1 = ttbr1;

    /* 'alive' el ultimo, y con barrera: el nucleo 0 lo esta esperando y no
     * debe verlo puesto antes que el resto de la ficha. */
    __asm__ volatile("dmb ish" ::: "memory");
    cores[core].alive = 1;
}

/* Lo llama boot.S en cada nucleo secundario, ya en EL1, con la MMU
 * encendida y su propia pila. Primera instruccion de C que ejecuta un
 * nucleo que no es el 0 en la historia de este kernel. */
void secondary_main(uint64_t core)
{
    fill_info(core);

    for (;;)
        __asm__ volatile("wfi");
}

int smp_start_secondaries(void)
{
    fill_info(0);                    /* el nucleo 0 tambien tiene ficha */

    /* 1. Dar la senyal ANTES de soltar a nadie: el que salga de la
     *    spin-table del firmware la va a mirar inmediatamente. */
    smp_go = 1;

    /* Los que esperan lo hacen con las caches apagadas, o sea leyendo la
     * RAM a secas. Si esta escritura se queda en nuestra cache no la veran
     * nunca. Es exactamente el problema del buzon de la GPU del paso 9, y
     * se arregla con la misma herramienta. */
    dcache_clean_range((const void *)&smp_go, sizeof(smp_go));

    /* 2. Y sacarlos de la spin-table del firmware, que es donde estan en
     *    hardware real. */
    uint64_t entry = virt_to_phys(secondary_entry);
    for (uint64_t c = 1; c < CORES; c++) {
        volatile uint64_t *mbox =
            (volatile uint64_t *)(KERNEL_VA_BASE + FW_SPIN_CPU1 + 8 * (c - 1));
        *mbox = entry;
        dcache_clean_range((const void *)mbox, sizeof(*mbox));
    }

    /* 3. 'sev' despierta a todo el que este en un 'wfe': los que esperan en
     *    nuestra spin-table y los que esperan en la del firmware. */
    __asm__ volatile("dsb sy\n sev" ::: "memory");

    /* 4. Esperar a que contesten, pero con un limite. Un nucleo que no
     *    arranca no puede llevarse por delante a los que si. */
    int vivos = 0;
    uint64_t limite = timer_now() + timer_hz() / 2;      /* medio segundo */
    while (timer_now() < limite) {
        vivos = 0;
        for (uint64_t c = 1; c < CORES; c++)
            if (cores[c].alive) vivos++;
        if (vivos == CORES - 1) break;
    }
    return vivos;
}

void smp_dump(void)
{
    uart_puts("\n  nucleo  MPIDR_EL1           EL  SP (su pila)        TTBR1\n");
    for (uint64_t c = 0; c < CORES; c++) {
        uart_puts("    ");
        uart_putc((char)('0' + c));
        uart_puts("     ");

        if (!cores[c].alive) {
            uart_puts("-- no ha contestado --\n");
            continue;
        }

        uart_puts("0x");
        uart_hex64(cores[c].mpidr);
        uart_puts("  ");
        uart_putc((char)('0' + cores[c].el));
        uart_puts("   0x");
        uart_hex64(cores[c].sp);
        uart_puts("  0x");
        uart_hex64(cores[c].ttbr1);
        uart_puts("\n");
    }
}
