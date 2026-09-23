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
#include "sync.h"
#include "ipc.h"
#include "elf.h"
#include "smp.h"
#include "spinlock.h"

/* Definidos en switch.S */
void cpu_switch_to(struct task *prev, struct task *next);
void ret_from_fork(void);
void ret_to_user(void);

/* El recolector esta mas abajo; sched_init() lo necesita aqui arriba. */
static void thread_reaper(void *arg);

/* switch.S accede al contexto con offsets desde el principio del struct */
_Static_assert(__builtin_offsetof(struct task, ctx) == 0, "ctx debe ir primero");

/* Las ranuras 0..CORES-1 estan reservadas: son la tarea idle de cada
 * nucleo, el contexto en el que ya estaba cuando arranco. El resto se
 * reparten a quien las pida. */
static struct task tasks[MAX_TASKS];
static uint64_t    next_pid = CORES;
/* Uno por nucleo: que al nucleo 2 se le acabe el turno a su hilo no dice
 * nada de lo que esta haciendo el 3. */
static volatile int need_resched[CORES];

/* Los cuatro nucleos planifican en cuanto esto vale 1, que es cuando el
 * nucleo 0 ha terminado de montar el sistema. */
static volatile int smp_sched_ready;

/* El cerrojo de todo lo de aqui, y tambien de sync.c y de ipc.c. */
static struct spinlock sched_lock = SPINLOCK("sched");

uint64_t sched_lock_irqsave(void)
{
    return spin_lock_irqsave(&sched_lock);
}

void sched_unlock_irqrestore(uint64_t flags)
{
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_unlock_new_task(void)
{
    spin_unlock(&sched_lock);
}

/* Avisar a UN nucleo ocioso de que hay trabajo.
 *
 * "Ocioso" se sabe sin poder leer el TPIDR_EL1 de los demas: la tarea idle
 * del nucleo N solo la ejecuta el nucleo N, asi que verla en RUNNING es
 * verlo a el sin nada que hacer.
 *
 * Se llama con sched_lock cogido, justo despues de dejar alguna tarea
 * lista. Antes esto lo hacia un 'sev', que despertaba a los cuatro nucleos
 * cada vez que alguien soltaba un cerrojo; un toque dirigido cuesta una
 * escritura y no molesta a quien esta trabajando. */
static uint64_t kick_next;          /* por donde empezar a buscar */

void sched_kick_idle(void)
{
    uint64_t yo = this_core();

    /* Empezando cada vez por uno distinto. Buscando siempre desde el 0, el
     * nucleo 1 se llevaba casi todos los avisos -el 0 suele estar
     * ocupado-, y eso es cargarle a uno el trabajo de interrumpirse por
     * los demas. Se ve en la columna de IRQ del comando 'j'. */
    for (uint64_t i = 0; i < CORES; i++) {
        uint64_t c = (kick_next + i) % CORES;
        if (c == yo) continue;
        if (tasks[c].state == TASK_RUNNING) {   /* su idle esta en la CPU */
            kick_next = (c + 1) % CORES;
            irq_send_resched(c);
            return;
        }
    }
}

/* La respuesta al toque: el aviso no lleva contenido, asi que solo hay que
 * apuntar que toca mirar. El sched_preempt() del final de irq_handle() hace
 * el resto. */
void sched_wake_core(void)
{
    need_resched[this_core()] = 1;
}

/* Lo que hace un nucleo cuando no tiene nada que hacer: pararse del todo
 * hasta que alguien le interrumpa. Lo despiertan su propio temporizador
 * (cada 10 ms, como red de seguridad) y el toque de otro nucleo que le haya
 * encontrado trabajo (al instante, que es lo normal). */
void idle_loop(void)
{
    for (;;) {
        __asm__ volatile("wfi");
        schedule();
    }
}

void sched_start_smp(void)
{
    smp_sched_ready = 1;
    __asm__ volatile("dsb sy\n sev" ::: "memory");
}
static uint64_t switches;        /* cambios de contexto totales */

/* --- Pilas de kernel con pagina de guarda ----------------------------
 *
 * La ranura de la tarea decide donde cae su pila, asi que no hace falta
 * llevar ninguna cuenta: la tarea 7 siempre tiene la suya en el mismo
 * sitio, con su guarda debajo.
 *
 * Devuelve la BASE de la pila (la direccion mas baja utilizable). El tope,
 * que es lo que va en SP, es base + PAGE_SIZE. */
static uint64_t kstack_alloc(int ranura)
{
    uint64_t guarda = KSTACK_AREA + (uint64_t)ranura * KSTACK_SLOT;
    uint64_t base   = guarda + PAGE_SIZE;

    uint64_t pa = pmm_alloc();
    if (!pa) return 0;

    if (vmm_map_page(base, pa, MM_RAM_RW) < 0) {
        pmm_free(pa);
        return 0;
    }
    return base;
}

static void kstack_free(int ranura)
{
    uint64_t base = KSTACK_AREA + (uint64_t)ranura * KSTACK_SLOT + PAGE_SIZE;
    vmm_unmap_page(base);            /* y devuelve la pagina fisica */
}

static const char *idle_names[CORES] = { "idle0", "idle1", "idle2", "idle3" };

/* Adoptar el contexto en el que ya esta este nucleo como su tarea idle. No
 * hay que rellenar ctx: se guardara solo la primera vez que ceda la CPU.
 *
 * Lo llaman el nucleo 0 desde sched_init() y cada secundario al llegar a C.
 * Sin esto, 'current' valdria cero en tres de los cuatro nucleos y lo
 * primero que lo mirase se llevaria el sistema por delante. */
void sched_adopt_core(uint64_t core)
{
    struct task *t = &tasks[core];

    t->state   = TASK_RUNNING;
    t->pid     = core;
    t->name    = idle_names[core];
    t->counter = TASK_QUANTUM;
    t->stack   = 0;                    /* usa la pila de arranque del suyo */

    set_this_task(t);
}

void sched_init(void)
{
    sched_adopt_core(0);

    /* El primer hilo del sistema es el que recoge a los muertos. Si no
     * existiera, cada proceso que termina se llevaria su ranura, su pila,
     * sus paginas y su ASID a la tumba. */
    task_create("reaper", thread_reaper, 0);
}

int task_create(const char *name, void (*fn)(void *), void *arg)
{
    uint64_t flags = sched_lock_irqsave();
    struct task *t = 0;

    for (int i = CORES; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) { t = &tasks[i]; break; }
    }
    if (!t) { sched_unlock_irqrestore(flags); return -1; }

    uint64_t stack = kstack_alloc((int)(t - tasks));   /* con su guarda */
    if (!stack) { sched_unlock_irqrestore(flags); return -1; }

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
    sched_kick_idle();                  /* y que alguien la coja ya */

    sched_unlock_irqrestore(flags);
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
        if (idx < CORES) continue;      /* las idle no compiten */
        struct task *t = &tasks[idx];

        /* Solo READY. Antes valia tambien RUNNING, porque RUNNING solo
         * podia significar "la que esta en esta CPU". Con cuatro nucleos
         * significa "corriendo en alguno", y elegirla seria ponerla a
         * ejecutar en dos sitios a la vez, sobre la misma pila. */
        if (t->state == TASK_READY)
            return t;
    }

    /* Nadie mas listo: seguimos con la que hay, si es que sigue queriendo
     * CPU; y si no, la tarea idle de ESTE nucleo, no la del 0. */
    if (current->state == TASK_RUNNING)
        return current;
    return &tasks[this_core()];
}

