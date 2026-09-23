/* irq.c - Enrutado de interrupciones en el BCM2837
 *
 * La Pi 3 tiene DOS controladores encadenados:
 *
 *   [1] "GPU interrupt controller" en 0x3F00B200 (heredado de la Pi 1).
 *       Recoge las IRQ de los perifericos (UART, SD, USB, DMA...). No sabe
 *       nada de multinucleo: es un mapa de bits de pendientes y otro de
 *       habilitados, repartidos en dos bancos de 32 mas uno de "basicas".
 *
 *   [2] "ARM local peripherals" en 0x40000000 (llego con el multinucleo).
 *       Decide a que NUCLEO va cada interrupcion, y ademas gestiona por su
 *       cuenta los temporizadores per-core y los mailboxes entre nucleos.
 *
 * El controlador [1] entrega todo lo suyo a [2] por una unica linea
 * agregada ("GPU IRQ"), y [2] se la pasa al nucleo que corresponda. Por eso
 * el handler tiene que preguntar dos veces: primero a [2] quien ha sido, y
 * si la respuesta es "la GPU", entonces a [1] cual de sus 64 fuentes.
 */
#include <stdint.h>
#include "mmio.h"
#include "irq.h"
#include "timer.h"
#include "uart.h"
#include "sched.h"
#include "smp.h"
#include "ipc.h"

/* --- [2] ARM local peripherals ---------------------------------------- */
/* LOCAL_BASE lo define mmio.h: 0x40000000 fisico, visto desde el mapa
 * lineal del kernel. */
#define GPU_INT_ROUTING       (LOCAL_BASE + 0x0C)  /* a que nucleo van las */
                                                   /* IRQ del controlador  */
                                                   /* de perifericos       */
/* Ojo: estos NO son un registro, son el primero de cuatro. Cada nucleo
 * tiene el suyo, uno cada 4 bytes. Escribir siempre en el del nucleo 0 es
 * lo que hacia este fichero cuando solo habia un nucleo despierto. */
#define CORE_TIMER_IRQCNTL(c) (LOCAL_BASE + 0x40 + 4 * (c))  /* que timers  */
                                                             /* avisan      */
#define CORE_MBOX_IRQCNTL(c)  (LOCAL_BASE + 0x50 + 4 * (c))  /* que buzones */
#define CORE_IRQ_SOURCE(c)    (LOCAL_BASE + 0x60 + 4 * (c))  /* quien fue   */

/* Los buzones entre nucleos: cuatro por nucleo, de 32 bits. Escribir en el
 * de otro nucleo le enciende una interrupcion, y eso es todo lo que hace
 * falta para avisarle de algo. El "write-high-to-clear" es literal: se
 * reconoce escribiendo de vuelta los bits que se leyeron. */
#define CORE_MBOX_SET(c, m)   (LOCAL_BASE + 0x80 + 0x10 * (c) + 4 * (m))
#define CORE_MBOX_CLR(c, m)   (LOCAL_BASE + 0xC0 + 0x10 * (c) + 4 * (m))

#define MBOX_RESCHED          0           /* el buzon 0 es "mirate el turno" */

#define SRC_CNTPSIRQ          (1u << 0)   /* timer fisico seguro           */
#define SRC_CNTPNSIRQ         (1u << 1)   /* timer fisico NO seguro <- ese */
#define SRC_CNTHPIRQ          (1u << 2)   /* timer del hipervisor          */
#define SRC_CNTVIRQ           (1u << 3)   /* timer virtual                 */
#define SRC_MBOX0             (1u << 4)   /* buzon 0: un IPI de otro nucleo*/
#define SRC_GPU               (1u << 8)   /* algo del controlador [1]      */

/* --- [1] GPU interrupt controller -------------------------------------- */
#define IC_BASE               (PERIPHERAL_BASE + 0xB200)
#define IRQ_PENDING_1         (IC_BASE + 0x04)     /* fuentes  0..31       */
#define IRQ_PENDING_2         (IC_BASE + 0x08)     /* fuentes 32..63       */
#define ENABLE_IRQS_1         (IC_BASE + 0x10)
#define ENABLE_IRQS_2         (IC_BASE + 0x14)
#define DISABLE_IRQS_1        (IC_BASE + 0x1C)
#define DISABLE_IRQS_2        (IC_BASE + 0x20)

#define IRQ_UART              57          /* la PL011 es la fuente 57      */

/* Una cuenta por nucleo: sumar sobre la misma variable desde cuatro
 * manejadores seria justo el error que el paso 13b acaba de ensenyar. */
static volatile uint64_t irqs[CORES];

void irq_init(void)
{
    /* Apagar todo lo que el firmware pudiera dejar encendido. */
    mmio_write(DISABLE_IRQS_1, 0xFFFFFFFF);
    mmio_write(DISABLE_IRQS_2, 0xFFFFFFFF);

    /* Mandar las interrupciones de perifericos al nucleo 0. El valor por
     * defecto ya es ese, pero en hardware real depende de lo que haya
     * dejado el firmware y no cuesta nada asegurarlo.
     *   bits [1:0] = nucleo que recibe las IRQ
     *   bits [3:2] = nucleo que recibe las FIQ                            */
    mmio_write(GPU_INT_ROUTING, 0);

    /* [1] Habilitar la fuente 57 (UART0). Como 57 >= 32, va en el banco 2
     *     y el bit dentro del banco es 57 - 32 = 25. */
    mmio_write(ENABLE_IRQS_2, 1u << (IRQ_UART - 32));

    irq_init_core();                 /* y lo que le toca al nucleo 0 */
}

