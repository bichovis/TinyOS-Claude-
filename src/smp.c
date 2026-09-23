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
#include "sched.h"
#include "spinlock.h"
#include "irq.h"
#include "exception.h"

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

/* ====================== EL CONTADOR EN DISPUTA =====================
 *
 * Cuatro hilos de kernel sumando sobre la misma variable. Quien los
 * reparte entre los nucleos es el planificador, que es exactamente como
 * ocurren las carreras de verdad: nadie las programa a proposito.
 */
static volatile uint64_t contador;
static volatile uint64_t hechos;
static volatile uint64_t listos;
static volatile uint64_t nucleo_de[CORES];
static volatile uint64_t idx_sig;
static uint64_t          hammer_iters;
static uint64_t          hammer_cuantos;
static int               hammer_lock;
static struct spinlock   contador_lock = SPINLOCK("contador");

/* El incremento en disputa. Sin cerrojo son tres pasos -leer, sumar,
 * escribir- y entre ellos cabe entero otro nucleo haciendo lo mismo: los
 * dos leen el mismo valor y los dos escriben el mismo, asi que dos
 * incrementos cuentan como uno. */
static void hammer_thread(void *arg)
{
    (void)arg;

    /* Barrera de salida: esperar a estar los cuatro antes de empezar.
     *
     * Sin esto no hay nada que ver: cada hilo termina sus vueltas antes de
     * que el siguiente llegue a arrancar, y el contador sale perfecto. La
     * corrupcion no la causa compartir el dato, la causa compartirlo AL
     * MISMO TIEMPO. Una carrera que no se solapa no es una carrera. */
    uint64_t f = spin_lock_irqsave(&contador_lock);
    listos++;
    spin_unlock_irqrestore(&contador_lock, f);

    while (listos < hammer_cuantos)
        task_yield();

    /* Dejar constancia de en que nucleo nos toca correr: si los cuatro
     * dicen el mismo, no hay carrera posible por mucho que compartamos. */
    f = spin_lock_irqsave(&contador_lock);
    if (idx_sig < CORES) nucleo_de[idx_sig++] = this_core();
    spin_unlock_irqrestore(&contador_lock, f);

    for (uint64_t i = 0; i < hammer_iters; i++) {
        if (hammer_lock) {
            uint64_t f = spin_lock_irqsave(&contador_lock);
            contador++;
            spin_unlock_irqrestore(&contador_lock, f);
        } else {
            contador++;
        }
    }

    f = spin_lock_irqsave(&contador_lock);
    hechos++;                            /* tambien es un dato compartido */
    spin_unlock_irqrestore(&contador_lock, f);
}

/* Lo llama boot.S en cada nucleo secundario, ya en EL1, con la MMU
 * encendida y su propia pila. Primera instruccion de C que ejecuta un
 * nucleo que no es el 0 en la historia de este kernel. */
void secondary_main(uint64_t core)
{
    /* Adoptar el contexto de arranque de este nucleo como su tarea idle,
     * para que 'current' (o sea, TPIDR_EL1) valga algo aqui tambien. */
    sched_adopt_core(core);

    /* VBAR_EL1 tambien es por nucleo, y este no lo ha puesto nadie. Si
     * llega una interrupcion sin tabla de vectores, el salto es al vacio:
     * tiene que ser LO PRIMERO, antes de destapar nada. */
    exception_init();

    irq_init_core();                 /* que su temporizador pueda avisarle */
    timer_start_core();              /* y que cuente                       */

    fill_info(core);

    irq_enable();

    /* Y a hacer de idle, igual que el nucleo 0 al final de kernel_main. */
    idle_loop();
}

uint64_t smp_hammer(uint64_t iters, int con_cerrojo)
{
    contador     = 0;
    hechos       = 0;
    listos       = 0;
    idx_sig      = 0;
    hammer_iters = iters;
    hammer_lock  = con_cerrojo;

    /* Los parametros visibles ANTES de crear a nadie, o un hilo podria
     * arrancar leyendo el numero de vueltas viejo. */
    __asm__ volatile("dmb ish" ::: "memory");

    uint64_t creados = 0;
    for (uint64_t c = 0; c < CORES; c++)
        if (task_create("martillo", hammer_thread, 0) >= 0)
            creados++;
    hammer_cuantos = creados;           /* a cuantos espera la barrera */

    /* Esperar a que acaben, con limite: un hilo que no arranca no puede
     * llevarse por delante la consola. */
    uint64_t limite = timer_now() + timer_hz() * 10;
    while (hechos < creados && timer_now() < limite)
        task_yield();

    uart_puts("(nucleos: ");
    for (uint64_t i = 0; i < CORES; i++) {
        uart_dec(nucleo_de[i]);
        uart_puts(" ");
    }
    uart_puts(") ");
    return contador;
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
    uart_puts("\n  nucleo  MPIDR_EL1           EL  SP (su pila)        IRQ atendidas\n");
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
        uart_puts("  ");
        uart_dec(irq_count_core(c));
        uart_puts("\n");
    }
}