/* El nucleo del planificador. Se entra CON el cerrojo cogido y se sale con
 * el cogido... pero no necesariamente en manos del mismo hilo.
 *
 * Eso es lo mas raro de este fichero, asi que despacio: el cerrojo NO se
 * suelta al cambiar de contexto, SE PASA DE MANO. Lo cierra el hilo que
 * sale y lo abre el hilo que entra, cuando llegue a su propio
 * sched_unlock_irqrestore() -el de la llamada a schedule() en la que a el
 * lo desalojaron, hace quiza mucho rato-. Durante todo el cambio nadie mas
 * puede mirar la tabla de tareas, que es exactamente lo que hace falta:
 * mientras se cambia, el estado esta a medias.
 *
 * Un cerrojo que cierra uno y abre otro suena a error. Es al reves: es lo
 * unico que funciona. Soltarlo antes del cambio dejaria la tabla a medias a
 * la vista de los otros tres nucleos, y soltarlo despues es imposible,
 * porque despues ya no somos nosotros.
 *
 * El unico que no tiene marco donde soltarlo es un hilo recien nacido, que
 * nunca ha pasado por aqui. De ese se encargan ret_from_fork (switch.S) y
 * ret_to_user (vectors.S) llamando a sched_unlock_new_task().
 */
void schedule_locked(void)
{
    need_resched[this_core()] = 0;

    struct task *prev = current;
    struct task *next = pick_next();

    if (next != prev) {
        if (prev->state == TASK_RUNNING)
            prev->state = TASK_READY;
        next->state   = TASK_RUNNING;
        next->counter = TASK_QUANTUM;
        set_this_task(next);
        switches++;

        /* Cambiar de espacio de direcciones: una escritura a TTBR0 con la
         * tabla y el ASID juntos, sin tocar la TLB. Un hilo de kernel no
         * tiene espacio de usuario, asi que recibe la tabla vacia y el
         * ASID 0. */
        vmm_switch_to(next->pgd ? next->pgd : vmm_empty_pgd(), next->asid);

        cpu_switch_to(prev, next);
        /* --- Cuando la ejecucion vuelve a esta linea han podido pasar
         * horas y haber corrido veinte hilos en cuatro nucleos. Somos otra
         * vez 'prev', y el cerrojo nos lo ha dejado cogido quien nos acaba
         * de devolver la CPU. --- */
    }
}

