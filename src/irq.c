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

/* IRQ_UART (57) e IRQ_USB (9) los define ipc_abi.h: son parte del contrato
 * con los drivers de EL0, no un detalle interno de este fichero.
 *
 * Las 64 fuentes del controlador [1] viven en dos bancos de 32, y cada
 * registro tiene su pareja. Hasta este paso todo esto estaba escrito a mano
 * para el 57 -banco 2, bit 25- en los cinco sitios que lo tocaban. Con dos
 * fuentes y una en cada banco, eso deja de valer. */
static void irq_abrir(uint64_t irq)
{
    if (irq < 32) mmio_write(ENABLE_IRQS_1, 1u << irq);
    else          mmio_write(ENABLE_IRQS_2, 1u << (irq - 32));
}

static void irq_cerrar(uint64_t irq)
{
    if (irq < 32) mmio_write(DISABLE_IRQS_1, 1u << irq);
    else          mmio_write(DISABLE_IRQS_2, 1u << (irq - 32));
}

static int irq_pendiente(uint32_t p1, uint32_t p2, uint64_t irq)
{
    return (irq < 32) ? (p1 & (1u << irq)) != 0
                      : (p2 & (1u << (irq - 32))) != 0;
}

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

    /* [1] Habilitar la de la UART, y SOLO esa.
     *
     * Es la unica que el kernel sabe atender por si mismo -tiene su propio
     * driver de PL011- asi que puede estar abierta desde el arranque. Las
     * demas se abren cuando alguien las reclama y se cierran cuando la
     * suelta, porque una fuente abierta que nadie atiende es un sistema
     * girando en el manejador para siempre. */
    irq_abrir(IRQ_UART);

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
/* Una tabla, y no una variable.
 *
 * Aqui habia un solo hueco -'irq_puerto'- y una comprobacion que decia
 * "solo la UART". Con dos drivers de EL0 eso no vale: el USB de la Pi 3B
 * lleva detras la red y el almacenamiento externo, asi que el dia que haya
 * un driver de USB habra DOS procesos esperando interrupciones distintas.
 *
 * Sigue siendo pequenya y sigue estando escrita a mano. Lo que ya no es, es
 * un unico hueco. */
#define MAX_IRQS_EL0   4

static struct {
    uint64_t irq;                 /* 0 = hueco libre */
    int      puerto;
} irqs_el0[MAX_IRQS_EL0];

/* Y la lista de lo que se puede reclamar. Es la frontera de privilegio de
 * las interrupciones, y por eso esta aqui y no la elige quien llama. */
static int reclamable(uint64_t irq)
{
    return irq == IRQ_UART || irq == IRQ_USB;
}

static int hueco_de(uint64_t irq)
{
    for (int i = 0; i < MAX_IRQS_EL0; i++)
        if (irqs_el0[i].irq == irq) return i;
    return -1;
}

/* Avisos que no cupieron en la cola del driver. Si esto no es cero, el
 * driver no da abasto. */
uint64_t irq_avisos_perdidos;

int irq_register(uint64_t irq, int puerto)
{
    if (!reclamable(irq) || puerto < 0) return -1;
    if (hueco_de(irq) >= 0)             return -1;   /* ya la lleva otro */

    int libre = hueco_de(0);
    if (libre < 0) return -1;                        /* no caben mas */

    irqs_el0[libre].irq    = irq;
    irqs_el0[libre].puerto = puerto;

    /* Abrirla AL RECLAMARLA. La de la UART ya estaba abierta y volver a
     * abrirla no hace danyo; las demas no lo estaban, porque hasta ahora no
     * habia nadie que supiera atenderlas. */
    irq_abrir(irq);

    /* Y si lo que se reclama es la UART, el kernel le CEDE tambien la
     * salida. Es un solo dispositivo y se entrega entero: quedarse la
     * escritura mientras otro se lleva la lectura es lo que hacia que dos
     * escritores se pisaran en la FIFO y se perdiera la mitad del texto. */
    if (irq == IRQ_UART) uart_ceder();
    return 0;
}

/* El proceso que la tenia ha muerto: el kernel recupera sus fuentes.
 *
 * Lo que se hace con cada una NO es lo mismo, y es la diferencia entre las
 * dos clases de interrupcion que hay aqui: la de la UART se vuelve a abrir,
 * porque el kernel tiene driver propio y puede seguir el solo -es lo que
 * hace que matar al conserver no deje la maquina sin teclado-. Cualquier
 * otra se CIERRA, porque no hay nadie detras: dejarla abierta sin quien la
 * atienda es colgar la maquina en el manejador. */
