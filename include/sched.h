/* sched.h - Hilos del kernel y planificador */
#pragma once
#include <stdint.h>

#define MAX_TASKS     16
#define TASK_QUANTUM  5       /* ticks seguidos que puede correr un hilo    */
#define STACK_MAGIC   0x5441534B5F4F4B21UL   /* "TASK_OK!" al fondo de la pila */

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,               /* quiere CPU, esperando turno                */
    TASK_RUNNING,             /* la que se esta ejecutando ahora mismo      */
    TASK_SLEEPING,            /* dormida hasta cierto tick                  */
    TASK_BLOCKED,             /* esperando en una cola (mutex, canal, UART)  */
    TASK_ZOMBIE,              /* termino; su pila aun no se ha liberado     */
};

/* Lo unico que hay que salvar para cambiar de hilo: los registros que el
 * ABI de AArch64 obliga a preservar a traves de una llamada a funcion.
 * De x0 a x18 no hacen falta: el compilador ya los ha guardado si le hacian
 * falta, porque para el esto es una llamada normal y corriente. */
struct cpu_context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp;              /* x29 */
    uint64_t sp;
    uint64_t pc;              /* x30: por donde continuar                   */
};

struct task {
    struct cpu_context ctx;   /* PRIMERO: switch.S da por hecho offset 0    */
    uint64_t    state;
    uint64_t    pid;
    uint64_t    stack;        /* pila del hilo, en el mapa lineal           */
    uint64_t    counter;      /* ticks que le quedan de su turno            */
    uint64_t    ticks_run;    /* CPU consumida en total                     */
    uint64_t    wake_tick;    /* si duerme, cuando despertar                */
    struct task *wait_next;   /* encadenamiento dentro de una cola de espera */
    uint64_t   *pgd;          /* tabla TTBR0 propia; 0 = hilo de kernel      */
    uint64_t    asid;         /* etiqueta de su TLB; 0 = hilo de kernel      */
    uint64_t    mmio_va;      /* MMIO concedido a un driver de EL0; 0 si no  */
    const char *name;
};

/* 'current' ya no puede ser una variable: hay cuatro nucleos y cada uno
 * esta ejecutando una tarea distinta. Su sitio es TPIDR_EL1, un registro
 * POR NUCLEO que la arquitectura reserva justo para esto: que el software
 * guarde un puntero a "lo que este nucleo esta haciendo".
 *
 * Sigue escribiendose 'current' en todo el kernel, pero ahora cada nucleo
 * lee el suyo. */
static inline struct task *this_task(void)
{
    uint64_t t;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(t));
    return (struct task *)t;
}

static inline void set_this_task(struct task *t)
{
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(t));
}

#define current  this_task()

void sched_init(void);
void sched_adopt_core(uint64_t core);   /* lo llama cada nucleo secundario */
int  task_create(const char *name, void (*fn)(void *), void *arg);
int  task_create_user(const char *name, const uint8_t *image, uint64_t size,
                      uint64_t mmio_pa);
void schedule(void);
void scheduler_tick(void);       /* lo llama el timer                       */
void task_yield(void);           /* ceder la CPU voluntariamente            */
void task_sleep(uint64_t ticks);
void task_exit(void);
void sched_preempt(void);        /* lo llama irq_handle()                   */
uint64_t sched_switches(void);   /* cambios de contexto totales             */
uint64_t sched_reaped(void);     /* tareas cuyos recursos se han devuelto   */
void sched_dump(void);