void schedule(void)
{
    uint64_t flags = sched_lock_irqsave();
    schedule_locked();
    sched_unlock_irqrestore(flags);
}

void scheduler_tick(void)
{
    /* Los despertares van con el reloj del sistema, que lleva el nucleo 0.
     * Que lo recorrieran los cuatro seria hacer cuatro veces el mismo
     * trabajo, y sobre una tabla que aun no tiene cerrojo. */
    if (this_core() == 0) {
        /* Con el cerrojo: los otros tres nucleos estan cambiando estados en
         * esta misma tabla. Recorrerla entera en cada tick es ineficiente;
         * con muchos hilos se usaria una cola ordenada. */
        uint64_t flags = sched_lock_irqsave();
        uint64_t now   = timer_ticks();
        int      algun = 0;

        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].state == TASK_SLEEPING && now >= tasks[i].wake_tick) {
                tasks[i].state = TASK_READY;
                algun = 1;
            }
        }
        if (algun) sched_kick_idle();
        sched_unlock_irqrestore(flags);
    }

    /* Esto si es de cada nucleo: la contabilidad de SU hilo y SU turno. */
    if (!current) return;
    current->ticks_run++;

    /* Una tarea idle no tiene turno que agotar: en cuanto llega un tick,
     * mira si hay trabajo. Un nucleo ocioso que esperase su quantum entero
     * tardaria 50 ms en enterarse de que hay algo que hacer, con los otros
     * tres a tope. (Lo instantaneo seria un IPI: avisar al nucleo ocioso en
     * el momento en que aparece una tarea lista. Eso es el paso siguiente.) */
    if (current->pid < CORES) {
        need_resched[this_core()] = 1;
        return;
    }

    if (current->counter > 0)
        current->counter--;
    if (current->counter == 0)
        need_resched[this_core()] = 1;       /* se le acabo el turno */
}

/* La llama irq_handle() al terminar de atender la interrupcion: es el punto
 * seguro para cambiar de hilo, con el trap_frame ya guardado en la pila. */
void sched_preempt(void)
{
    uint64_t core = this_core();

    /* Los secundarios reciben su tick y lo apuntan, pero no cambian de
     * hilo: sin cerrojo en el planificador, dos nucleos podrian llevarse
     * la misma tarea. Lo enciende el paso siguiente. */
    if (core != 0 && !smp_sched_ready)
        return;

    if (need_resched[core])
        schedule();
}

void task_yield(void)
{
    current->counter = 0;
    schedule();
}

void task_sleep(uint64_t ticks)
{
    /* Marcarse dormido y dormirse de verdad, sin soltar el cerrojo entre
     * una cosa y otra: si lo soltaramos, otro nucleo podria despertarnos en
     * ese hueco y nos dormiriamos despues del despertador. */
    uint64_t flags = sched_lock_irqsave();
    current->wake_tick = timer_ticks() + ticks;
    current->state     = TASK_SLEEPING;
    schedule_locked();
    sched_unlock_irqrestore(flags);
}

/* ====================== EL RECOLECTOR ==============================
 *
 * Un hilo que termina no puede limpiar lo suyo, porque esta corriendo
 * ENCIMA de ello: su pila de kernel es la que tiene bajo los pies y su
 * tabla de traduccion es la que hay puesta en TTBR0 en ese mismo instante.
 * No se puede tirar de la alfombra estando de pie sobre ella.
 *
 * Asi que task_exit() solo hace dos cosas: marcarse zombi y avisar. El
 * entierro lo hace otro hilo, con su propia pila y con la tabla vacia en
 * TTBR0, cuando el muerto ya no se esta ejecutando.
 *
 * Que eso sea seguro descansa en un detalle del planificador: para que el
 * recolector llegue a ejecutarse, el zombi ha tenido que dejar la CPU, y
 * pick_next() no vuelve a elegirlo nunca. Desde ese momento su pila es
 * papel mojado y se puede devolver. (Con varios nucleos no bastaria: el
 * zombi podria seguir corriendo en otro. Ese dia habra que revisarlo.)
 */
