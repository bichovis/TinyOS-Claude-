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
#include "fpu.h"
#include "irq.h"
#include "timer.h"
#include "uart.h"
#include "exception.h"
#include "sync.h"
#include "ipc.h"
#include "fs_abi.h"
#include "mmio.h"
#include "elf.h"
#include "smp.h"
#include "spinlock.h"

/* Definidos en switch.S */
void cpu_switch_to(struct task *prev, struct task *next);
void ret_from_fork(void);
void ret_to_user(void);

/* El recolector esta mas abajo; sched_init() lo necesita aqui arriba. */
static void thread_reaper(void *arg);
static struct task *by_pid(uint64_t pid);
static void fd_cerrar_todos(struct task *t);

/* switch.S accede al contexto con offsets desde el principio del struct */
_Static_assert(__builtin_offsetof(struct task, ctx) == 0, "ctx debe ir primero");

/* Las ranuras 0..CORES-1 estan reservadas: son la tarea idle de cada
 * nucleo, el contexto en el que ya estaba cuando arranco. El resto se
 * reparten a quien las pida. */
static struct task tasks[MAX_TASKS];
static uint64_t    next_pid = CORES;

static void mapeos_limpiar(struct task *t);

/* Cuantos hilos han existido. Sirve para poner en contexto cuantos han
 * llegado a pedir la FPU: sin el denominador, el numero no dice nada. */
uint64_t task_creados(void) { return next_pid - CORES; }
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
static void kstack_free(int ranura);

static uint64_t kstack_alloc(int ranura)
{
    uint64_t guarda = KSTACK_AREA + (uint64_t)ranura * KSTACK_SLOT;
    uint64_t base   = guarda + PAGE_SIZE;

    for (int i = 0; i < KSTACK_PAGINAS; i++) {
        uint64_t pa = pmm_alloc();
        if (!pa) { kstack_free(ranura); return 0; }

        if (vmm_map_page(base + (uint64_t)i * PAGE_SIZE, pa, MM_RAM_RW) < 0) {
            pmm_free(pa);
            kstack_free(ranura);
            return 0;
        }
    }
    return base;
}

