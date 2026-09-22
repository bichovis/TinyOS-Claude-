/* sched.c - Planificador round-robin
 *
 * Politica: cada hilo listo recibe TASK_QUANTUM ticks seguidos; cuando se le
 * acaban, pasa el turno al siguiente del array. Sin prioridades, sin
 * equidad ponderada, sin nada. Es la politica mas simple que funciona, y
 * sirve perfectamente para ver el mecanismo, que es lo que importa.
 *
 * La tarea 0 es especial: es el hilo en el que ya estabamos (kernel_main).
 * No se crea, se "adopta", y hace de tarea idle: siempre esta lista, asi
 * que schedule() nunca se queda sin nadie a quien elegir.
 */
#include <stdint.h>
#include "sched.h"
#include "mm.h"
#include "irq.h"
#include "timer.h"
#include "uart.h"
#include "exception.h"

/* Definidos en switch.S */
void cpu_switch_to(struct task *prev, struct task *next);
void ret_from_fork(void);
void ret_to_user(void);

/* switch.S accede al contexto con offsets desde el principio del struct */
_Static_assert(__builtin_offsetof(struct task, ctx) == 0, "ctx debe ir primero");

static struct task tasks[MAX_TASKS];
static uint64_t    next_pid = 1;
static volatile int need_resched;
static uint64_t switches;        /* cambios de contexto totales */

struct task *current;

void sched_init(void)
{
    /* Adoptar el contexto actual como tarea 0. No hay que rellenar ctx:
     * se guardara solo la primera vez que esta tarea ceda la CPU. */
    tasks[0].state     = TASK_RUNNING;
    tasks[0].pid       = 0;
    tasks[0].name      = "idle";
    tasks[0].counter   = TASK_QUANTUM;
    tasks[0].stack     = 0;            /* usa la pila del arranque */
    current            = &tasks[0];
}

int task_create(const char *name, void (*fn)(void *), void *arg)
{
    uint64_t flags = irq_save();
    struct task *t = 0;

    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) { t = &tasks[i]; break; }
    }
    if (!t) { irq_restore(flags); return -1; }

    uint64_t stack = pmm_alloc();       /* 4 KB de pila por hilo */
    if (!stack) { irq_restore(flags); return -1; }

    /* Marca al fondo de la pila para detectar desbordamientos. Un hilo que
     * se pasa de pila no da ningun error: pisa silenciosamente lo que haya
     * debajo, y el sistema falla mucho despues y en otro sitio. */
    *(uint64_t *)stack = STACK_MAGIC;

    t->stack     = stack;
    t->name      = name;
    t->pid       = next_pid++;
    t->counter   = TASK_QUANTUM;
    t->ticks_run = 0;

    /* Preparar el contexto para que el primer cpu_switch_to aterrice en
     * ret_from_fork con la funcion en x19 y el argumento en x20. */
    for (uint64_t *p = (uint64_t *)&t->ctx;
         p < (uint64_t *)((char *)&t->ctx + sizeof(t->ctx)); p++)
        *p = 0;
    t->ctx.x19 = (uint64_t)fn;
    t->ctx.x20 = (uint64_t)arg;
    t->ctx.pc  = (uint64_t)ret_from_fork;
    t->ctx.sp  = stack + PAGE_SIZE;     /* la pila crece hacia abajo */

    t->state = TASK_READY;              /* ultimo: ya es elegible */

    irq_restore(flags);
    return (int)t->pid;
}

/* Elige el siguiente hilo listo, empezando por el de despues del actual.
 * Ese "empezando por el siguiente" es todo el round-robin que hay aqui.
 *
 * La tarea 0 (idle) NO compite: si entrara en la rotacion se llevaria un
 * turno de cada vuelta, robandole la mitad de la CPU a un hilo que si tiene
 * trabajo. Solo se elige cuando no queda nadie mas. */
static struct task *pick_next(void)
{
    uint64_t start = (uint64_t)(current - tasks);

    for (int i = 1; i <= MAX_TASKS; i++) {
        uint64_t idx = (start + (uint64_t)i) % MAX_TASKS;
        if (idx == 0) continue;
        struct task *t = &tasks[idx];
        if (t->state == TASK_READY || t->state == TASK_RUNNING)
            return t;
    }
    return &tasks[0];                   /* nadie quiere CPU: a dormir */
}

void schedule(void)
{
    uint64_t flags = irq_save();

    need_resched = 0;
    struct task *prev = current;
    struct task *next = pick_next();

    if (next != prev) {
        if (prev->state == TASK_RUNNING)
            prev->state = TASK_READY;
        next->state   = TASK_RUNNING;
        next->counter = TASK_QUANTUM;
        current       = next;
        switches++;

        /* Cambiar de espacio de direcciones. Las tareas de kernel vuelven
         * a la tabla del kernel: asi ninguna sigue corriendo sobre la de un
         * proceso que podria morir. vmm_switch_to() no hace nada si ya es
         * la activa, que es el caso habitual entre hilos de kernel. */
        vmm_switch_to(next->pgd ? next->pgd : vmm_kernel_pgd());

        cpu_switch_to(prev, next);
        /* --- Cuando la ejecucion vuelve a esta linea, han podido pasar
         * horas y haber corrido veinte hilos. Estamos otra vez en 'prev',
         * y 'flags' se lee de SU pila, no de la de nadie mas. --- */
    }

    irq_restore(flags);
}