static struct waitqueue reaper_wq;

/* Una cola donde espera todo el que quiera enterarse de que alguien ha
 * terminado. Es una sola para todos los procesos: despertar a cuatro
 * esperando para que tres se vuelvan a dormir es mas barato que llevar una
 * lista por pid, con la cantidad de tareas que caben aqui. */
static struct waitqueue exit_wq;
static uint64_t         reaped;

static void reap(struct task *t)
{
    /* Su espacio de direcciones entero: tablas, paginas y el ASID, que
     * ademas limpia de la TLB lo que quedara con esa etiqueta. */
    if (t->pgd)
        vmm_destroy_pgd(t->pgd, t->asid);

    /* Y su pila, que vive en la zona de pilas y no en el mapa lineal: se
     * quita del mapa del kernel y la pagina fisica vuelve sola. */
    if (t->stack)
        kstack_free((int)(t - tasks));

    t->pgd     = 0;
    t->asid    = 0;
    t->stack   = 0;
    t->mmio_va = 0;
    t->name    = "(libre)";
    reaped++;

    /* ESTO, EL ULTIMO. En cuanto la ranura vuelve a UNUSED, task_create()
     * puede darsela a otro; para entonces ya no queda nada por devolver. */
    t->state = TASK_UNUSED;
}

static void thread_reaper(void *arg)
{
    (void)arg;

    for (;;) {
        uint64_t flags = sched_lock_irqsave();

        struct task *dead = 0;
        for (int i = CORES; i < MAX_TASKS; i++)
            if (tasks[i].state == TASK_ZOMBIE) { dead = &tasks[i]; break; }

        if (!dead) {
            /* Buscar y dormirse, sin soltar las IRQ entre una cosa y otra:
             * si las soltaramos, un hilo podria morir justo en medio y su
             * aviso llegaria antes de que estuvieramos en la cola. Nos
             * dormiriamos despues del despertador. */
            wq_wait(&reaper_wq);
            sched_unlock_irqrestore(flags);
            continue;
        }
        sched_unlock_irqrestore(flags);

        /* Fuera de la seccion critica: destruir un espacio de direcciones
         * recorre miles de entradas y no es plan de hacerlo con las
         * interrupciones tapadas. Nadie mas va a tocar a este muerto: el
         * planificador no elige zombis y recolector no hay mas que uno. */
        reap(dead);
    }
}

uint64_t sched_reaped(void) { return reaped; }

/* Buscar una tarea por pid. Con el cerrojo cogido. */
static struct task *by_pid(uint64_t pid)
{
    for (int i = CORES; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid)
            return &tasks[i];
    return 0;
}

/* --- La pila que crece sola -------------------------------------------
 *
 * Hasta aqui, TODO fallo de traduccion en un proceso significaba lo mismo:
 * "ha tocado donde no debia, se muere". Esto cambia esa lectura. Un fallo
 * justo debajo de la pila no es un error, es una PETICION: el proceso
 * necesita mas sitio y la forma de pedirlo es usarlo.
 *
 * Ese cambio de interpretacion es el corazon de la memoria virtual
 * moderna. De aqui salen el mmap, el copy-on-write y el fork: en los tres,
 * el fallo de pagina deja de ser un accidente y pasa a ser el mecanismo.
 *
 * La comprobacion clave es la del puntero de pila. Una pagina se da si la
 * direccion tocada esta POR ENCIMA del SP del proceso: asi es como se ve
 * una pila que crece de verdad, porque el compilador baja SP primero y
 * escribe despues. Un puntero salvaje que apunte mucho mas abajo sigue
 * siendo mortal, que es lo que tiene que ser.
 */
int task_grow_stack(uint64_t direccion, uint64_t sp)
{
    struct task *t = current;
    if (!t || !t->pgd) return 0;

    uint64_t pag = direccion & ~(uint64_t)(PAGE_SIZE - 1);

    /* ¿Cae en la ventana donde la pila puede crecer? */
    if (pag >= t->stack_low)        return 0;   /* ya estaba mapeada */
    if (pag < USER_STACK_MIN)       return 0;   /* se ha pasado del limite */
    if (direccion >= USER_STACK_TOP) return 0;

    /* ¿Y parece una pila, o es un puntero perdido? Se deja un margen
     * pequeño por si el compilador escribe algo antes de terminar de bajar
     * el SP, pero nada de barra libre. */
    if (direccion + 128 < sp) return 0;

    /* Mapear desde donde ya habia hasta donde hace falta. */
    for (uint64_t va = t->stack_low - PAGE_SIZE; va >= pag; va -= PAGE_SIZE) {
        uint64_t pa = pmm_alloc();
        if (!pa) return 0;
        if (vmm_map_in(t->pgd, va, pa, MM_USER_DATA) < 0) {
            pmm_free(pa);
            return 0;
        }
        t->stack_low = va;
        if (va == pag) break;
    }
    return 1;
}

