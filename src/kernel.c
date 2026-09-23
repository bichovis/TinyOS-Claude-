/* kernel.c - Nucleo de TinyOS */
#include <stdint.h>
#include "uart.h"
#include "exception.h"
#include "irq.h"
#include "timer.h"
#include "mm.h"
#include "mmio.h"
#include "mbox.h"
#include "smp.h"
#include "sched.h"
#include "sync.h"
#include "ipc.h"
#include "file.h"
#include "fpu.h"

#define TICK_HZ      100

/* La imagen del proceso de usuario, empotrada por tools/bin2c.py. Cuando
 * haya un sistema de ficheros, esto sera una carga de verdad desde disco. */
extern const uint8_t  user_hello[];      extern const uint64_t user_hello_size;
extern const uint8_t  user_conserver[];  extern const uint64_t user_conserver_size;
extern const uint8_t  user_client[];     extern const uint64_t user_client_size;
extern const uint8_t  user_fs[];         extern const uint64_t user_fs_size;
extern const uint8_t  user_ls[];         extern const uint64_t user_ls_size;
extern const uint8_t  user_cat[];        extern const uint64_t user_cat_size;
extern const uint8_t  user_run[];        extern const uint64_t user_run_size;
extern const uint8_t  user_sh[];         extern const uint64_t user_sh_size;


/* --- Enrutar los pines de la tarjeta al EMMC --------------------------
 *
 * En la Pi 3 hay DOS controladores de SD: el Arasan (el EMMC de arriba) y
 * el SDHOST de Broadcom. Por defecto el EMMC esta cableado al modulo WiFi
 * y no al zocalo de la tarjeta; para que hable con la SD hay que llevar los
 * GPIO 48 a 53 a su funcion alternativa 3.
 *
 * QEMU no emula el multiplexado de pines, asi que alli el driver funciona
 * sin esto y en la placa no funciona en absoluto.
 *
 * Y esto lo hace el KERNEL, no el driver, aunque el driver viva en EL0 y
 * sea el dueño del periferico. El motivo es que el bloque GPIO es uno solo
 * para toda la placa: darle al servidor de ficheros la pagina del GPIO
 * seria darle tambien los pines de la UART, y con ellos la posibilidad de
 * dejar al sistema sin consola. Conceder un periferico incluye enrutarle
 * los pines, y el pin mux se queda donde estan los privilegios.
 */
#define GPIO_BASE   (PERIPHERAL_BASE + 0x200000)
#define GPPUD       (GPIO_BASE + 0x94)
#define GPPUDCLK1   (GPIO_BASE + 0x9C)

static void gpio_function(int pin, uint32_t fn)
{
    uint64_t reg   = GPIO_BASE + 4 * (pin / 10);
    int      shift = (pin % 10) * 3;
    uint32_t v     = mmio_read(reg);

    v &= ~(7u << shift);
    v |=  (fn & 7u) << shift;
    mmio_write(reg, v);
}

static void sd_route_pins(void)
{
    gpio_function(47, 0);                   /* deteccion de tarjeta: entrada */
    for (int pin = 48; pin <= 53; pin++)
        gpio_function(pin, 7);              /* ALT3 = EMMC                   */

    /* Resistencias de pull-up en los seis. La secuencia es la que manda el
     * manual de Broadcom y no se puede abreviar: poner el modo, esperar,
     * marcar los pines, esperar, y limpiar las dos cosas. */
    uint32_t pines = 0;
    for (int pin = 47; pin <= 53; pin++)
        pines |= 1u << (pin - 32);          /* GPPUDCLK1 cubre el 32 al 53 */

    mmio_write(GPPUD, 2);                   /* 2 = pull-up */
    delay_cycles(150);
    mmio_write(GPPUDCLK1, pines);
    delay_cycles(150);
    mmio_write(GPPUD, 0);
    mmio_write(GPPUDCLK1, 0);
}

/* Ventana virtual para los experimentos, dentro del espacio del kernel pero
 * fuera del mapa lineal: ahi no hay nada fisico. Que funcione es justamente
 * la demostracion de que la traduccion existe. */
#define TEST_VA_A    (KERNEL_VA_BASE + 0xC0000000UL)
#define TEST_VA_B    (KERNEL_VA_BASE + 0xC0001000UL)
#define TEST_VA_RO   (KERNEL_VA_BASE + 0xC0010000UL)

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