void irq_release_port(int puerto)
{
    for (int i = 0; i < MAX_IRQS_EL0; i++) {
        if (irqs_el0[i].irq == 0 || irqs_el0[i].puerto != puerto) continue;

        uint64_t irq = irqs_el0[i].irq;
        irqs_el0[i].irq    = 0;
        irqs_el0[i].puerto = -1;

        if (irq == IRQ_UART) { irq_abrir(irq); uart_recuperar(); }
        else                   irq_cerrar(irq);
    }
}

/* ¿Es ESE proceso el duenyo de esa interrupcion? Lo pregunta syscall.c para
 * saber quien puede sacar el texto del kernel: el duenyo de la UART y nadie
 * mas. La tabla guarda puertos, no pids, asi que hay que preguntarle a ipc.c
 * de quien es el puerto. */
int irq_es_duenyo(uint64_t irq, uint64_t pid)
{
    int h = hueco_de(irq);
    if (h < 0 || !pid) return 0;
    return port_owner(irqs_el0[h].puerto) == pid;
}

int irq_ack(uint64_t irq)
{
    if (hueco_de(irq) < 0) return -1;    /* no es tuya, no la reabres */
    irq_abrir(irq);
    return 0;
}

/* Decirle al duenyo de la consola que hay texto del kernel esperando.
 *
 * Con una bandera para no inundarle: mientras no haya vaciado, un aviso basta.
 * La bandera se levanta cuando el anillo se queda vacio, que es lo unico que
 * significa "ya lo tiene todo". */
static volatile int klog_avisado;

static void klog_avisar(void)
{
    int h = hueco_de(IRQ_UART);
    if (h < 0) return;                    /* la UART la lleva el kernel */

    if (!uart_klog_hay()) { klog_avisado = 0; return; }
    if (klog_avisado)     return;

    if (port_notify(irqs_el0[h].puerto, CMSG_KLOG) == 0)
        klog_avisado = 1;
}

void irq_handle(void)
{
    uint64_t core = this_core();
    irqs[core]++;

    /* Cada nucleo pregunta por su propio registro: el del 0 no dice nada
     * de lo que le ha pasado al 2. */
    uint32_t src = mmio_read(CORE_IRQ_SOURCE(core));

    if (src & SRC_CNTPNSIRQ) {
        timer_irq();

        /* Y de paso, avisar al duenyo de la consola de que el kernel ha
         * escrito algo.
         *
         * Aqui y no en el sitio donde se escribe, y el motivo es el orden de
         * cerrojos que uart.h tiene escrito: quien tiene el de la UART no
         * puede pedir sched_lock, y port_notify lo pide. Escribir texto
         * ocurre con el cerrojo de la UART cogido; este manejador, no.
         *
         * El precio es hasta 10 ms de retraso para un mensaje del kernel, que
         * no se nota en algo que se lee con los ojos. Y el eco de las teclas
         * no pasa por aqui: de eso se encarga el propio conserver, que vacia
         * el anillo justo despues de entregar lo que ha leido. */
        klog_avisar();
    }

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
        /* Segunda pregunta: dentro del controlador [1], quien fue. Se leen
         * los dos bancos una sola vez, no uno por cada fuente registrada. */
        uint32_t p1 = mmio_read(IRQ_PENDING_1);
        uint32_t p2 = mmio_read(IRQ_PENDING_2);

        /* Las que tienen duenyo en EL0. */
        for (int i = 0; i < MAX_IRQS_EL0; i++) {
            uint64_t irq = irqs_el0[i].irq;
            if (!irq || !irq_pendiente(p1, p2, irq)) continue;

            /* Se le avisa y se CIERRA hasta que diga que ya.
             *
             * Y si el aviso NO se puede entregar -la cola del puerto
             * llena- hay que volver a abrirla inmediatamente. Esto no es
             * una precaucion teorica: enmascarar y no avisar deja la
             * fuente cerrada esperando un irq_ack que nadie va a hacer, y
             * el teclado se muere para siempre sin un solo mensaje de
             * error. Es la version con interrupciones del mismo fallo de
             * siempre: dos pasos que tienen que pasar los dos o ninguno. */
            irq_cerrar(irq);

            if (port_notify(irqs_el0[i].puerto, CMSG_IRQ) < 0) {
                irq_avisos_perdidos++;
                irq_abrir(irq);
            }
        }

        /* Y la UART cuando todavia la lleva el kernel, que es el caso de
         * antes de que init arranque el conserver -y el de despues, si el
         * conserver se muere-. */
        if (irq_pendiente(p1, p2, IRQ_UART) && hueco_de(IRQ_UART) < 0)
            uart_irq();
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