/* Cuantas paginas de pila tiene ahora mismo. */
uint64_t task_stack_pages(struct task *t)
{
    return (USER_STACK_TOP - t->stack_low) / PAGE_SIZE;
}

/* --- El monton de un proceso -----------------------------------------
 *
 * 'brk' es el tope: la primera direccion que el proceso todavia NO tiene.
 * Moverlo hacia arriba es pedir memoria, hacia abajo devolverla, y el
 * valor que se devuelve es el tope VIEJO, que es justo el principio de lo
 * que se acaba de conseguir.
 *
 * Es la interfaz mas tonta que existe para pedir memoria y por eso es la
 * que llevan los Unix desde 1971: el kernel no sabe de bloques ni de
 * listas, solo de "hasta aqui". Repartir ese espacio en trozos es trabajo
 * del proceso, y lo hace user/umalloc.c con el mismo algoritmo que usa el
 * kernel para el suyo. Lo unico que cambia entre los dos asignadores es de
 * donde sale la memoria: uno la pide al gestor de paginas y el otro aqui.
 */
uint64_t task_sbrk(int64_t delta)
{
    struct task *t = current;
    uint64_t viejo = t->brk;

    if (!t->pgd || delta == 0) return viejo;

    uint64_t alineado_viejo = (viejo + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    if (delta > 0) {
        uint64_t nuevo = viejo + (uint64_t)delta;
        if (nuevo < viejo || nuevo > USER_HEAP_MAX) return viejo;  /* no cabe */

        uint64_t alineado_nuevo = (nuevo + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

        for (uint64_t va = alineado_viejo; va < alineado_nuevo; va += PAGE_SIZE) {
            uint64_t pa = pmm_alloc();
            if (!pa) {
                /* Sin memoria a mitad: deshacer lo repartido y no mover el
                 * tope. Media peticion es peor que ninguna, porque el
                 * proceso creeria tener lo que pidio. */
                for (uint64_t v = alineado_viejo; v < va; v += PAGE_SIZE)
                    vmm_unmap_in(t->pgd, v);
                return viejo;
            }
            if (vmm_map_in(t->pgd, va, pa, MM_USER_DATA) < 0) {
                pmm_free(pa);
                for (uint64_t v = alineado_viejo; v < va; v += PAGE_SIZE)
                    vmm_unmap_in(t->pgd, v);
                return viejo;
            }
        }
        t->brk = nuevo;
        return viejo;
    }

    /* Encoger. No se baja del suelo, que es donde acaba el ELF. */
    uint64_t nuevo = viejo - (uint64_t)(-delta);
    if (nuevo > viejo || nuevo < t->brk_base) return viejo;

    uint64_t alineado_nuevo = (nuevo + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t va = alineado_nuevo; va < alineado_viejo; va += PAGE_SIZE)
        vmm_unmap_in(t->pgd, va);

    t->brk = nuevo;
    return viejo;
}

int task_alive(uint64_t pid)
{
    uint64_t flags = sched_lock_irqsave();
    struct task *t = by_pid(pid);
    int vivo = t && t->state != TASK_ZOMBIE;
    sched_unlock_irqrestore(flags);
    return vivo;
}

/* Esperar a que un proceso termine.
 *
 * El bucle vuelve a mirar en cada vuelta, y eso no es desconfianza: puede
 * haber varios esperando y el aviso es para todos, asi que al despertar hay
 * que comprobar si el que nos interesa a nosotros es el que ha muerto.
 *
 * Que la tarea haya desaparecido del todo tambien vale como "termino": el
 * recolector puede haber pasado por ahi antes de que nos despertaramos. */
int task_wait(uint64_t pid)
{
    uint64_t flags = sched_lock_irqsave();

    for (;;) {
        struct task *t = by_pid(pid);
        if (!t || t->state == TASK_ZOMBIE) break;
        wq_wait(&exit_wq);
    }

    sched_unlock_irqrestore(flags);
    return 0;
}

void task_exit(void)
{
    uint64_t flags = sched_lock_irqsave();
    (void)flags;                 /* este cerrojo no lo soltamos nosotros */
    current->state = TASK_ZOMBIE;
    /* Si era un servidor, sus puertos mueren con el. Hay que despertar a
     * quien estuviera esperando o se quedaria bloqueado para siempre
     * esperando a alguien que ya no existe. */
    ipc_release_ports(current->pid);

    /* Avisar a quien nos tiene que enterrar. Lo unico que hace es ponerlo
     * listo; no corre hasta que soltemos la CPU en el schedule() de abajo. */
    wq_wake_one(&reaper_wq);
    wq_wake_all(&exit_wq);       /* y a quien estuviera esperandonos */

    /* Y sin soltar el cerrojo: se lo lleva el hilo que entre. Que siga
     * cogido durante todo el cambio es lo que hace seguro al recolector,
     * porque no podra ni mirar la tabla hasta que hayamos dejado la CPU
     * de verdad. Sin eso liberaria la pila que aun tenemos bajo los pies. */
    schedule_locked();
    for (;;) { }                /* schedule no vuelve a elegirnos nunca */
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
/* Carga [va_ini, va_fin) en el espacio nuevo con los permisos dados.
 *
 * Lo que caiga por debajo de 'copia_hasta' sale de la imagen; lo que quede
 * por encima se queda a cero, que es justo lo que quiere .bss. Y "dejar a
 * cero" aqui no cuesta nada: pmm_alloc entrega las paginas limpias.
 */
/* Cargar un segmento: copiar lo que hay en el fichero y rellenar el resto
 * con ceros. Ese "resto" es .bss, y no hace falta tratarlo aparte: las
 * paginas del PMM ya vienen limpias, asi que no hacer nada ES rellenar
 * con ceros. */
static int load_segment(uint64_t *pgd, const uint8_t *img, uint64_t size,
                        uint64_t vaddr, uint64_t off,
                        uint64_t filesz, uint64_t memsz, uint64_t flags)
{
    for (uint64_t p = 0; p < memsz; p += PAGE_SIZE) {
        uint64_t page = pmm_alloc();
        if (!page) return -1;

        uint64_t n = (p < filesz) ? (filesz - p) : 0;
        if (n > PAGE_SIZE) n = PAGE_SIZE;

        if (n) {
            if (off + p + n > size) return -1;    /* fichero truncado */
            kcopy(phys_to_virt(page), img + off + p, n);
        }

        if (vmm_map_in(pgd, vaddr + p, page, flags) < 0) return -1;
    }
    return 0;
}

/* Leer un ELF y montarlo en un espacio de direcciones nuevo.
 *
 * Lo unico que se mira son los program headers de tipo PT_LOAD: cada uno
 * dice que bytes del fichero van a que direccion, cuanto ocupan de verdad
 * y con que permisos. Todo lo demas del fichero -secciones, simbolos- es
 * para el enlazador y el depurador.
 *
 * La comprobacion es aburrida y es exactamente la que evita que un fichero
 * mal formado (o malicioso) consiga que el kernel mapee donde no debe. */
static int load_elf(uint64_t *pgd, const uint8_t *img, uint64_t size,
                    uint64_t *entry, uint64_t *tope)
{
    if (size < sizeof(struct elf64_ehdr)) return -1;

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)img;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')      return -1;
    if (eh->e_ident[4] != ELF_CLASS64)                        return -1;
    if (eh->e_ident[5] != ELF_DATA_LSB)                       return -1;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_AARCH64) return -1;
    if (eh->e_phentsize != sizeof(struct elf64_phdr))         return -1;
    if (eh->e_phnum == 0 || eh->e_phnum > 8)                  return -1;
    if (eh->e_phoff > size ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr) > size)
        return -1;

    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)(img + eh->e_phoff);

    int      cargados = 0;
    uint64_t fin      = USER_BASE;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;

        uint64_t va  = ph[i].p_vaddr;
        uint64_t mem = ph[i].p_memsz;

        if (ph[i].p_filesz > mem)                 return -1;
        if (va & (PAGE_SIZE - 1))                 return -1;  /* ver user.ld */
        if (va < USER_BASE)                       return -1;
        if (va + mem < va)                        return -1;  /* desbordamiento */
        if (va + mem > USER_STACK_TOP - PAGE_SIZE) return -1;  /* pisaria la pila */

        /* Los permisos salen del fichero, no de un convenio. Si el tramo
         * se escribe, nunca se ejecuta; si no, es codigo de solo lectura.
         * W^X sin tener que pensarlo. */
        uint64_t flags = (ph[i].p_flags & PF_W) ? MM_USER_DATA : MM_USER_CODE;

        if (load_segment(pgd, img, size, va, ph[i].p_offset,
                         ph[i].p_filesz, mem, flags) < 0)
            return -1;
        if (va + mem > fin) fin = va + mem;
        cargados++;
    }

    if (!cargados) return -1;
    if (eh->e_entry < USER_BASE ||
        eh->e_entry >= USER_STACK_TOP - PAGE_SIZE) return -1;

    *entry = eh->e_entry;

    /* El monton del proceso empieza donde acaba su imagen, en la primera
     * frontera de pagina que queda libre. */
    *tope = (fin + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    return 0;
}

