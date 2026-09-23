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
#include "user_abi.h"
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

/* Lo que hace un nucleo cuando no tiene nada que hacer.
 *
 * 'wfe' y no 'wfi', y la diferencia es todo el reparto de trabajo: wfi solo
 * despierta con una interrupcion, o sea que un nucleo ocioso tardaria hasta
 * un tick entero en enterarse de que hay una tarea lista. wfe despierta
 * ademas con 'sev', y resulta que spin_unlock() ya hace 'sev' — asi que
 * cada vez que alguien suelta el cerrojo del planificador, y eso incluye
 * cada vez que una tarea pasa a lista, los nucleos ociosos se despiertan
 * solos y van a buscar trabajo.
 *
 * Un 'sev' de mas solo cuesta una vuelta de este bucle sin encontrar nada.
 * Un 'sev' de menos cuesta 10 ms de un nucleo parado. */
void idle_loop(void)
{
    for (;;) {
        __asm__ volatile("wfe");
        schedule();
    }
}

void sched_start_smp(void)
{
    smp_sched_ready = 1;
    __asm__ volatile("dsb sy\n sev" ::: "memory");
}
static uint64_t switches;        /* cambios de contexto totales */

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

    uint64_t stack_pa = pmm_alloc();    /* 4 KB de pila por hilo */
    if (!stack_pa) { sched_unlock_irqrestore(flags); return -1; }

    /* pmm_alloc habla en fisico; el hilo va a usar la pila de verdad, asi
     * que lo que se guarda es la direccion por la que el kernel la ve. */
    uint64_t stack = (uint64_t)phys_to_virt(stack_pa);

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
        /* Recorrer el array entero en cada tick es ineficiente; con muchos
         * hilos se usaria una cola ordenada. */
        uint64_t now = timer_ticks();
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].state == TASK_SLEEPING && now >= tasks[i].wake_tick)
                tasks[i].state = TASK_READY;
        }
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
static uint64_t         reaped;

