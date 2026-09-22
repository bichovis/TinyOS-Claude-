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
    uint64_t    stack;        /* pagina fisica de la pila                   */
    uint64_t    counter;      /* ticks que le quedan de su turno            */
    uint64_t    ticks_run;    /* CPU consumida en total                     */
    uint64_t    wake_tick;    /* si duerme, cuando despertar                */
    struct task *wait_next;   /* encadenamiento dentro de una cola de espera */
    uint64_t   *pgd;          /* tabla TTBR0 propia; 0 = hilo de kernel      */
    uint64_t    mmio_va;      /* MMIO concedido a un driver de EL0; 0 si no  */
    const char *name;
};

extern struct task *current;

void sched_init(void);
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
void sched_dump(void);