/* [2] Que el temporizador fisico no-seguro de ESTE nucleo genere IRQ.
 *     Estamos en non-secure EL1 (lo fijamos con SCR_EL3.NS), de ahi que el
 *     bit correcto sea CNTPNSIRQ y no CNTPSIRQ. */
void irq_init_core(void)
{
    uint64_t core = this_core();

    mmio_write(CORE_TIMER_IRQCNTL(core), SRC_CNTPNSIRQ);

    /* Y que el buzon 0 tambien pueda interrumpirnos: es por donde los otros
     * nucleos nos diran que hay trabajo. */
    mmio_write(CORE_MBOX_IRQCNTL(core), 1u << MBOX_RESCHED);
}

/* Darle un toque a otro nucleo. No lleva informacion: el mensaje es "mirate
 * el turno", y lo que haya que mirar ya esta en la tabla de tareas.
 *
 * Esto sustituye al 'sev' a los cuatro vientos que usabamos antes, que
 * despertaba a los cuatro nucleos cada vez que alguien soltaba el cerrojo
 * del planificador, tuvieran o no algo que hacer. */
void irq_send_resched(uint64_t core)
{
    if (core < CORES)
        mmio_write(CORE_MBOX_SET(core, MBOX_RESCHED), 1);
}

/* --- Interrupciones para procesos de EL0 -----------------------------
 *
 * Un driver en espacio de usuario no puede recibir una interrupcion: las
 * interrupciones son de EL1 y ahi no se entra sin privilegios. Lo que se
 * le puede dar es un aviso.
 *
 * El trato es: cuando llega la interrupcion, el kernel la ENMASCARA y le
 * manda un mensaje. El driver la atiende a su ritmo, ya en EL0, y cuando
 * termina la vuelve a abrir. Enmascararla no es un detalle: si se dejara
 * abierta, volveria a saltar inmediatamente -el periferico sigue
 * pidiendo atencion- y el sistema se quedaria dando vueltas en el
 * manejador sin llegar nunca a ejecutar al driver que iba a arreglarlo.
 *
 * Solo se puede pedir la de la UART. Dejar que un proceso se quedara con
 * la del temporizador seria dejarle parar el planificador. */
static int irq_puerto = -1;

int irq_register(uint64_t irq, int puerto)
{
    if (irq != IRQ_UART || puerto < 0) return -1;
    if (irq_puerto >= 0) return -1;      /* ya la lleva otro */
    irq_puerto = puerto;
    return 0;
}

/* El proceso que la tenia ha muerto: el kernel la recupera y la reabre. */
void irq_release_port(int puerto)
{
    if (irq_puerto != puerto) return;
    irq_puerto = -1;
    mmio_write(ENABLE_IRQS_2, 1u << (IRQ_UART - 32));
}

int irq_ack(uint64_t irq)
{
    if (irq != IRQ_UART) return -1;
    mmio_write(ENABLE_IRQS_2, 1u << (IRQ_UART - 32));
    return 0;
}

void irq_handle(void)
{
    uint64_t core = this_core();
    irqs[core]++;

    /* Cada nucleo pregunta por su propio registro: el del 0 no dice nada
     * de lo que le ha pasado al 2. */
    uint32_t src = mmio_read(CORE_IRQ_SOURCE(core));

    if (src & SRC_CNTPNSIRQ)
        timer_irq();

    /* Un toque de otro nucleo. Reconocerlo es escribir de vuelta lo que se
     * lee; no hay mas que hacer, porque el aviso no lleva contenido: el
     * sched_preempt() del final de esta funcion es la respuesta. */
    if (src & SRC_MBOX0) {
        uint32_t v = mmio_read(CORE_MBOX_CLR(core, MBOX_RESCHED));
        mmio_write(CORE_MBOX_CLR(core, MBOX_RESCHED), v);
        sched_wake_core();
    }

    /* Las IRQ de perifericos van todas al nucleo 0 (GPU_INT_ROUTING), asi
     * que este bit solo se enciende alli. */
    if (src & SRC_GPU) {
        /* Segunda pregunta: dentro del controlador [1], quien fue. */
        uint32_t p2 = mmio_read(IRQ_PENDING_2);
        if (p2 & (1u << (IRQ_UART - 32))) {
            if (irq_puerto >= 0) {
                /* Hay un driver en EL0 esperandola: se le avisa y se cierra
                 * hasta que diga que ya. */
                mmio_write(DISABLE_IRQS_2, 1u << (IRQ_UART - 32));
                port_notify(irq_puerto, CMSG_IRQ);
            } else {
                uart_irq();              /* todavia la lleva el kernel */
            }
        }
    }

    /* Punto seguro para cambiar de hilo: el contexto del hilo interrumpido
     * ya esta entero en su pila (lo apilo kernel_entry), asi que podemos
     * congelarla y saltar a otra sin que se entere. */
    sched_preempt();
}

uint64_t irq_count(void)
{
    uint64_t total = 0;
    for (uint64_t c = 0; c < CORES; c++)
        total += irqs[c];
    return total;
}

uint64_t irq_count_core(uint64_t core)
{
    return (core < CORES) ? irqs[core] : 0;
}

/* --- Mascara de interrupciones de la propia CPU ------------------------
 * DAIF: D=Debug, A=SError, I=IRQ, F=FIQ. Un 1 significa "tapada".
 * 'daifclr #2' pone a 0 el bit I -> deja pasar las IRQ.
 * 'daifset #2' lo pone a 1      -> las tapa.
 * Ojo: esto NO desactiva el hardware, solo hace que la CPU las ignore;
 * quedan pendientes y entran en cuanto se destapan.
 */
void irq_enable(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
void irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }

uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags) :: "memory");
    irq_disable();
    return flags;
}

void irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
}