/* --- Los argumentos ---------------------------------------------------
 *
 * Un proceso nuevo no tiene forma de saber que se espera de el. Hasta
 * ahora, 'cat' llevaba el nombre del fichero escrito dentro; con esto se
 * le puede decir al arrancarlo.
 *
 * El convenio es el de siempre: x0 = argc, x1 = argv, y argv apunta a un
 * array de punteros terminado en cero. Todo eso vive en la pila del
 * proceso, que es el unico sitio que ya es suyo y donde se puede escribir
 * antes de que exista.
 *
 * Se escribe por el mapa lineal del kernel, no por la direccion de
 * usuario: esa pagina todavia no esta en ningun TTBR0 activo.
 */
#define MAX_ARGS   8
#define ARGS_BYTES 128

static void build_args(uint64_t ustack_pa, const char *args,
                       uint64_t *argc_out, uint64_t *argv_out, uint64_t *sp_out)
{
    char    *k    = (char *)phys_to_virt(ustack_pa);   /* la pagina, en kernel */
    uint64_t base = USER_STACK_TOP - PAGE_SIZE;        /* la misma, en usuario */

    /* 1. La cadena, arriba del todo. */
    uint64_t len = 0;
    if (args) while (args[len] && len < ARGS_BYTES - 1) len++;

    uint64_t o_str = PAGE_SIZE - (len + 1);
    for (uint64_t i = 0; i < len; i++) k[o_str + i] = args[i];
    k[o_str + len] = 0;

    /* 2. Partirla por los espacios, ahi mismo. Cada palabra queda como una
     *    cadena independiente porque el separador pasa a ser un cero. */
    uint64_t off[MAX_ARGS];
    uint64_t argc = 0;
    int      dentro = 0;

    for (uint64_t i = 0; i <= len; i++) {
        char c = k[o_str + i];
        if (c == ' ' || c == 0) {
            k[o_str + i] = 0;
            dentro = 0;
        } else if (!dentro) {
            dentro = 1;
            if (argc < MAX_ARGS) off[argc++] = o_str + i;
        }
    }

    /* 3. El array de punteros, debajo, y alineado a 16 porque el ABI de
     *    AArch64 exige que la pila lo este. */
    uint64_t o_argv = (o_str - (argc + 1) * 8) & ~15UL;
    uint64_t *argv  = (uint64_t *)(k + o_argv);

    for (uint64_t i = 0; i < argc; i++)
        argv[i] = base + off[i];
    argv[argc] = 0;                       /* el cero final del convenio */

    *argc_out = argc;
    *argv_out = base + o_argv;
    *sp_out   = base + o_argv;
}