static void kstack_free(int ranura)
{
    uint64_t base = KSTACK_AREA + (uint64_t)ranura * KSTACK_SLOT + PAGE_SIZE;

    /* Todas las paginas de la pila, no solo la primera. vmm_unmap_page
     * devuelve -1 en las que no estaban mapeadas, y eso pasa cuando se
     * llama a medio reservar; da igual, es el caso que se quiere. */
    for (int i = 0; i < KSTACK_PAGINAS; i++)
        vmm_unmap_page(base + (uint64_t)i * PAGE_SIZE);
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

        /* Si el que se va tenia la FPU encendida, guardarla y apagarla. La
         * mayoria de los hilos no la ha encendido nunca y esto es una
         * comparacion y ya. El que entra NO la recibe: si la quiere,
         * atrapara y se le dara entonces. */
        fp_switch_out(prev);

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
    fd_cerrar_todos(t);

    if (t->stack)
        kstack_free((int)(t - tasks));

    fp_release(t);              /* sus 528 bytes, si llego a necesitarlos */
    mapeos_limpiar(t);

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

        /* Un zombi con padre vivo NO se toca: existe para que ese padre
         * pueda leer su codigo de salida. Cuando el padre lo recoge -o se
         * muere sin hacerlo- deja de tener padre y entonces es nuestro. */
        struct task *dead = 0;
        for (int i = CORES; i < MAX_TASKS; i++) {
            if (tasks[i].state != TASK_ZOMBIE) continue;
            uint64_t p = tasks[i].parent;
            if (p && by_pid(p)) continue;         /* su padre sigue ahi */
            dead = &tasks[i];
            break;
        }

        if (!dead) {
            /* Buscar y dormirse, sin soltar las IRQ entre una cosa y otra:
             * si las soltaramos, un hilo podria morir justo en medio y su
             * aviso llegaria antes de que estuvieramos en la cola. Nos
             * dormiriamos despues del despertador. */
            wq_wait_uninterruptible(&reaper_wq);
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
/* ====================== FICHEROS MAPEADOS ==========================
 *
 * mmap no lee nada. Reserva un tramo de direcciones, apunta de que fichero
 * viene, y se va. La primera vez que el proceso toca una de esas paginas
 * salta un fallo de traduccion, y AHI se trae el trozo que hace falta.
 *
 * Eso ya lo sabiamos hacer: es lo mismo que la pila que crece y que
 * copy-on-write. Lo nuevo, y es gordo, es DE DONDE sale el contenido.
 *
 * Hasta hoy, cuando el kernel necesitaba una pagina se la pedia al gestor
 * de paginas, que es codigo suyo, en su mismo nivel de privilegio, y que
 * contesta siempre. Ahora se la pide a un PROCESO DE EL0: manda un mensaje
 * al servidor de ficheros y se duerme hasta que conteste. El kernel, a
 * mitad de una instruccion que todavia no ha terminado de ejecutarse,
 * esperando a un programa sin privilegios.
 *
 * Esa inversion es la idea entera del microkernel llevada hasta el final.
 * Y funciona porque el hilo que falla no es "el kernel": es un hilo
 * normal, con su pila y su entrada en la tabla de tareas, que resulta
 * estar en modo privilegiado. Puede dormirse como cualquier otro, y
 * mientras tanto los demas siguen corriendo. Si el servidor no esta, la
 * peticion falla, el proceso muere, y el sistema sigue.
 *
 * Lo que NO se puede hacer es que el propio servidor de ficheros mapee un
 * fichero: se estaria esperando a si mismo. No hay nada que lo impida por
 * ahora, y esta en las limitaciones.
 */
/* Donde acaba un mapeo, con su pagina de hueco detras. Ese hueco es
 * barato y evita que un desbordado de un tramo aterrice en el siguiente. */
static uint64_t fin_de(const struct mapeo *m)
{
    return m->base + ((m->len + PAGE_SIZE - 1) / PAGE_SIZE + 1) * PAGE_SIZE;
}

/* Buscar ranura en la tabla y sitio en el mapa, REUTILIZANDO huecos.
 *
 * La primera version ponia cada tramo detras del ultimo y nunca miraba
 * atras. Es mas simple y funciona... hasta que alguien reserva y suelta
 * muchas veces: cada vuelta consume direcciones que ya no vuelven, y a los
 * 128 MB de la zona se acaba el sitio aunque no haya nada mapeado.
 *
 * Un compilador hace exactamente eso miles de veces. Asi que se busca el
 * primer hueco que valga, empezando por abajo. Con cuatro mapeos por
 * proceso el bucle es de risa; con muchos habria que ordenarlos. */
static struct mapeo *reservar_tramo(struct task *t, uint64_t paginas)
{
    int hueco = -1;
    for (int i = 0; i < MAX_MAPEOS; i++)
        if (!t->mapeos[i].base) { hueco = i; break; }

    if (hueco < 0) return 0;                   /* ya tiene cuatro */

    uint64_t bytes = paginas * PAGE_SIZE;
    uint64_t donde = USER_MMAP_BASE;

    /* Empujar hacia arriba mientras choque con alguno. Como cada empujon
     * salta por encima de un tramo entero, esto termina: hay cuatro. */
    for (int vueltas = 0; vueltas <= MAX_MAPEOS; vueltas++) {
        int choca = 0;

        for (int i = 0; i < MAX_MAPEOS; i++) {
            struct mapeo *m = &t->mapeos[i];
            if (!m->base) continue;

            if (donde < fin_de(m) && m->base < donde + bytes) {
                donde = fin_de(m);
                choca = 1;
            }
        }

        if (!choca) {
            if (donde + bytes > USER_MMAP_MAX) return 0;
            t->mapeos[hueco].base = donde;
            return &t->mapeos[hueco];
        }
    }
    return 0;
}

int64_t task_mmap(const char *ruta, uint64_t *tam)
{
    struct task *t = current;
    if (!t || !t->pgd) return -1;

    /* El servidor de ficheros NO puede mapear ficheros.
     *
     * Rellenar una pagina mapeada es mandarle un mensaje al servidor y
     * dormirse hasta que conteste. Si el que se duerme ES el servidor, no
     * queda nadie para contestarle: se espera a si mismo, para siempre, y
     * con el se cuelga todo el que quiera leer algo.
     *
     * Es la pega de fondo de invertir la dependencia -el kernel esperando
     * a un proceso- y la unica forma honesta de tratarla es nombrarla.
     * Aqui se corta en seco: mejor un mmap que devuelve -1 que un sistema
     * que se para sin decir por que. */
    if (t->pid == port_owner(PORT_FILES)) return -1;

    int64_t largo = fs_tamano(ruta);
    if (largo < 0) return -1;

    uint64_t paginas = ((uint64_t)largo + PAGE_SIZE - 1) / PAGE_SIZE;
    if (paginas == 0) paginas = 1;            /* un fichero vacio, una pagina */

    struct mapeo *m = reservar_tramo(t, paginas);
    if (!m) return -1;

    m->len     = (uint64_t)largo;
    m->anonimo = 0;
    for (int i = 0; i < FS_PATH_MAX; i++) m->ruta[i] = ruta[i];

    if (tam) *tam = m->len;
    return (int64_t)m->base;
}

/* Memoria nueva, a cero, escribible, y traida segun se toque.
 *
 * Es el mismo mecanismo que mapear un fichero, con la pagina saliendo del
 * gestor de paginas en vez del servidor. Y eso lo hace MUCHO mas barato:
 * no hay mensaje, no hay espera, no hay nadie de quien depender. Reservar
 * cien megas cuesta lo mismo que reservar uno -nada- y solo se paga por
 * las paginas que se tocan.
 *
 * Frente a sbrk, que mueve un solo tope: aqui cada tramo va por su cuenta
 * y se suelta entero cuando sobra, sin esperar a que se vacie lo que
 * tiene encima. */
int64_t task_mmap_anon(uint64_t bytes)
{
    struct task *t = current;
    if (!t || !t->pgd || !bytes) return -1;

    uint64_t paginas = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    struct mapeo *m = reservar_tramo(t, paginas);
    if (!m) return -1;

    m->len     = bytes;
    m->anonimo = 1;
    m->ruta[0] = 0;
    return (int64_t)m->base;
}

/* El fallo de pagina de un fichero mapeado. Devuelve 1 si lo ha resuelto. */
int task_mmap_fault(uint64_t direccion)
{
    struct task *t = current;
    if (!t || !t->pgd) return 0;
    if (direccion < USER_MMAP_BASE || direccion >= USER_MMAP_MAX) return 0;

    uint64_t pag = direccion & ~(uint64_t)(PAGE_SIZE - 1);

    struct mapeo *m = 0;
    for (int i = 0; i < MAX_MAPEOS; i++) {
        struct mapeo *c = &t->mapeos[i];
        if (!c->base) continue;
        uint64_t fin = c->base + ((c->len + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
        if (pag >= c->base && pag < fin) { m = c; break; }
    }
    if (!m) return 0;                          /* ahi no hay nada mapeado */

    uint64_t pa = pmm_alloc();
    if (!pa) return 0;

    /* Rellenarla ANTES de mapearla. Si se mapeara primero, el proceso
     * podria ver la pagina a medio llenar: aqui hay cuatro nucleos, y el
     * fichero lo trae un servidor que tarda. */
    char *dst = (char *)phys_to_virt(pa);
    for (uint64_t i = 0; i < PAGE_SIZE; i++) dst[i] = 0;

    if (m->anonimo) {
        /* Memoria y no fichero: la pagina ya esta a cero, que es todo lo
         * que hacia falta. Ni mensaje, ni espera, ni nadie de quien
         * depender. */
        if (vmm_map_in(t->pgd, pag, pa, MM_USER_DATA) < 0) {
            pmm_free(pa);
            return 0;
        }
        return 1;
    }

    uint64_t off = pag - m->base;
    uint64_t pedir = m->len > off ? m->len - off : 0;
    if (pedir > PAGE_SIZE) pedir = PAGE_SIZE;

    if (pedir && fs_leer_en(m->ruta, off, dst, pedir) < 0) {
        pmm_free(pa);
        return 0;
    }

    /* El ultimo trozo del fichero no llena la pagina, y lo que sobra se
     * queda a cero. Es lo que hace cualquier Unix, y es lo que permite
     * tratar el final sin contar bytes a mano. */
    if (vmm_map_in(t->pgd, pag, pa, MM_USER_RO) < 0) {
        pmm_free(pa);
        return 0;
    }
    return 1;
}

/* Soltar un mapeo: quitar del mapa las paginas que se llegaron a traer y
 * devolver la ranura.
 *
 * Hace falta de verdad y no por elegancia. Un proceso tiene cuatro
 * ranuras, y el shell mapea un fichero por cada orden que ejecuta: sin
 * esto, a la quinta orden se queda sin sitio. */
int task_munmap(uint64_t base)
{
    struct task *t = current;
    if (!t || !t->pgd) return -1;

    for (int i = 0; i < MAX_MAPEOS; i++) {
        struct mapeo *m = &t->mapeos[i];
        if (m->base != base || !m->base) continue;

        uint64_t paginas = (m->len + PAGE_SIZE - 1) / PAGE_SIZE;
        if (paginas == 0) paginas = 1;

        /* vmm_unmap_in devuelve la pagina fisica al gestor, que lleva
         * cuentas: si el fork la dejo compartida, solo baja el contador. Y
         * las que nunca se llegaron a tocar no estan mapeadas, asi que
         * unmap dice -1 y no pasa nada. */
        for (uint64_t p = 0; p < paginas; p++)
            vmm_unmap_in(t->pgd, m->base + p * PAGE_SIZE);

        m->base = 0;
        return 0;
    }
    return -1;
}

static void mapeos_limpiar(struct task *t)
{
    for (int i = 0; i < MAX_MAPEOS; i++) t->mapeos[i].base = 0;
}

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

/* --- Descriptores de fichero ------------------------------------------
 *
 * Una tabla pequenya por proceso. Lo unico que hace es dar un numero a
 * cada cosa abierta, y ese numero es lo que permite que quien arranca un
 * programa decida a donde va su salida sin que el programa se entere.
 */
struct fichero *task_fd(int fd)
{
    if (!current || fd < 0 || fd >= MAX_FD) return 0;
    return current->fd[fd];
}

int task_fd_alloc(struct fichero *f)
{
    if (!current) return -1;
    for (int i = 0; i < MAX_FD; i++)
        if (!current->fd[i]) { current->fd[i] = f; return i; }
    return -1;
}

int task_fd_close(int fd)
{
    struct fichero *f = task_fd(fd);
    if (!f) return -1;
    current->fd[fd] = 0;
    file_close(f);
    return 0;
}

/* Poner algo en un descriptor concreto, cerrando lo que hubiera. Es la
 * pieza que hace posible una tuberia: el hijo pone el extremo de escritura
 * en el 1, y a partir de ahi todo lo que "imprima" va a la tuberia. */
int task_fd_dup2(int viejo, int nuevo)
{
    struct fichero *f = task_fd(viejo);
    if (!f || nuevo < 0 || nuevo >= MAX_FD) return -1;
    if (viejo == nuevo) return nuevo;

    if (current->fd[nuevo]) file_close(current->fd[nuevo]);
    file_dup(f);
    current->fd[nuevo] = f;
    return nuevo;
}

static void fd_heredar(struct task *hijo, struct task *padre)
{
    for (int i = 0; i < MAX_FD; i++) {
        hijo->fd[i] = padre->fd[i];
        if (hijo->fd[i]) file_dup(hijo->fd[i]);
    }
}

static void fd_cerrar_todos(struct task *t)
{
    for (int i = 0; i < MAX_FD; i++) {
        if (t->fd[i]) file_close(t->fd[i]);
        t->fd[i] = 0;
    }
}

/* ====================== SENYALES ===================================
 *
 * Una senyal es un bit. Todo lo demas -cuando se mira, que se hace con el,
 * como se le cuenta al proceso- es politica que decide el kernel.
 *
 * El momento en que se miran no es casual: JUSTO ANTES de volver a EL0, y
 * en ningun otro sitio. Antes no se puede, porque el proceso esta a medias
 * de una llamada al sistema y su estado no es coherente; despues no hay
 * ocasion, porque ya se ha ido.
 */
static volatile uint64_t consola_pgid;     /* que GRUPO esta en primer plano */

static void kcopy(void *dst, const void *src, uint64_t n);   /* mas abajo */

/* Las que detienen. Se repiten en varios sitios y una lista suelta se
 * desincroniza, asi que va una vez. */
#define ES_PARADA(s) ((s) == SIGSTOP || (s) == SIGTSTP || \
                      (s) == SIGTTIN || (s) == SIGTTOU)

int task_signal(uint64_t pid, int sig)
{
    if (sig <= 0 || sig >= SIG_MAX) return -1;

    uint64_t flags = sched_lock_irqsave();
    struct task *t = by_pid(pid);
    int ok = 0;

    if (t && t->state != TASK_ZOMBIE && t->pgd) {
        /* Parar y seguir se ANULAN entre si, y hay que hacerlo al
         * apuntarlas y no al entregarlas.
         *
         * Si llegan un Ctrl-Z y un SIGCONT casi a la vez y las dos se
         * quedan apuntadas, el proceso arranca y se vuelve a parar, o al
         * reves, segun el orden en que se miren los bits. Ninguno de los
         * dos resultados es el que pidio nadie: lo que pidio el ultimo en
         * hablar es lo que vale, y para eso la que llega borra a su
         * contraria. */
        if (sig == SIGCONT) {
            t->sig_pending &= ~((1u << SIGSTOP) | (1u << SIGTSTP) |
                                (1u << SIGTTIN) | (1u << SIGTTOU));
        } else if (ES_PARADA(sig)) {
            t->sig_pending &= ~(1u << SIGCONT);
        }

        t->sig_pending |= 1u << sig;

        /* Un proceso detenido no vuelve a EL0, asi que NO pasa por
         * signal_deliver: apuntarle un bit ahi no sirve de nada. Lo que lo
         * saca de ahi tiene que actuar aqui.
         *
         * SIGCONT es la que existe para eso. Pero SIGKILL tambien tiene
         * que hacerlo, y esto es facil de olvidar: sin ello, un proceso
         * parado seria inmatable -la senyal fulminante se quedaria
         * apuntada en la libreta de alguien que no va a leerla nunca- y
         * habria que despertarlo para poder matarlo. Asi que se le
         * devuelve la CPU lo justo para que se muera. */
        if (t->state == TASK_STOPPED && (sig == SIGCONT || sig == SIGKILL)) {
            t->state = TASK_READY;
            sched_kick_idle();
            wq_wake_all(&exit_wq);       /* el padre puede estar en waitpid */
        }

        /* Y despertarlo, porque una senyal apuntada en la libreta de
         * alguien que duerme no sirve de nada hasta que se despierte.
         *
         * Si estaba BLOQUEADO esperando algo, hay que sacarlo de la cola a
         * mano y marcarle que lo ha despertado una senyal, no el aviso que
         * esperaba: su llamada al sistema tiene que volver diciendo que la
         * interrumpieron, no fingir que la condicion se cumplio. */
        if (t->state == TASK_STOPPED) {
            /* Detenido: la senyal espera a que lo reanuden. No se le
             * despierta, que es justo lo que significa estar parado. */
        } else if (t->state == TASK_SLEEPING) {
            t->state = TASK_READY;
        } else if (t->state == TASK_BLOCKED) {
            t->interrumpido = 1;
            wq_remove(t);
            t->state = TASK_READY;
        }
        sched_kick_idle();
        ok = 1;
    }

    sched_unlock_irqrestore(flags);
    return ok ? 0 : -1;
}

int task_set_handler(int sig, uint64_t manejador, uint64_t trampolin)
{
    if (!current || !current->pgd)       return -1;
    if (sig <= 0 || sig >= SIG_MAX)      return -1;
    if (sig == SIGKILL)                  return -1;   /* esa no se atrapa */

    current->sig_handler[sig] = manejador;
    if (trampolin) current->sig_tramp = trampolin;
    return 0;
}

void task_set_console(uint64_t pgid) { consola_pgid = pgid; }
uint64_t task_console_pid(void)       { return consola_pgid; }

/* Meter un proceso en un grupo.
 *
 * Solo se puede mover a uno mismo o a un hijo, y solo mientras el hijo no
 * se haya convertido ya en otro programa. Esa segunda regla es la de Unix
 * y no es burocracia: en cuanto el hijo hace exec, el shell ya no sabe
 * que esta ejecutando ahi dentro, y mover de trabajo a un programa que ya
 * corre es cambiarle el suelo de sitio.
 *
 * Aqui la simplificamos a "a uno mismo o a un hijo", porque sin
 * close-on-exec ni sesiones no hay forma de distinguir el antes del
 * despues sin apuntar una bandera mas. */
int task_set_pgid(uint64_t pid, uint64_t pgid)
{
    if (!current) return -ESRCH;
    if (!pid)  pid  = current->pid;
    if (!pgid) pgid = pid;

    uint64_t flags = sched_lock_irqsave();

    struct task *t = by_pid(pid);
    int r = 0;

    if (!t || !t->pgd)                              r = -ESRCH;
    else if (t != current && t->parent != current->pid) r = -EPERM;
    else t->pgid = pgid;

    sched_unlock_irqrestore(flags);
    return r;
}

/* Poner un grupo en primer plano.
 *
 * "Solo init" era una frontera de privilegio, pero no la correcta: quien
 * tiene que ceder la consola al trabajo que acaba de arrancar es el
 * shell, y el shell no es init. Y la regla de verdad no habla de quien
 * eres sino de que llevas, como el testigo de una carrera:
 *
 *   1. init siempre, porque es quien la reparte cuando no queda nadie.
 *   2. Si tu grupo la tiene ahora mismo, puedes pasarla.
 *   3. Si la tiene un grupo que formaron hijos tuyos, puedes recuperarla.
 *      Es lo que hace un shell cuando el trabajo que puso delante termina:
 *      se la habia prestado, y se la devuelve a si mismo.
 *   4. Si no la tiene nadie vivo, que se la quede quien la pida. El grupo
 *      al que se le dio se ha muerto entero.
 *
 * Lo que la regla impide es lo que tiene que impedir: que un proceso de
 * segundo plano se ponga delante por su cuenta y se quede con el teclado
 * del que esta sentado ahi. */
int task_dar_consola(uint64_t pgid)
{
    if (!current) return -EPERM;
    if (current->pid == task_init_pid()) { task_set_console(pgid); return 0; }

    uint64_t flags = sched_lock_irqsave();
    uint64_t tiene = consola_pgid;

    int mio = (current->pgid == tiene);
    int de_un_hijo = 0, vivo = 0;

    if (!mio && tiene)
        for (int i = CORES; i < MAX_TASKS; i++) {
            struct task *t = &tasks[i];
            if (t->state == TASK_UNUSED || !t->pgd || t->state == TASK_ZOMBIE) continue;
            if (t->pgid != tiene) continue;
            vivo = 1;
            if (t->parent == current->pid) { de_un_hijo = 1; break; }
        }

    int ok = mio || de_un_hijo || !vivo;
    if (ok) consola_pgid = pgid;

    sched_unlock_irqrestore(flags);
    return ok ? 0 : -EPERM;
}

int64_t task_get_pgid(uint64_t pid)
{
    uint64_t flags = sched_lock_irqsave();
    struct task *t = pid ? by_pid(pid) : current;
    int64_t r = (t && t->pgd) ? (int64_t)t->pgid : -ESRCH;
    sched_unlock_irqrestore(flags);
    return r;
}

/* --- El primer proceso ------------------------------------------------
 *
 * El kernel arranca UNO, y a partir de ahi todo lo demas lo arranca ese.
 * No es una regla de estilo: es que no hay otra forma de llegar a EL0, y
 * en cuanto hay una forma conviene que sea una sola.
 *
 * A init se le conceden dos cosas que a nadie mas: arrancar los programas
 * que el kernel lleva dentro -con el dispositivo que necesiten- y decir
 * quien manda en la consola. Ese "a nadie mas" es la unica frontera de
 * privilegio que hay entre procesos en este sistema. */
static uint64_t init_pid;

uint64_t task_init_pid(void)            { return init_pid; }
void     task_set_init_pid(uint64_t p)  { init_pid = p; }

/* Los programas que viajan dentro del kernel. Son los justos para
 * arrancar: los dos drivers y el interprete. Lo demas ya se lee de la
 * tarjeta, que para eso esta el servidor de ficheros. */
extern const uint8_t  user_conserver[];  extern const uint64_t user_conserver_size;
extern const uint8_t  user_fs[];         extern const uint64_t user_fs_size;
extern const uint8_t  user_sh[];         extern const uint64_t user_sh_size;
extern const uint8_t  user_init[];       extern const uint64_t user_init_size;

struct empotrado {
    const char     *nombre;
    const uint8_t  *imagen;
    const uint64_t *tam;
};

static const struct empotrado empotrados[] = {
    { "conserver", user_conserver, &user_conserver_size },
    { "fs",        user_fs,        &user_fs_size        },
    { "sh",        user_sh,        &user_sh_size        },
    { "init",      user_init,      &user_init_size      },
    { 0, 0, 0 }
};

/* De un numero de dispositivo a una direccion fisica.
 *
 * Aqui esta la lista entera de lo que se puede conceder, y esta EN EL
 * KERNEL. Un proceso pide "la UART", no una direccion: no puede inventarse
 * uno que no este en esta tabla ni pedir la pagina de al lado. */
static uint64_t dispositivo_a_fisica(uint64_t dev)
{
    switch (dev) {
    case DEV_UART: return UART0_PHYS;
    case DEV_EMMC: return EMMC_PHYS;
    default:       return 0;
    }
}

int task_bootstrap(const char *nombre, const struct args *args,
                   const struct args *entorno, uint64_t dispositivo)
{
    for (const struct empotrado *e = empotrados; e->nombre; e++) {
        int igual = 1;
        for (int i = 0; ; i++) {
            if (e->nombre[i] != nombre[i]) { igual = 0; break; }
            if (!nombre[i]) break;
        }
        if (!igual) continue;

        return task_create_user(e->nombre, e->imagen, *e->tam,
                                dispositivo_a_fisica(dispositivo),
                                args, entorno);
    }
    return -1;
}

/* Ctrl-C. Va al GRUPO de primer plano, y a todos los que haya dentro.
 *
 * Antes iba al duenyo de la consola, o al hijo al que este estuviera
 * esperando: una cadena de un solo eslabon, y por eso fallaba en los dos
 * casos que importan. En "a | b" el shell espera primero a 'a', asi que
 * Ctrl-C mataba a 'a' y dejaba a 'b' vivo leyendo de una tuberia que ya
 * no tenia quien escribiera. Y a un nieto -un programa que se bifurca- no
 * llegaba nunca.
 *
 * Los dos fallos son el mismo: se estaba buscando UN proceso cuando lo
 * que el usuario quiere parar es un TRABAJO, que puede tener varios. La
 * cadena no se arregla haciendola mas larga; se arregla dejando de
 * seguirla y preguntando quien pertenece al grupo.
 *
 * Se hacen dos pasadas y no una: primero se apuntan los pid con el
 * cerrojo cogido, y luego se senyalan sin el, porque task_signal lo
 * vuelve a pedir. Recorrer la tabla llamando a task_signal desde dentro
 * seria un interbloqueo contra uno mismo. */
int task_signal_grupo(uint64_t pgid, int sig)
{
    if (!pgid) return -ESRCH;

    uint64_t destinos[MAX_TASKS];
    int n = 0;

    uint64_t flags = sched_lock_irqsave();

    for (int i = CORES; i < MAX_TASKS && n < MAX_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pgd &&
            tasks[i].state != TASK_ZOMBIE && tasks[i].pgid == pgid)
            destinos[n++] = tasks[i].pid;

    sched_unlock_irqrestore(flags);

    for (int i = 0; i < n; i++) task_signal(destinos[i], sig);
    return n ? 0 : -ESRCH;
}

/* Detenerse aqui mismo, y volver donde estabas.
 *
 * No hay nada que guardar. El proceso esta a mitad de una llamada al
 * sistema, con su pila de kernel y su marco de excepcion tal cual: si no
 * se ejecuta, tampoco se mueve. Al reanudarlo, sigue por la linea de
 * abajo, y desde ahi vuelve por el mismo camino por el que entro.
 *
 * Devuelve -1 si al despertar hay algo que no admite continuar: solo
 * SIGKILL puede sacar de aqui aparte de SIGCONT, asi que si no fue el uno
 * fue el otro.
 *
 * Que esto sea una funcion y no dos copias importa: lo llaman el
 * repartidor de senyales -para un Ctrl-Z- y la lectura del teclado -para
 * un SIGTTIN-, y son dos caminos muy distintos hasta la misma decision. */
int task_parar(void)
{
    uint64_t flags = sched_lock_irqsave();

    current->state = TASK_STOPPED;
    wq_wake_all(&exit_wq);          /* que el padre se entere ANTES */
    schedule_locked();

    int abortar = (current->sig_pending & (1u << SIGKILL)) != 0;

    sched_unlock_irqrestore(flags);
    return abortar ? -1 : 0;
}

void task_console_interrupt(void) { task_signal_grupo(consola_pgid, SIGINT); }
void task_console_stop(void)      { task_signal_grupo(consola_pgid, SIGTSTP); }

/* ¿El grupo de quien pregunta es el que tiene la consola?
 *
 * Lo pregunta la lectura del teclado. Con la consola sin repartir -antes
 * de que init arranque a nadie- la respuesta es que si: negarsela a todo
 * el mundo dejaria la maquina muda. */
int task_en_primer_plano(void)
{
    if (!current || !current->pgd) return 1;
    if (!consola_pgid)             return 1;
    return current->pgid == consola_pgid;
}

/* ¿Puede el KERNEL escribir en esa direccion del proceso?
 *
 * Preguntarselo a la MMU no basta, y esto costo un fallo raro de
 * encontrar. Despues de un fork, las paginas del proceso estan marcadas de
 * solo lectura esperando a que el las toque; si quien escribe es el kernel
 * -copiando un caracter leido, o un mensaje- la MMU dice que no se puede
 * y tiene razon, pero la respuesta correcta no es rendirse: es hacer lo
 * mismo que se haria si hubiera escrito el proceso.
 *
 * Asi que aqui se intenta, por orden: ¿se puede ya?, ¿es una pagina
 * compartida que toca copiar?, ¿es la pila, que aun no ha crecido hasta
 * ahi? Y solo si nada de eso vale, que no. */
/* Arreglar un fallo de pagina sobre memoria de un proceso. Devuelve 1 si
 * se ha podido.
 *
 * ESTO YA NO LO LLAMA NADIE PARA ACCEDER. Desde el paso 43, el kernel
 * toca memoria de usuario de una sola forma -copy_from_user y
 * copy_to_user- y esas no preguntan: acceden, y si falla, se falla. Esta
 * funcion es lo que hay al otro lado de ese fallo.
 *
 * Esa separacion es la que hacia falta. Antes habia dos verbos que
 * parecian el mismo -"comprobar si puedo" y "conseguir que pueda"- y el
 * codigo tenia que acordarse de cual usaba. Ahora hay uno solo, y esto no
 * es un verbo: es el manejador.
 *
 * Tres cosas se pueden arreglar, en este orden:
 *   - un fichero mapeado del que aun no se ha traido la pagina;
 *   - una pagina compartida por un fork, que toca copiar antes de
 *     escribir;
 *   - la pila, que crece hacia abajo cuando el proceso la necesita.
 */
int user_fault_fix(uint64_t va, int escritura)
{
    if (!current || !current->pgd) return 0;

    if (escritura) {
        if (vmm_translate_user_w(va)) return 1;

        if (vmm_cow_fault(current->pgd, va, current->asid) &&
            vmm_translate_user_w(va)) return 1;

        if (task_grow_stack(va, va) && vmm_translate_user_w(va)) return 1;
        return 0;
    }

    if (vmm_translate_user(va)) return 1;
    if (task_mmap_fault(va) && vmm_translate_user(va)) return 1;
    if (task_grow_stack(va, va) && vmm_translate_user(va)) return 1;
    return 0;
}

/* El marco de senyal va en la pila del proceso, y la pila puede no estar
 * toda ahi: crecer es lo suyo. Se escribe un byte en cada pagina para
 * provocar el fallo que la trae, y si alguno no se puede es que no hay
 * sitio y el proceso no puede recibir senyales.
 *
 * Con copy_to_user, "provocar el fallo" y "comprobar si se puede" son la
 * misma operacion, que es justo lo que se buscaba al dejar un solo
 * camino. */
static int pila_escribible(uint64_t sp, uint64_t n)
{
    const char cero = 0;

    for (uint64_t p = sp & ~(uint64_t)(PAGE_SIZE - 1); p < sp + n; p += PAGE_SIZE)
        if (copy_to_user(p, &cero, 1) != 0) return 0;

    return 1;
}

/* Entregar una senyal. Aqui esta el truco entero.
 *
 * Si no hay manejador, la accion por defecto: morirse. Y si lo hay, el
 * kernel FABRICA UNA LLAMADA A FUNCION en espacio de usuario: guarda el
 * contexto interrumpido en la pila del proceso y reescribe el marco de
 * excepcion para que, al hacer el 'eret', el proceso aparezca dentro de su
 * manejador como si lo hubiera llamado el mismo.
 *
 * Cuando el manejador retorna, cae en el trampolin -que el kernel dejo en
 * x30- y ese llama a sigreturn, que deshace todo esto y devuelve al
 * proceso exactamente donde estaba. El proceso no puede notar la
 * diferencia, y esa es la idea.
 */
void signal_deliver(struct trap_frame *f)
{
    struct task *t = current;

    if (!t || !t->pgd || !t->sig_pending) return;
    if (t->sig_frame) return;            /* ya hay una en curso: sin anidar */

    for (int s = 1; s < SIG_MAX; s++) {
        if (!(t->sig_pending & (1u << s))) continue;
        t->sig_pending &= ~(1u << s);

        /* SIGCONT sin manejador no hace NADA aqui, y eso es correcto: su
         * trabajo entero ocurrio en task_signal, que es quien devolvio el
         * proceso a la cola de listos. Cuando llega hasta este punto es
         * que el proceso ya estaba corriendo, y entonces no hay nada que
         * continuar. */
        if (s == SIGCONT && !t->sig_handler[s]) continue;

        /* Ni SIGKILL ni SIGSTOP se atrapan, y por el mismo motivo: son las
         * dos unicas garantias que le quedan a quien esta fuera. Una es
         * "esto se puede matar" y la otra "esto se puede parar", y las dos
         * dejan de valer en cuanto el programa puede opinar. */
        uint64_t h = (s == SIGKILL || s == SIGSTOP) ? 0 : t->sig_handler[s];

        /* Detenerse es una accion POR DEFECTO, igual que morirse; lo que
         * pasa es que no es definitiva. El proceso se queda aqui dentro,
         * a mitad de volver a EL0, con su marco de excepcion intacto: no
         * se guarda nada ni se deshace nada, porque no se va a ningun
         * sitio. Cuando alguien le mande un SIGCONT seguira por la linea
         * de abajo y hara su eret como si no hubiera pasado nada.
         *
         * Y hay que avisar al padre ANTES de pararse. Si no, un shell que
         * esta en waitpid esperando a este proceso se queda ahi para
         * siempre: el hijo no ha muerto -asi que no despierta a nadie- y
         * tampoco va a volver a correr. Los dos esperando al otro. */
        if (!h && ES_PARADA(s)) {
            task_parar();
            continue;                    /* al volver, mirar si queda algo */
        }

        if (!h) {
            uint64_t lf = uart_begin();
            uart_puts("\n  [kernel] ");
            uart_puts(t->name);
            uart_puts(" termina por la senyal ");
            uart_dec((uint64_t)s);
            uart_puts("\n");
            uart_end(lf);
            task_exit();                 /* no vuelve */
        }

        /* El marco lleva los registros enteros y, si este proceso ha
         * llegado a usar la FPU, tambien sus 528 bytes.
         *
         * Guardarlos solo cuando hacen falta no es tacanyeria: la mayoria
         * de los procesos no toca la coma flotante en su vida, y hacerles
         * pagar 528 bytes de pila en cada senyal seria cobrarles por algo
         * que no usan. Es la misma pereza del paso 30, ahora en la pila. */
        int con_fp = (t->fp_state != 0);
        uint64_t marco = sizeof(struct trap_frame) + (con_fp ? FP_STATE_SIZE : 0);

        uint64_t sp = (f->sp_el0 - marco) & ~15UL;

        if (!t->sig_tramp || !pila_escribible(sp, marco)) {
            uart_puts("\n  [kernel] no puedo entregarle la senyal: lo mato\n");
            task_exit();
        }

        kcopy((void *)sp, f, sizeof(struct trap_frame));

        if (con_fp) {
            /* Bajar a memoria lo que este vivo en los registros ANTES de
             * copiarlo. Si el proceso tiene la FPU encendida, la copia
             * buena esta en el silicio y la de t->fp_state es de la
             * ultima vez que lo desalojaron. */
            fp_switch_out(t);
            kcopy((char *)sp + sizeof(struct trap_frame), t->fp_state,
                  FP_STATE_SIZE);
        }

        t->sig_fp    = con_fp;
        t->sig_frame = sp;
        f->sp_el0    = sp;
        f->elr       = h;               /* el proceso "aparece" aqui */
        f->x[0]      = (uint64_t)s;     /* con la senyal como argumento */
        f->lr        = t->sig_tramp;    /* y vuelve por aqui */
        return;
    }
}

/* Deshacer lo anterior. Devuelve el x0 que tenia el proceso antes de que
 * lo interrumpieramos: el despachador de llamadas lo pondra en su sitio,
 * igual que hace con exec. */
int64_t signal_return(struct trap_frame *f)
{
    struct task *t = current;
    if (!t || !t->sig_frame) return -1;

    uint64_t sp = t->sig_frame;
    if (!vmm_translate_user(sp)) return -1;

    kcopy(f, (const void *)sp, sizeof(struct trap_frame));

    /* Y la coma flotante, que es de lo que iba este paso.
     *
     * Lo que haya dejado el manejador en los registros se tira: se apaga
     * la FPU -eso guarda lo suyo en t->fp_state, que enseguida se pisa- y
     * se restaura lo que habia antes de la senyal. El programa
     * interrumpido no se entera de nada, que es justo lo que se le
     * prometio. */
    if (t->sig_fp && t->fp_state) {
        fp_switch_out(t);
        kcopy(t->fp_state, (const char *)sp + sizeof(struct trap_frame),
              FP_STATE_SIZE);
    } else if (!t->sig_fp && t->fp_state) {
        /* El proceso no habia tocado la FPU y el MANEJADOR si. No hay
         * nada que restaurar, pero dejarle los numeros del manejador
         * seria que el programa se encontrara luego basura con la que no
         * contaba. Se suelta y la proxima vez nace a cero. */
        fp_hw_disable();
        fp_release(t);
    }

    t->sig_frame = 0;
    t->sig_fp    = 0;
    return (int64_t)f->x[0];
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
int task_wait(uint64_t pid, int64_t *codigo, int *que, int banderas)
{
    uint64_t flags = sched_lock_irqsave();
    int      ret   = 0;

    /* Con WNOHANG NO se apunta la espera. 'waiting_for' existia para que
     * Ctrl-C supiera a quien seguir, y ya no sirve para eso; lo que si
     * hace todavia es decir que este proceso esta parado en un hijo, y
     * quien pregunta sin bloquearse no lo esta. */
    if (current && !(banderas & WNOHANG)) current->waiting_for = pid;

    if (que) *que = W_SALIDA;

    for (;;) {
        struct task *t = by_pid(pid);

        if (!t) {                          /* ya no existe: nada que contar */
            if (codigo) *codigo = -1;
            break;
        }

        if (t->state == TASK_ZOMBIE) {
            if (codigo) *codigo = t->exit_code;

            /* Recogido. Ahora si se lo puede llevar el recolector: el
             * zombi existia precisamente para que su padre leyera esto. */
            t->parent = 0;
            wq_wake_one(&reaper_wq);
            break;
        }

        /* Detenido no es terminado, y por eso hay que pedirlo aparte.
         *
         * Un waitpid que contestara a las dos cosas sin distinguirlas seria
         * peor que uno que no contesta: el que espera creeria que su hijo
         * ha acabado y seguiria adelante dejando atras un proceso que
         * sigue existiendo, con sus ficheros abiertos y su memoria. Que
         * WUNTRACED haya que pedirlo -y no venga de serie- es de las pocas
         * decisiones de Unix que se explican solas: quien no sabe que
         * existen los procesos detenidos no sabria que hacer con uno.
         *
         * Y NO se recoge: un proceso parado no es un zombi, va a volver.
         * Lo unico que se hace es contarlo. */
        if (t->state == TASK_STOPPED && (banderas & WUNTRACED)) {
            if (codigo) *codigo = 0;
            if (que)    *que    = W_PARADO;
            break;
        }

        if (banderas & WNOHANG) {
            ret = -EAGAIN;                   /* sigue vivo; vuelve luego */
            break;
        }

        if (wq_wait(&exit_wq) < 0) {       /* nos ha interrumpido una senyal */
            ret = -EINTR;
            break;
        }
    }

    if (current) current->waiting_for = 0;

    sched_unlock_irqrestore(flags);
    return ret;
}

void task_exit_con(int64_t codigo)
{
    uint64_t flags = sched_lock_irqsave();
    (void)flags;                 /* este cerrojo no lo soltamos nosotros */
    current->exit_code = codigo;
    current->state     = TASK_ZOMBIE;
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

void task_exit(void) { task_exit_con(-1); }

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
/* Leer de la imagen de un programa, venga de donde venga.
 *
 * load_elf sirve a DOS AMOS y es facil no darse cuenta:
 *
 *   - los programas empotrados en el propio kernel, que arranca el menu;
 *   - y los que trae un proceso, que casi siempre son un fichero mapeado.
 *
 * Para los segundos hay que usar copy_from_user, que accede con permisos
 * de EL0 y sabe recuperarse. Para los primeros eso NO vale: son memoria
 * del kernel, EL0 no puede verla, y ldtr falla siempre.
 *
 * Se distingue por la direccion, que es lo unico que hay y lo unico que
 * no se puede falsear: por encima de KERNEL_VA_BASE es nuestro.
 *
 * (Esto costo un arranque entero en el que TODOS los procesos fallaban al
 * crearse. El sintoma decia "no he podido crearlo" once veces seguidas y
 * no decia por que; la causa era que una funcion que servia a dos amos
 * paso a servir bien solo a uno.) */
static int leer_imagen(void *dst, const uint8_t *img, uint64_t off, uint64_t n)
{
    uint64_t src = (uint64_t)img + off;

    if (src >= KERNEL_VA_BASE) {
        kcopy(dst, (const void *)src, n);
        return 0;
    }
    return copy_from_user(dst, src, n) == 0 ? 0 : -1;
}

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

            /* copy_from_user y no kcopy: 'img' es una direccion del
             * PROCESO, y puede ser un fichero mapeado del que todavia no
             * se ha traido nada. Cada pagina que falte llega sola, porque
             * el fallo lo arregla el manejador y la instruccion se
             * reintenta. Y si el puntero es basura, esto devuelve cuantos
             * bytes faltaron en vez de reventar el kernel. */
            if (leer_imagen(phys_to_virt(page), img, off + p, n) < 0) {
                pmm_free(page);
                return -1;
            }
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

    /* Las cabeceras se traen a memoria del kernel ANTES de mirarlas, y no
     * es solo por los fallos de pagina: leerlas directamente de la memoria
     * del proceso deja la puerta abierta a que las cambie EN MEDIO, entre
     * la comprobacion y el uso. Con cuatro nucleos eso no es teorico: otro
     * hilo del mismo proceso puede estar escribiendo ahi mismo.
     *
     * Se comprueba lo que se va a usar, y se usa lo que se comprobo. */
    struct elf64_ehdr cab;
    if (leer_imagen(&cab, img, 0, sizeof(cab)) < 0) return -1;

    const struct elf64_ehdr *eh = &cab;

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

    struct elf64_phdr ph[8];
    if (leer_imagen(ph, img, eh->e_phoff,
                    (uint64_t)eh->e_phnum * sizeof(ph[0])) < 0)
        return -1;

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
 * El convenio es el de siempre: x0 = argc, x1 = argv, x2 = envp, y los
 * dos arrays terminan en cero. Todo eso vive en la pila del
 * proceso, que es el unico sitio que ya es suyo y donde se puede escribir
 * antes de que exista.
 *
 * Se escribe por el mapa lineal del kernel, no por la direccion de
 * usuario: esa pagina todavia no esta en ningun TTBR0 activo.
 */

/* Copiar un lote de cadenas a la pila del proceso nuevo y dejar su array
 * de punteros. Sirve igual para los argumentos y para el entorno, porque
 * los dos son lo mismo: un array de cadenas terminado en cero.
 *
 * 'tope' es por donde va el texto (se llena hacia abajo) y se actualiza.
 * Devuelve la direccion de USUARIO del array. */
static uint64_t poner_lote(char *k, uint64_t base, const struct args *a,
                           uint64_t *tope)
{
    uint64_t off[MAX_ARGS];

    for (int i = a->n - 1; i >= 0; i--) {
        const char *s = a->buf + a->off[i];
        uint64_t len = 0;
        while (s[len]) len++;

        *tope -= len + 1;
        off[i] = *tope;
        for (uint64_t j = 0; j <= len; j++) k[*tope + j] = s[j];
    }

    uint64_t n      = (uint64_t)a->n;
    uint64_t o_argv = (*tope - (n + 1) * 8) & ~15UL;
    uint64_t *argv  = (uint64_t *)(k + o_argv);

    for (uint64_t i = 0; i < n; i++) argv[i] = base + off[i];
    argv[n] = 0;

    *tope = o_argv;
    return base + o_argv;
}

static void build_args(uint64_t ustack_pa, const struct args *a,
                       const struct args *e,
                       uint64_t *argc_out, uint64_t *argv_out,
                       uint64_t *envp_out, uint64_t *sp_out)
{
    char    *k    = (char *)phys_to_virt(ustack_pa);   /* la pagina, en kernel */
    uint64_t base = USER_STACK_TOP - PAGE_SIZE;        /* la misma, en usuario */

    /* Aqui ya no se parte nada: los argumentos vienen troceados. Esta
     * funcion solo los COPIA a la pila del proceso nuevo y monta los dos
     * arrays de punteros. Quien trocea es el shell, que es su trabajo.
     *
     * El ENTORNO va primero, mas arriba en la pila, y los argumentos
     * debajo. El orden da igual para el programa -recibe dos punteros-
     * pero el sp final tiene que quedar en el array de argumentos, que es
     * lo que espera el convenio. */
    uint64_t tope = PAGE_SIZE;

    uint64_t envp = poner_lote(k, base, e, &tope);
    uint64_t argv = poner_lote(k, base, a, &tope);

    *argc_out = (uint64_t)a->n;
    *argv_out = argv;
    *envp_out = envp;
    *sp_out   = argv;
}

/* Comodidad para el menu del kernel, que arranca programas con
 * argumentos literales: "cat HOLA.TXT" y cosas asi. */
int task_create_user_str(const char *name, const uint8_t *image, uint64_t size,
                         uint64_t mmio_pa, const char *cadena)
{
    struct args a, e;
    args_de_cadena(&a, cadena);

    /* El entorno con el que nace un proceso arrancado desde el menu del
     * kernel. Alguien tiene que poner el primero: el entorno se HEREDA, y
     * una herencia necesita un antepasado.
     *
     * En un Unix de verdad lo pone init leyendo ficheros de
     * configuracion. Aqui son dos lineas escritas a mano, y eso basta
     * para que el shell deje de llevar el PATH dentro del codigo. */
    args_de_cadena(&e, "PATH=/usr/bin:. HOME=/ TERM=serie");

    return task_create_user(name, image, size, mmio_pa, &a, &e);
}

/* Partir una cadena por espacios. Sin comillas: esto es para los caminos
 * internos del kernel -el menu- donde los argumentos son literales que
 * escribimos nosotros. */
void args_de_cadena(struct args *a, const char *s)
{
    a->n = 0;
    uint64_t escribe = 0;

    if (!s) return;

    while (*s && a->n < MAX_ARGS) {
        while (*s == ' ') s++;
        if (!*s) break;

        a->off[a->n++] = (uint16_t)escribe;
        while (*s && *s != ' ' && escribe < ARGS_BYTES - 1)
            a->buf[escribe++] = *s++;
        while (*s && *s != ' ') s++;          /* por si no cabia */
        a->buf[escribe++] = 0;
    }
}

/* Como se llama un proceso que arranca otro proceso.
 *
 * El nombre no puede ser un puntero a la memoria del que llama: ese espacio
 * de direcciones puede desaparecer antes que la tarea. Se copia la primera
 * palabra de sus argumentos, que es justo como se llama a si mismo. */
/* El nombre del proceso sale de argv[0], como en cualquier Unix. Ya no
 * hay que buscar el primer espacio: viene troceado. */
static void nombre_de_args(struct task *t, const struct args *a)
{
    uint64_t o = 0;

    if (a && a->n > 0) {
        const char *s = a->buf + a->off[0];
        while (*s && o < sizeof(t->namebuf) - 1) t->namebuf[o++] = *s++;
    }
    if (!o) t->namebuf[o++] = '?';
    t->namebuf[o] = 0;
    t->name = t->namebuf;
}

/* --- Convertirse en otro programa ------------------------------------
 *
 * fork duplica; exec sustituye. Juntos son la forma clasica de arrancar un
 * programa en Unix, y cada uno hace una cosa sola: el proceso se bifurca y
 * el hijo se convierte en otra cosa.
 *
 * Aqui hay una regla que no aparece en ninguna otra parte del kernel: NO
 * HAY VUELTA ATRAS. En cuanto se tira el espacio de direcciones viejo, un
 * fallo deja al proceso sin memoria, sin codigo y sin sitio al que volver.
 * Por eso todo lo que puede fallar se hace ANTES: se construye el espacio
 * nuevo entero, y solo cuando esta montado y no queda nada que pueda ir
 * mal se cambia el de sitio.
 *
 * Y el orden importa por otro motivo: la imagen del programa nuevo vive en
 * la memoria del proceso VIEJO. Si se tirara primero, no quedaria nada que
 * leer. Se construye el espacio nuevo leyendo del viejo, que sigue siendo
 * el activo, y se cambia al final.
 *
 * Devuelve argc, y eso no es capricho: el despachador de llamadas hace
 * "f->x[0] = ret" al terminar, que es exactamente donde el programa nuevo
 * espera encontrar su argc.
 */
int task_exec(const uint8_t *image, uint64_t size, const struct args *args,
              const struct args *entorno,
              struct trap_frame *f)
{
    struct task *t = current;
    if (!t || !t->pgd) return -1;

    uint64_t asid = 0;
    uint64_t *pgd = vmm_create_pgd(&asid);
    if (!pgd) return -1;

    uint64_t entry = 0, tope = USER_BASE;
    if (load_elf(pgd, image, size, &entry, &tope) < 0) goto fail;

    uint64_t ustack = pmm_alloc();
    if (!ustack) goto fail;

    uint64_t argc = 0, argv = 0, envp = 0, sp = USER_STACK_TOP;
    build_args(ustack, args, entorno, &argc, &argv, &envp, &sp);

    if (vmm_map_in(pgd, USER_STACK_TOP - PAGE_SIZE, ustack, MM_USER_DATA) < 0)
        goto fail;

    /* ---- A partir de aqui ya no se puede fallar ---- */

    uint64_t *viejo_pgd  = t->pgd;
    uint64_t  viejo_asid = t->asid;

    uint64_t flags = sched_lock_irqsave();
    t->pgd       = pgd;
    t->asid      = asid;
    t->brk_base  = tope;
    t->brk       = tope;
    t->stack_low = USER_STACK_TOP - PAGE_SIZE;

    /* Un programa nuevo empieza sin senyales pendientes y sin manejadores:
     * los que habia eran del programa anterior y ya no existen. */
    t->sig_pending = 0;
    t->sig_frame   = 0;
    t->sig_fp      = 0;
    t->sig_tramp   = 0;
    for (int s = 0; s < SIG_MAX; s++) t->sig_handler[s] = 0;

    /* Y sin coma flotante, por lo mismo. Lo que hubiera en esos registros
     * era del programa anterior; el nuevo tiene que empezar con la FPU
     * apagada y encontrarsela a cero cuando la pida. Se apaga a mano
     * porque exec NO pasa por el cambio de contexto: vuelve a EL0 siendo
     * el mismo hilo, y nadie mas iba a hacerlo. */
    fp_hw_disable();
    fp_release(t);

    /* Los ficheros mapeados se van con el programa que los mapeo. Las
     * paginas ya estan muertas -el espacio de direcciones entero se
     * sustituye-, asi que esto solo borra el apunte. */
    mapeos_limpiar(t);

    /* El directorio actual NO se toca, y esa ausencia es la regla: el
     * programa cambia, el sitio donde estabas no. Es lo que hace que
     * "cd docs" seguido de "cat notas.txt" funcione. */

    /* El MMIO concedido NO se hereda: se le dio al programa que habia, y
     * ese programa ya no existe. Un driver que hace exec deja de ser un
     * driver. */
    t->mmio_va   = 0;

    nombre_de_args(t, args);
    sched_unlock_irqrestore(flags);

    /* El espacio nuevo, activo YA: el viejo esta a punto de dejar de
     * existir y TTBR0 todavia apunta a el. */
    vmm_switch_to(pgd, asid);
    vmm_destroy_pgd(viejo_pgd, viejo_asid);

    /* Y el contexto, reescrito entero. El proceso no "vuelve" de esta
     * llamada: aparece en el primer instante de otro programa. */
    kzero(f, sizeof(*f));
    f->elr    = entry;
    f->spsr   = 0;                  /* EL0t, con las IRQ abiertas */
    f->sp_el0 = sp;
    f->x[1]   = argv;               /* x0 lo pone el despachador con argc */
    f->x[2]   = envp;               /* y crt0 lo guarda en 'environ'      */

    return (int)argc;

fail:
    vmm_destroy_pgd(pgd, asid);
    return -1;
}

/* --- Bifurcarse ------------------------------------------------------
 *
 * Un proceso se duplica. El hijo sale de aqui con EL MISMO estado que el
 * padre -los mismos registros, la misma pila, la misma posicion en el
 * codigo- y la unica diferencia esta en x0: el padre recibe el pid del
 * hijo y el hijo recibe un cero. De ahi sale el "if (fork() == 0)" de toda
 * la vida.
 *
 * Y no se copia memoria. Los dos espacios apuntan a las mismas paginas,
 * marcadas de solo lectura; la primera escritura de cualquiera de los dos
 * es la que paga su copia. Bifurcar un proceso de 8 MB cuesta lo mismo que
 * bifurcar uno de 8 KB: recorrer la tabla.
 */
int task_fork(struct trap_frame *f)
{
    struct task *padre = current;
    if (!padre || !padre->pgd) return -1;   /* un hilo de kernel no se bifurca */

    uint64_t flags = sched_lock_irqsave();
    struct task *t = 0;

    for (int i = CORES; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_UNUSED) { t = &tasks[i]; break; }
    if (!t) { sched_unlock_irqrestore(flags); return -1; }

    t->state = TASK_BLOCKED;                /* ranura reservada */
    t->name  = "(bifurcando)";
    t->stack = 0;
    t->pgd   = 0;

    /* La ranura viene de otro hilo que ya murio: su marca de FPU no es
     * nuestra. Aqui, y no mas abajo, porque fp_area_alloc la mira. */
    t->fp_state  = 0;
    t->fp_activo = 0;
    t->fp_pedida = 0;
    sched_unlock_irqrestore(flags);

    uint64_t asid = 0;
    uint64_t *pgd = vmm_fork(padre->pgd, padre->asid, &asid);
    if (!pgd) goto fail;

    uint64_t kstack = kstack_alloc((int)(t - tasks));
    if (!kstack) { vmm_destroy_pgd(pgd, asid); goto fail; }
    *(uint64_t *)kstack = STACK_MAGIC;

    /* Los descriptores SE HEREDAN, y eso es lo que hace util al fork: el
     * shell prepara una tuberia, se bifurca, y el hijo ya la tiene puesta
     * sin haber hecho nada.
     *
     * Y se copian FUERA del cerrojo del planificador: file_dup lo pide por
     * su cuenta, y pedirlo dos veces es un interbloqueo contra uno mismo.
     * La ranura ya esta reservada, asi que nadie mas la va a tocar. */
    fd_heredar(t, padre);

    /* El contexto del hijo es una copia del que tiene el padre ahora mismo
     * en su pila de kernel: los mismos registros y el mismo punto de
     * retorno. Solo cambia x0. */
    struct trap_frame *tf =
        (struct trap_frame *)(kstack + KSTACK_PAGINAS * PAGE_SIZE - sizeof(struct trap_frame));
    /* kcopy y no "*tf = *f": una asignacion de estructura de 288 bytes la
     * convierte gcc en una llamada a memcpy, y aqui no hay libc. */
    kcopy(tf, f, sizeof(*tf));
    tf->x[0] = 0;

    /* La coma flotante tambien se hereda: el hijo es el mismo programa en
     * el mismo punto, y si el padre tenia un numero a medias en q0 el hijo
     * tiene que verlo igual.
     *
     * Primero se BAJA la del padre a memoria. Si la tiene encendida, sus
     * registros son la copia buena y la que hay en su area esta vieja;
     * copiar sin bajarla le daria al hijo un estado de hace un rato. El
     * padre volvera a atraparla la proxima vez que la use, que cuesta una
     * excepcion y nada mas. */
    if (padre->fp_state) {
        fp_switch_out(padre);
        if (fp_area_alloc(t))
            kcopy(t->fp_state, padre->fp_state, FP_STATE_SIZE);
    }
    t->fp_activo = 0;                   /* la suya esta en memoria, no viva */

    flags = sched_lock_irqsave();

    t->stack     = kstack;
    t->pgd       = pgd;
    t->asid      = asid;
    t->pid       = next_pid++;
    t->counter   = TASK_QUANTUM;
    t->ticks_run = 0;
    t->mmio_va   = padre->mmio_va;

    /* El directorio actual se hereda. Es lo que hace que "cd docs" en el
     * shell tenga efecto sobre lo que ejecutes despues. */
    for (int i = 0; i < FS_PATH_MAX; i++) t->cwd[i] = padre->cwd[i];

    /* Y los ficheros mapeados. Las paginas que ya estaban dentro las copia
     * vmm_fork como cualquier otra -en copy-on-write-, y las que no,
     * volveran a pedirse al servidor cuando el hijo las toque. Es la misma
     * pereza dos veces: una en la memoria y otra en el disco. */
    /* kcopy y no una asignacion de estructura: son 80 bytes, y gcc
     * convierte eso en una llamada a memcpy que aqui no existe. Es la
     * tercera vez que pasa en este proyecto y siempre se ve igual, en el
     * enlazador y no en el compilador. */
    kcopy(t->mapeos, padre->mapeos, sizeof(t->mapeos));

    t->brk_base  = padre->brk_base;
    t->brk       = padre->brk;
    t->stack_low = padre->stack_low;

    /* Los manejadores SE HEREDAN -el hijo es el mismo programa y sabe
     * atrapar lo mismo- pero las senyales pendientes no: son del padre, y
     * el hijo no tiene por que pagarlas. */
    for (int s = 0; s < SIG_MAX; s++) t->sig_handler[s] = padre->sig_handler[s];
    t->sig_tramp   = padre->sig_tramp;
    t->sig_pending = 0;
    t->sig_frame   = 0;
    t->sig_fp      = 0;
    t->waiting_for = 0;
    t->parent      = padre->pid;

    /* El grupo se hereda, como el directorio actual: un hijo forma parte
     * del mismo TRABAJO que su padre mientras nadie diga lo contrario. Es
     * lo que hace que Ctrl-C sobre "sh" alcance a un nieto sin que nadie
     * lleve un arbol. */
    t->pgid        = padre->pgid;

    /* El nombre se copia, no se apunta: el del padre puede vivir en el
     * padre, y el padre puede morirse antes. */
    uint64_t o = 0;
    for (const char *s = padre->name; *s && o < sizeof(t->namebuf) - 1; s++)
        t->namebuf[o++] = *s;
    t->namebuf[o] = 0;
    t->name = t->namebuf;

    kzero(&t->ctx, sizeof(t->ctx));
    t->ctx.pc = (uint64_t)ret_to_user;
    t->ctx.sp = (uint64_t)tf;

    t->state = TASK_READY;
    sched_kick_idle();

    int pid = (int)t->pid;
    sched_unlock_irqrestore(flags);
    return pid;

fail:
    flags = sched_lock_irqsave();
    t->state = TASK_UNUSED;
    sched_unlock_irqrestore(flags);
    return -1;
}

int task_create_user(const char *name, const uint8_t *image, uint64_t size,
                     uint64_t mmio_pa, const struct args *args,
                     const struct args *entorno)
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

    uint64_t argc = 0, argv = 0, envp = 0, sp = USER_STACK_TOP;
    build_args(ustack, args, entorno, &argc, &argv, &envp, &sp);

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

    /* Las ranuras se reciclan, asi que lo de las senyales hay que
     * limpiarlo a mano: un manejador que quedara puesto apuntaria al
     * codigo de un programa que ya no existe. */
    t->sig_pending = 0;
    t->sig_frame   = 0;
    t->sig_fp      = 0;
    t->sig_tramp   = 0;
    t->waiting_for = 0;
    for (int s = 0; s < SIG_MAX; s++) t->sig_handler[s] = 0;

    /* Nace sin FPU. Si la quiere, que la pida atrapando. */
    t->fp_state  = 0;
    t->fp_activo = 0;
    t->fp_pedida = 0;

    /* Y en el raiz. Un proceso creado desde el menu del kernel no tiene de
     * quien heredar un directorio actual. */
    t->cwd[0] = '/';
    t->cwd[1] = 0;

    mapeos_limpiar(t);

    /* Entrada, salida y errores a la consola. Si quien lo arranca quiere
     * otra cosa, que los cambie despues. */
    for (int i = 0; i < MAX_FD; i++) t->fd[i] = 0;
    t->fd[0] = t->fd[1] = t->fd[2] = file_consola();
    t->parent = current ? current->pid : 0;

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

    /* Un proceso que no nace de un fork no tiene de quien heredar un
     * trabajo, asi que forma el suyo y le da su propio nombre. De ahi
     * sale que el pgid de un grupo sea siempre el pid del primero. */
    t->pgid      = t->pid;
    t->counter   = TASK_QUANTUM;
    t->ticks_run = 0;

    /* --- El trap_frame fabricado --- */
    struct trap_frame *tf =
        (struct trap_frame *)(kstack + KSTACK_PAGINAS * PAGE_SIZE - sizeof(struct trap_frame));
    kzero(tf, sizeof(*tf));
    tf->elr    = entry;              /* lo dice el ELF                      */
    tf->spsr   = 0;                  /* M=0b0000 -> EL0t; DAIF=0 -> IRQ ON  */
    tf->sp_el0 = sp;                 /* su pila, con argv ya puesto encima  */
    tf->x[0]   = argc;
    tf->x[1]   = argv;
    tf->x[2]   = envp;               /* el entorno, que crt0 guardara       */

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