static void reap(struct task *t)
{
    /* Su espacio de direcciones entero: tablas, paginas y el ASID, que
     * ademas limpia de la TLB lo que quedara con esa etiqueta. */
    if (t->pgd)
        vmm_destroy_pgd(t->pgd, t->asid);

    /* Y su pila. t->stack guarda la direccion del mapa lineal, asi que hay
     * que bajarla a fisico para devolversela al gestor de paginas. */
    if (t->stack)
        pmm_free(virt_to_phys((void *)t->stack));

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
static int load_range(uint64_t *pgd, const uint8_t *image, uint64_t size,
                      uint64_t va_ini, uint64_t va_fin, uint64_t copia_hasta,
                      uint64_t flags)
{
    for (uint64_t va = va_ini; va < va_fin; va += PAGE_SIZE) {
        uint64_t page = pmm_alloc();
        if (!page) return -1;

        if (va < copia_hasta) {
            uint64_t n = copia_hasta - va;
            if (n > PAGE_SIZE) n = PAGE_SIZE;

            /* Ni un byte de fuera de la imagen: el fichero podria estar
             * truncado y la cabecera prometer mas de lo que hay. */
            uint64_t off = va - USER_BASE;
            if (off >= size)          n = 0;
            else if (off + n > size)  n = size - off;

            if (n) kcopy(phys_to_virt(page), image + off, n);
        }

        if (vmm_map_in(pgd, va, page, flags) < 0) return -1;
    }
    return 0;
}

/* Comprueba que la cabecera dice algo coherente. Es codigo aburrido y es
 * exactamente el que evita que una imagen mal construida (o maliciosa)
 * consiga que el kernel mapee donde no debe. */
static int header_ok(const struct user_header *h, uint64_t size)
{
    if (size < sizeof(*h))                       return 0;
    if (h->magic != USER_MAGIC)                  return 0;
    if (h->version != USER_ABI_VER)              return 0;
    if (h->text_start != USER_BASE)              return 0;
    if (h->text_end & (PAGE_SIZE - 1))           return 0;  /* el corte de */
                                                            /* permisos va */
                                                            /* en frontera */
    if (h->text_end  <  h->text_start)           return 0;
    if (h->data_end  <  h->text_end)             return 0;
    if (h->bss_end   <  h->data_end)             return 0;
    if (h->entry     <  h->text_start ||
        h->entry     >= h->text_end)             return 0;  /* entrar en un */
                                                            /* sitio no     */
                                                            /* ejecutable   */
    /* Los datos tienen que estar de verdad en la imagen. El texto no hace
     * falta comprobarlo igual: text_end esta redondeado a pagina, asi que
     * un programa pequenyo da una imagen mas corta y el cargador rellena
     * el resto de la pagina con ceros. */
    if (h->data_end > h->text_end &&
        h->data_end - h->text_start > size)      return 0;
    if (h->bss_end >= USER_STACK_TOP - PAGE_SIZE) return 0; /* pisaria la   */
                                                            /* pila         */
    return 1;
}

int task_create_user(const char *name, const uint8_t *image, uint64_t size,
                     uint64_t mmio_pa)
{
    const struct user_header *h = (const struct user_header *)image;
    if (!header_ok(h, size)) return -1;

    uint64_t flags = sched_lock_irqsave();
    struct task *t = 0;

    for (int i = CORES; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_UNUSED) { t = &tasks[i]; break; }
    if (!t) { sched_unlock_irqrestore(flags); return -1; }

    uint64_t asid = 0;
    uint64_t *pgd = vmm_create_pgd(&asid);
    if (!pgd) { sched_unlock_irqrestore(flags); return -1; }

    uint64_t kstack_pa = 0;

    /* --- Tramo 1: texto y rodata. Solo lectura y ejecutable. -------------
     * Que sea RO no es un detalle: es lo que impide que un programa se
     * reescriba a si mismo, y lo que permitiria mas adelante compartir
     * estas paginas entre varias instancias del mismo programa. */
    if (load_range(pgd, image, size, h->text_start, h->text_end,
                   h->text_end, MM_USER_CODE) < 0)
        goto fail;

    /* --- Tramo 2: datos y bss. Escribible y NUNCA ejecutable. ------------
     * Hasta aqui el proceso no podia tener una sola variable global: la
     * imagen entera se mapeaba de solo lectura, asi que escribir en .data
     * era un fallo de permisos y .bss ni siquiera estaba mapeada. */
    uint64_t rw_end = (h->bss_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (load_range(pgd, image, size, h->text_end, rw_end,
                   h->data_end, MM_USER_DATA) < 0)
        goto fail;

    /* --- Pila de usuario: una pagina justo debajo de USER_STACK_TOP --- */
    uint64_t ustack = pmm_alloc();
    if (!ustack) goto fail;
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
    kstack_pa = pmm_alloc();
    if (!kstack_pa) goto fail;
    uint64_t kstack = (uint64_t)phys_to_virt(kstack_pa);
    *(uint64_t *)kstack = STACK_MAGIC;

    t->stack     = kstack;
    t->pgd       = pgd;
    t->asid      = asid;
    t->name      = name;
    t->pid       = next_pid++;
    t->counter   = TASK_QUANTUM;
    t->ticks_run = 0;

    /* --- El trap_frame fabricado --- */
    struct trap_frame *tf =
        (struct trap_frame *)(kstack + PAGE_SIZE - sizeof(struct trap_frame));
    kzero(tf, sizeof(*tf));
    tf->elr    = h->entry;           /* ya no se supone: lo dice la imagen  */
    tf->spsr   = 0;                  /* M=0b0000 -> EL0t; DAIF=0 -> IRQ ON  */
    tf->sp_el0 = USER_STACK_TOP;     /* su pila, no la nuestra              */

    kzero(&t->ctx, sizeof(t->ctx));
    t->ctx.pc = (uint64_t)ret_to_user;
    t->ctx.sp = (uint64_t)tf;

    t->state = TASK_READY;

    sched_unlock_irqrestore(flags);
    return (int)t->pid;

fail:
    /* Media carga es peor que ninguna: se devuelve todo lo repartido. Antes
     * de que existiera el recolector esto no se podia ni escribir. */
    if (kstack_pa) pmm_free(kstack_pa);
    vmm_destroy_pgd(pgd, asid);
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
        }
        uart_puts("\n");
    }
    sched_unlock_irqrestore(flags);
}