/* Como se llama un proceso que arranca otro proceso.
 *
 * El nombre no puede ser un puntero a la memoria del que llama: ese espacio
 * de direcciones puede desaparecer antes que la tarea. Se copia la primera
 * palabra de sus argumentos, que es justo como se llama a si mismo. */
static void nombre_de_args(struct task *t, const char *args)
{
    uint64_t o = 0;
    if (args) {
        while (*args == ' ') args++;
        while (*args && *args != ' ' && o < sizeof(t->namebuf) - 1)
            t->namebuf[o++] = *args++;
    }
    if (!o) { t->namebuf[o++] = '?'; }
    t->namebuf[o] = 0;
    t->name = t->namebuf;
}

int task_create_user(const char *name, const uint8_t *image, uint64_t size,
                     uint64_t mmio_pa, const char *args)
{
    /* Coger una ranura y soltar el cerrojo enseguida.
     *
     * Cargar un proceso es copiar paginas y construir tablas: milisegundos.
     * Hacerlo con sched_lock cogido dejaria a los otros tres nucleos
     * parados todo ese rato, y no hace falta: en cuanto la ranura esta
     * reservada, nadie mas la va a tocar.
     *
     * TASK_BLOCKED es la reserva. No es UNUSED, asi que no se la lleva otro
     * task_create; no es READY ni RUNNING, asi que el planificador no la
     * elige; y no es ZOMBIE, asi que el recolector la ignora. */
    uint64_t flags = sched_lock_irqsave();
    struct task *t = 0;

    for (int i = CORES; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_UNUSED) { t = &tasks[i]; break; }
    if (!t) { sched_unlock_irqrestore(flags); return -1; }

    t->state     = TASK_BLOCKED;
    t->name      = "(cargando)";
    t->stack     = 0;
    t->pgd       = 0;
    t->ticks_run = 0;
    sched_unlock_irqrestore(flags);

    uint64_t asid = 0;
    uint64_t *pgd = vmm_create_pgd(&asid);
    if (!pgd) {
        flags = sched_lock_irqsave();
        t->state = TASK_UNUSED;          /* devolver la ranura */
        sched_unlock_irqrestore(flags);
        return -1;
    }

    uint64_t kstack    = 0;
    uint64_t entry     = 0;
    uint64_t tope      = USER_BASE;

    if (load_elf(pgd, image, size, &entry, &tope) < 0)
        goto fail;

    /* --- Pila de usuario: una pagina justo debajo de USER_STACK_TOP --- */
    uint64_t ustack = pmm_alloc();
    if (!ustack) goto fail;

    uint64_t argc = 0, argv = 0, sp = USER_STACK_TOP;
    build_args(ustack, args, &argc, &argv, &sp);

    if (vmm_map_in(pgd, USER_STACK_TOP - PAGE_SIZE, ustack, MM_USER_DATA) < 0)
        goto fail;

    /* --- MMIO concedido: asi un driver puede vivir en EL0 ---------------
     * Le mapeamos la pagina de registros del periferico en su espacio, como
     * memoria Device y accesible desde EL0. A partir de ahi el driver habla
     * con el hardware sin pasar por el kernel ni una sola vez. */
    t->mmio_va = 0;
    if (mmio_pa) {
        if (vmm_map_in(pgd, USER_MMIO_BASE, mmio_pa & ~(PAGE_SIZE - 1),
                       MM_DEVICE | PTE_AP_RW_ALL | PTE_nG) < 0)
            goto fail;
        t->mmio_va = USER_MMIO_BASE | (mmio_pa & (PAGE_SIZE - 1));
    }

    /* --- Pila de kernel: donde se guardara su contexto en cada syscall --- */
    kstack = kstack_alloc((int)(t - tasks));
    if (!kstack) goto fail;
    *(uint64_t *)kstack = STACK_MAGIC;

    /* Y ahora si, publicar: el cerrojo vuelve solo para el momento en que
     * esta tarea pasa a existir para los demas. */
    flags = sched_lock_irqsave();

    t->stack     = kstack;
    t->pgd       = pgd;
    t->asid      = asid;
    t->brk_base  = tope;
    t->brk       = tope;
    t->stack_low = USER_STACK_TOP - PAGE_SIZE;
    if (name) t->name = name;
    else      nombre_de_args(t, args);
    t->pid       = next_pid++;
    t->counter   = TASK_QUANTUM;
    t->ticks_run = 0;

    /* --- El trap_frame fabricado --- */
    struct trap_frame *tf =
        (struct trap_frame *)(kstack + PAGE_SIZE - sizeof(struct trap_frame));
    kzero(tf, sizeof(*tf));
    tf->elr    = entry;              /* lo dice el ELF                      */
    tf->spsr   = 0;                  /* M=0b0000 -> EL0t; DAIF=0 -> IRQ ON  */
    tf->sp_el0 = sp;                 /* su pila, con argv ya puesto encima  */
    tf->x[0]   = argc;
    tf->x[1]   = argv;

    kzero(&t->ctx, sizeof(t->ctx));
    t->ctx.pc = (uint64_t)ret_to_user;
    t->ctx.sp = (uint64_t)tf;

    t->state = TASK_READY;
    sched_kick_idle();

    sched_unlock_irqrestore(flags);
    return (int)t->pid;

fail:
    /* Media carga es peor que ninguna: se devuelve todo lo repartido. */
    if (kstack) kstack_free((int)(t - tasks));
    vmm_destroy_pgd(pgd, asid);

    flags = sched_lock_irqsave();
    t->state = TASK_UNUSED;              /* y la ranura reservada */
    sched_unlock_irqrestore(flags);
    return -1;
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
    uint64_t flags = sched_lock_irqsave();

    uart_puts("\n  cambios de contexto: ");
    uart_dec(switches);
    uart_puts("   tareas recogidas: ");
    uart_dec(reaped);
    uart_puts("\n  pid  nombre    estado   ticks CPU  pila\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_UNUSED) continue;

        uint64_t lf = uart_begin();     /* la fila entera, de una pieza */
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
        if (t->pgd) {
            uart_puts("   EL0 asid ");
            uart_dec(t->asid);
            uart_puts("  pila ");
            uart_dec(task_stack_pages(t));
            uart_puts("p");
        }
        uart_puts("\n");
        uart_end(lf);
    }
    sched_unlock_irqrestore(flags);
}