void kernel_main(uint64_t dtb_ptr)
{
    uart_init();
    exception_init();
    irq_init();

    uart_puts("\n");
    uart_puts("+--------------------------------------+\n");
    uart_puts("|  TinyOS  -  microkernel RPi 3B       |\n");
    uart_puts("|  microkernel ARM64 bare metal        |\n");
    uart_puts("+--------------------------------------+\n\n");

    uart_puts("  Exception Level : EL");
    uart_putc((char)('0' + current_el()));
    uart_puts("\n  Device tree en  : 0x");
    uart_hex64(dtb_ptr);
    uart_puts("\n  MMU             : ");
    uart_puts(mmu_is_on() ? "encendida desde boot.S\n" : "APAGADA (?)\n");
    uart_puts("  Kernel en       : 0x");
    uart_hex64(KERNEL_VA_BASE + 0x80000);
    uart_puts("  (TTBR1)\n");

    timer_init(TICK_HZ);
    uart_enable_rx_irq();
    irq_enable();

    /* Preguntar a la GPU cuanta RAM nos ha dejado. Tiene que hacerse ANTES
     * de encender la MMU: la GPU lee el buffer de la RAM y no ve nuestra
     * cache. Si no contesta (o estamos en un emulador que no lo implementa)
     * nos quedamos con el tope teorico. */
    uint64_t ram_base = 0, ram_size = 0;
    if (mbox_arm_memory(&ram_base, &ram_size)) {
        uart_puts("  RAM de la CPU   : ");
        uart_dec(ram_size / 1024 / 1024);
        uart_puts(" MB  (base 0x");
        uart_hex64(ram_base);
        uart_puts(")\n");
    } else {
        uart_puts("  RAM de la CPU   : la GPU no contesta, supongo el maximo\n");
        ram_size = RAM_MAX;
    }

    pmm_init(ram_base + ram_size);
    ipc_init();
    file_init();
    mem_stats();

    /* Tres cuartas partes de la maquina llevan aparcadas desde el paso 1.
     * A partir de aqui estan encendidas, cada una con su pila y su MMU,
     * compartiendo las tablas del kernel. Todavia no hacen nada: sin
     * cerrojos de verdad no pueden tocar lo que es de todos. */
    uart_puts("\n  Despertando los nucleos 1, 2 y 3...\n");
    int vivos = smp_start_secondaries();
    uart_puts("    han contestado ");
    uart_dec((uint64_t)vivos);
    uart_puts(" de 3\n");
    smp_dump();

    /* Enrutar los pines de la tarjeta al EMMC. Lo hace el KERNEL porque
     * es quien tiene la pagina de GPIO, y darsela al servidor de ficheros
     * seria darle tambien la UART, que esta en la misma pagina. Es la
     * unica parte del driver de SD que no vive en EL0, y esta aqui por
     * donde cae en el mapa de memoria, no por lo que hace. */
    sd_route_pins();

    sched_init();

    /* Los otros tres nucleos entran al planificador. Hasta esta linea se
     * han limitado a contar sus ticks. */
    uart_puts("\n  Abriendo el planificador a los cuatro nucleos...\n");
    sched_start_smp();

    /* Y AQUI SE ACABA LO QUE EL KERNEL SABE DEL SISTEMA.
     *
     * Arranca un proceso, init, y se olvida. Quien levanta los drivers, en
     * que orden, de donde sale el entorno y que interprete se usa son
     * decisiones que ya no estan aqui: estan en user/init.c y en /etc/rc.
     *
     * Hasta el paso 43 habia aqui debajo mil lineas de demostraciones -un
     * menu de veinticinco teclas, nueve hilos contando cosas, carreras de
     * datos a proposito- que fueron el proyecto mientras se aprendia como
     * funcionaba cada pieza. Ya no: ahora hay un sistema, y las
     * demostraciones se hacen desde el, con programas. */
    {
        struct args a, e;
        args_de_cadena(&a, "init");
        args_de_cadena(&e, "");

        int pid = task_bootstrap("init", &a, &e, DEV_NINGUNO);
        if (pid < 0) {
            uart_puts("\n  [kernel] no arranca init. Aqui no hay nada mas"
                      " que hacer.\n");
        } else {
            task_set_init_pid((uint64_t)pid);
        }
    }

    /* La tarea 0 se queda de idle pura, igual que los otros tres nucleos:
     * solo se ejecuta cuando nadie mas quiere CPU. */
    idle_loop();
}