void scheduler_tick(void)
{
    /* Despertar a las que les toque. Recorrer el array entero en cada tick
     * es ineficiente; con muchos hilos se usaria una cola ordenada. */
    uint64_t now = timer_ticks();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_SLEEPING && now >= tasks[i].wake_tick)
            tasks[i].state = TASK_READY;
    }

    if (!current) return;
    current->ticks_run++;

    if (current->counter > 0)
        current->counter--;
    if (current->counter == 0)
        need_resched = 1;       /* se le acabo el turno */
}

/* La llama irq_handle() al terminar de atender la interrupcion: es el punto
 * seguro para cambiar de hilo, con el trap_frame ya guardado en la pila. */
void sched_preempt(void)
{
    if (need_resched)
        schedule();
}

void task_yield(void)
{
    current->counter = 0;
    schedule();
}

void task_sleep(uint64_t ticks)
{
    uint64_t flags = irq_save();
    current->wake_tick = timer_ticks() + ticks;
    current->state     = TASK_SLEEPING;
    irq_restore(flags);
    schedule();                 /* no volvera hasta que alguien nos despierte */
}

void task_exit(void)
{
    uint64_t flags = irq_save();
    current->state = TASK_ZOMBIE;
    irq_restore(flags);

    schedule();
    for (;;) { }                /* schedule() no vuelve a elegirnos nunca */
}

/* No hay libc: cualquier cosa que parezca de <string.h> hay que escribirla. */
static int kstrlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void kzero(void *dst, uint64_t n)
{
    uint8_t *d = dst;
    while (n--) *d++ = 0;
}

static void kcopy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
}

/* Crea un PROCESO: hilo de kernel + espacio de direcciones propio + una
 * imagen de codigo cargada en el, y arranca en EL0.
 *
 * La diferencia con task_create() esta al final: en vez de aterrizar en
 * ret_from_fork y llamar a una funcion del kernel, fabricamos a mano el
 * trap_frame que kernel_exit espera encontrar, y dejamos que su 'eret'
 * nos deposite en EL0. Para la CPU es indistinguible de volver de una
 * interrupcion que hubiera ocurrido en el primer instante del proceso.
 */
int task_create_user(const char *name, const uint8_t *image, uint64_t size)
{
    uint64_t flags = irq_save();
    struct task *t = 0;

    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_UNUSED) { t = &tasks[i]; break; }
    if (!t) { irq_restore(flags); return -1; }

    uint64_t *pgd = vmm_create_pgd();
    if (!pgd) { irq_restore(flags); return -1; }

    /* --- Codigo: tantas paginas como haga falta, copiadas de la imagen --- */
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t page = pmm_alloc();
        if (!page) { irq_restore(flags); return -1; }

        uint64_t chunk = size - off;
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
        kcopy((void *)page, image + off, chunk);

        vmm_map_in(pgd, USER_BASE + off, page, MM_USER_CODE);
    }

    /* --- Pila de usuario: una pagina justo debajo de USER_STACK_TOP --- */
    uint64_t ustack = pmm_alloc();
    if (!ustack) { irq_restore(flags); return -1; }
    vmm_map_in(pgd, USER_STACK_TOP - PAGE_SIZE, ustack, MM_USER_DATA);

    /* --- Pila de kernel: donde se guardara su contexto en cada syscall --- */
    uint64_t kstack = pmm_alloc();
    if (!kstack) { irq_restore(flags); return -1; }
    *(uint64_t *)kstack = STACK_MAGIC;

    t->stack     = kstack;
    t->pgd       = pgd;
    t->name      = name;
    t->pid       = next_pid++;
    t->counter   = TASK_QUANTUM;
    t->ticks_run = 0;

    /* --- El trap_frame fabricado --- */
    struct trap_frame *tf =
        (struct trap_frame *)(kstack + PAGE_SIZE - sizeof(struct trap_frame));
    kzero(tf, sizeof(*tf));
    tf->elr    = USER_BASE;          /* empezar por el principio del codigo */
    tf->spsr   = 0;                  /* M=0b0000 -> EL0t; DAIF=0 -> IRQ ON  */
    tf->sp_el0 = USER_STACK_TOP;     /* su pila, no la nuestra              */

    kzero(&t->ctx, sizeof(t->ctx));
    t->ctx.pc = (uint64_t)ret_to_user;
    t->ctx.sp = (uint64_t)tf;

    t->state = TASK_READY;

    irq_restore(flags);
    return (int)t->pid;
}

static const char *state_name(uint64_t s)
{
    switch (s) {
    case TASK_READY:    return "listo  ";
    case TASK_RUNNING:  return "activo ";
    case TASK_SLEEPING: return "durmien";
    case TASK_BLOCKED:  return "bloq   ";
    case TASK_ZOMBIE:   return "zombi  ";
    default:            return "libre  ";
    }
}

uint64_t sched_switches(void) { return switches; }

void sched_dump(void)
{
    uint64_t flags = irq_save();

    uart_puts("\n  cambios de contexto: ");
    uart_dec(switches);
    uart_puts("\n  pid  nombre    estado   ticks CPU  pila\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_UNUSED) continue;

        uart_puts("  ");
        uart_dec(t->pid);
        uart_puts(t->pid < 10 ? "    " : "   ");
        uart_puts(t->name);
        for (int n = 0; n < 10 - kstrlen(t->name); n++)
            uart_putc(' ');
        uart_puts(state_name(t->state));
        uart_puts("  ");
        uart_dec(t->ticks_run);

        if (t->stack) {
            int ok = (*(uint64_t *)t->stack == STACK_MAGIC);
            uart_puts(ok ? "         ok" : "         DESBORDADA");
        }
        if (t->pgd) uart_puts("   EL0");
        uart_puts("\n");
    }
    irq_restore(flags);
}
