/* syscall.c - Despacho de llamadas al sistema desde EL0
 *
 * Todo lo que un proceso puede pedirle al kernel pasa por aqui. En un
 * microkernel esta lista debe ser CORTA: idealmente solo comunicacion y
 * gestion de memoria, y que los servicios (ficheros, red, drivers) vivan en
 * otros procesos con los que se habla por mensajes. Las de aqui son las
 * minimas para tener algo que ejecutar.
 */
#include <stdint.h>
#include "syscall.h"
#include "exception.h"
#include "uart.h"
#include "sched.h"
#include "sync.h"
#include "timer.h"
#include "mm.h"
#include "file.h"
#include "ipc.h"
#include "mbox.h"
#include "irq.h"

extern struct mutex *console_mutex(void);

/* Un puntero que viene de EL0 NO es de fiar: puede apuntar al kernel, a un
 * periferico o a memoria de otro proceso. Antes de tocarlo hay que
 * comprobar que el proceso tiene derecho a leerlo, y eso lo sabe la MMU.
 * 'at s1e0r' traduce como lo haria EL0; si falla, el puntero es invalido
 * por muy legible que sea para el kernel. */
/* --- Tocar memoria de un proceso, de una sola forma ------------------
 *
 * Aqui habia dos caminos y habia que acordarse de cual tocaba:
 *
 *   user_readable() + un puntero a pelo   -> comprobaba a mano el rango,
 *                                            traia las paginas por
 *                                            adelantado y luego leia.
 *   copy_from_user()                       -> dejaba comprobar al silicio
 *                                            y sabia recuperarse.
 *
 * Los dos funcionaban. El problema de tener dos es que el primero
 * dependia de que nadie se olvidara de llamarlo, y de que el rango que
 * comprobaba fuera el mismo que luego se leia. Dos sitios que tienen que
 * decir lo mismo acaban diciendo cosas distintas.
 *
 * Desde el paso 43 solo queda el segundo. La comprobacion la hace la MMU
 * en cada acceso -ldtr y sttr usan permisos de EL0- y lo que falta llega
 * por el camino del fallo de pagina. Lo que antes eran dos pasos que
 * podian discrepar es ahora uno que no puede.
 *
 * Lo unico que se conserva a mano es el LIMITE del rango, y para otra
 * cosa: saber si un tamanyo es razonable antes de ponerse a trabajar. */
static int user_rango(uint64_t va, uint64_t len)
{
    if (len == 0) return 1;
    if (va < USER_BASE || va >= USER_LIMIT) return 0;
    if (va + len < va || va + len > USER_LIMIT) return 0;
    return 1;
}

/* Traerse una cadena del proceso, con tope.
 *
 * Se copia de golpe lo que quepa y se busca el cero dentro. Si el bloque
 * se corta antes -porque la cadena estaba al final de lo mapeado- lo que
 * se copio sigue valiendo: solo hay que encontrar el cero ahi dentro. Un
 * bucle byte a byte seria mas obvio y mucho mas lento. */
static int copiar_cadena(char *dst, uint64_t uva, uint64_t max)
{
    uint64_t falta = copy_from_user(dst, uva, max - 1);
    uint64_t hay   = (max - 1) - falta;

    for (uint64_t i = 0; i < hay; i++)
        if (!dst[i]) return i ? 0 : -1;          /* cero encontrado */

    return -1;            /* no cabe, o el puntero no vale */
}

static int copiar_ruta(char *dst, uint64_t uva)
{
    return copiar_cadena(dst, uva, FS_PATH_MAX);
}

/* Traerse una ruta del proceso Y resolverla contra su directorio actual.
 *
 * Las dos cosas van siempre juntas, y juntarlas ahorra un buffer de 256
 * bytes en cada sitio que la use. Eso importa aqui mas de lo que parece:
 * el despachador de llamadas es UNA funcion con un switch enorme, y el
 * compilador puede reservar a la vez el sitio de todas las ramas. */
static int traer_ruta(uint64_t uva, char *abs)
{
    char rel[FS_PATH_MAX];
    if (copiar_ruta(rel, uva) < 0) return -1;
    return path_resolve(current->cwd, rel, abs, FS_PATH_MAX);
}

static int copiar_args(struct args *a, uint64_t uargv)
{
    a->n = 0;
    if (!uargv) return 0;

    uint64_t escribe = 0;

    for (int i = 0; i < MAX_ARGS; i++) {
        uint64_t p;
        if (copy_from_user(&p, uargv + (uint64_t)i * 8, 8) != 0) return -1;
        if (!p) break;                            /* el cero final */

        a->off[a->n] = (uint16_t)escribe;

        if (copiar_cadena(a->buf + escribe, p, ARGS_BYTES - escribe) < 0)
            return -1;

        while (a->buf[escribe]) escribe++;
        escribe++;                                /* el cero */
        a->n++;
    }
    return 0;
}

/* Las seis operaciones que empiezan por una ruta, en una sola funcion.
 *
 * No es por ahorrar lineas: es por la PILA. Cada una necesita una o dos
 * rutas de 256 bytes, y el compilador no siempre reaprovecha el hueco
 * entre ramas de un switch. Con las seis dentro del despachador, la suma
 * de todas puede reservarse a la vez, y la pila de kernel son 4 KB con una
 * pagina de guarda debajo.
 *
 * Se encontro de la unica forma en que se encuentran estas cosas:
 * estrellandose contra la guarda. Que el kernel dijera "desbordamiento de
 * pila de kernel" en vez de corromper la tarea de al lado es exactamente
 * por lo que se puso esa pagina en el paso 22. */
static int64_t por_nombre(uint64_t nr, struct trap_frame *f)
{
    char abs[FS_PATH_MAX];
    if (traer_ruta(f->x[0], abs) < 0) return -EFAULT;

    switch (nr) {
    case SYS_unlink: return fs_borrar(abs);
    case SYS_mkdir:  return fs_mkdir(abs);
    case SYS_rmdir:  return fs_rmdir(abs);

    case SYS_stat: {
        struct estado e = { 0, 0, 0 };
        int r = fs_estado(abs, &e.tam, &e.mtime, &e.flags);
        if (r < 0) return r;
        return copy_to_user(f->x[1], &e, sizeof(e)) == 0 ? 0 : -EFAULT;
    }

    case SYS_opendir: {
        /* Tres motivos distintos para no poder abrir un directorio, y los
         * tres mandan a sitios distintos: no esta, no es un directorio, o
         * ya tienes todos los descriptores cogidos. */
        uint64_t flags = 0;
        int r = fs_estado(abs, 0, 0, &flags);
        if (r < 0) return r;
        if (!(flags & FS_ES_DIR)) return -ENOTDIR;

        struct fichero *fi = file_opendir(abs);
        if (!fi) return -ENOMEM;

        int fd = task_fd_alloc(fi);
        if (fd < 0) { file_close(fi); return -EMFILE; }
        return fd;
    }

    case SYS_rename: {
        char destino[FS_PATH_MAX];
        if (traer_ruta(f->x[1], destino) < 0) return -EFAULT;
        return fs_renombrar(abs, destino);
    }
    }
    return -ENOSYS;
}

/* exec y spawn, fuera del despachador y por el mismo motivo que
 * por_nombre: cada uno necesita DOS struct args de 292 bytes -los
 * argumentos y el entorno- y en la pila de kernel eso se nota. */
static int64_t sys_spawn(struct trap_frame *f)
{
    uint64_t buf = f->x[0], len = f->x[1];

    if (len == 0 || len > 256 * 1024) return -1;
    if (!user_rango(buf, len))         return -1;

    struct args args, entorno;
    if (copiar_args(&args, f->x[2]) < 0)    return -1;
    if (copiar_args(&entorno, f->x[3]) < 0) return -1;

    return task_create_user(0, (const uint8_t *)buf, len, 0, &args, &entorno);
}

static int64_t sys_exec(struct trap_frame *f)
{
    uint64_t buf = f->x[0], len = f->x[1];

    if (len == 0 || len > 1024 * 1024) return -1;
    if (!user_rango(buf, len))         return -1;

    /* Los dos lotes se copian ANTES de tocar nada del proceso, que es lo
     * unico que hace segura esta llamada: exec destruye el espacio de
     * direcciones de donde salen.
     *
     * Y el entorno se copia igual que los argumentos, no se hereda solo:
     * exec lo REEMPLAZA. Que en la practica casi siempre sea el mismo es
     * cosa del shell, que le pasa el suyo; el kernel no da nada por
     * hecho. */
    struct args args, entorno;
    if (copiar_args(&args, f->x[2]) < 0)    return -1;
    if (copiar_args(&entorno, f->x[3]) < 0) return -1;

    return task_exec((const uint8_t *)buf, len, &args, &entorno, f);
}

/* Y bootstrap, por lo mismo: dos struct args mas. */
static int64_t sys_bootstrap(struct trap_frame *f)
{
    /* La comprobacion es una linea, y es toda la frontera de privilegio
     * que hay entre procesos en este sistema: hay el primero y hay los
     * demas. Basta porque init es el unico que puede existir antes de que
     * exista nadie mas. */
    if (!current || current->pid != task_init_pid()) return -1;

    char nombre[16];
    if (copiar_cadena(nombre, f->x[0], sizeof(nombre)) < 0) return -1;

    struct args args, entorno;
    if (copiar_args(&args, f->x[1]) < 0)    return -1;
    if (copiar_args(&entorno, f->x[2]) < 0) return -1;

    /* argv vacio: que el programa se llame como pidio init. */
    if (args.n == 0) args_de_cadena(&args, nombre);

    return task_bootstrap(nombre, &args, &entorno, f->x[3]);
}

static int64_t sys_send(uint64_t port, uint64_t umsg)
{


    struct message m;
    if (copy_from_user(&m, umsg, sizeof(m)) != 0) return -1;
    m.from = current->pid;          /* el remitente NO se puede falsificar */
    if (m.len > MSG_DATA_MAX) m.len = MSG_DATA_MAX;

    return port_send((int)port, &m);
}

static int64_t sys_recv(uint64_t port, uint64_t umsg)
{


    struct message m;
    if (port_recv((int)port, &m, current->pid) < 0)
        return -1;

    /* Ojo: port_recv puede haber bloqueado al proceso y, al despertar,
     * seguimos en su espacio de direcciones porque schedule() restaura
     * TTBR0 con el contexto. Por eso este puntero sigue siendo valido. */
    if (copy_to_user(umsg, &m, sizeof(m)) != 0) return -1;
    return 0;
}

void syscall_dispatch(struct trap_frame *f)
{
    uint64_t nr = f->x[8];
    uint64_t a0 = f->x[0], a1 = f->x[1];
    int64_t  ret = -1;

    switch (nr) {
    /* Ya no va directo a la UART: va al descriptor que le digan. Que el 1
     * sea la consola es una costumbre, no una ley, y es exactamente lo que
     * permite que una tuberia funcione. */
    case SYS_write:
        ret = file_write(task_fd((int)f->x[0]), f->x[1], f->x[2]);
        break;

    case SYS_exit:
        /* Solo se anuncia si algo fue mal. Que un programa termine bien es
         * lo normal, y decirlo en voz alta llena de ruido una sesion de
         * shell: ahi el silencio ES la respuesta correcta. */
        if (a0 != 0) {
            uint64_t lf = uart_begin();
            uart_puts("\n  [kernel] el proceso ");
            uart_puts(current->name);
            uart_puts(" ha terminado con codigo ");
            uart_dec(a0);
            uart_puts("\n");
            uart_end(lf);
        }
        task_exit_con((int64_t)a0);      /* no vuelve */
        break;

    case SYS_yield:
        task_yield();
        ret = 0;
        break;

    case SYS_getpid:
        ret = (int64_t)current->pid;
        break;

    case SYS_sleep:
        task_sleep(a0);
        ret = 0;
        break;

    case SYS_uptime:
        ret = (int64_t)timer_uptime_ms();
        break;

    case SYS_port_create:
        ret = port_create(current->pid, (int64_t)f->x[0]);
        break;

    case SYS_send:
        ret = sys_send(a0, a1);
        break;

    case SYS_recv:
        ret = sys_recv(a0, a1);
        break;

    /* Crear un proceso con una imagen que trae el llamante.
     *
     * El puntero es del espacio del proceso que llama, y aqui se puede
     * desreferenciar sin mas porque TTBR0 sigue siendo el suyo: estamos
     * dentro de SU syscall. task_create_user() copia de ahi a las paginas
     * del hijo. Si nos desalojan a medias, al volver TTBR0 vuelve con
     * nosotros. */
    case SYS_spawn:
        ret = sys_spawn(f);
        break;

    /* Un driver de EL0 no puede hablar con el buzon de la GPU: el buzon es
     * uno solo para toda la maquina y darlo entero seria dar el control de
     * la placa. Pero si necesita saber a que velocidad va su reloj, porque
     * de ahi sale el divisor. El kernel contesta a esa pregunta concreta y
     * a ninguna mas. */
    case SYS_clock_rate: {
        uint64_t id = f->x[0];
        if (id != CLK_EMMC && id != CLK_UART && id != CLK_CORE) { ret = -1; break; }
        ret = (int64_t)mbox_clock_rate((uint32_t)id);
        break;
    }

    /* Leer de la consola. Por ahora la entrada sigue siendo del kernel: es
     * el unico periferico que no se ha cedido, porque de el depende poder
     * decir que algo ha fallado. */
    /* Mover el tope del monton del proceso. Devuelve el tope VIEJO, que es
     * el principio de lo que se acaba de conseguir. */
    /* Bifurcarse. El valor de retorno es lo unico que distingue a los dos:
     * aqui se deja el pid del hijo, y en el frame del hijo se deja un
     * cero. */
    /* Cuanta memoria queda. No es informacion privilegiada -cualquiera
     * puede deducirla pidiendo hasta que falle- y permite que un programa
     * ensenye lo que cuesta de verdad una operacion. */
    case SYS_freepages:
        ret = (int64_t)pmm_free_pages();
        break;

    /* Mandarle una senyal a otro proceso. Cualquiera puede a cualquiera:
     * no hay usuarios ni permisos que comprobar. */
    /* Un numero negativo nombra un GRUPO, que es el convenio de Unix. Y no
     * es un truco sucio: un pid y un pgid viven en el mismo espacio de
     * numeros -un grupo se llama como su primer proceso- asi que para
     * decir cual de los dos es hace falta algo que no sea el numero. El
     * signo estaba libre porque no hay pids negativos. */
    case SYS_kill: {
        int64_t quien = (int64_t)f->x[0];
        ret = (quien < 0) ? task_signal_grupo((uint64_t)-quien, (int)f->x[1])
                          : task_signal((uint64_t)quien, (int)f->x[1]);
        break;
    }

    /* Decir que hacer cuando llegue una. El trampolin lo pone la libreria
     * de usuario, no el programa: es por donde vuelve el manejador. */
    case SYS_signal:
        ret = task_set_handler((int)f->x[0], f->x[1], f->x[2]);
        break;

    /* Lo llama el trampolin cuando el manejador termina. Devuelve el x0
     * que tenia el proceso antes de la interrupcion, y el "f->x[0] = ret"
     * del final lo deja en su sitio. */
    case SYS_sigreturn:
        ret = signal_return(f);
        break;

    case SYS_fork:
        ret = task_fork(f);
        break;

    /* Convertirse en otro programa. Si sale bien no "vuelve": el frame que
     * se restaura al salir de aqui ya es el del programa nuevo. */
    case SYS_exec:
        ret = sys_exec(f);
        break;

    case SYS_sbrk:
        ret = (int64_t)task_sbrk((int64_t)f->x[0]);
        break;

    case SYS_read:
        ret = file_read(task_fd((int)f->x[0]), f->x[1], f->x[2]);
        break;

    /* Una tuberia y sus dos extremos, que se le devuelven al proceso como
     * dos numeros escritos en un array suyo. */
    case SYS_pipe: {
        struct fichero *r = 0, *w = 0;
        if (!user_rango(f->x[0], 2 * sizeof(int))) { ret = -EFAULT; break; }
        if (file_pipe(&r, &w) < 0)                    { ret = -1; break; }

        int fr = task_fd_alloc(r);
        int fw = task_fd_alloc(w);
        if (fr < 0 || fw < 0) {
            if (fr >= 0) task_fd_close(fr); else file_close(r);
            if (fw >= 0) task_fd_close(fw); else file_close(w);
            ret = -1;
            break;
        }

        ((int *)f->x[0])[0] = fr;
        ((int *)f->x[0])[1] = fw;
        ret = 0;
        break;
    }

    /* Abrir un fichero de la tarjeta. Devuelve un descriptor, que es lo
     * que hace que el shell pueda ponerlo en el 0 o en el 1 con dup2 y que
     * el programa no se entere de nada. */
    case SYS_open: {
        char rel[FS_PATH_MAX], abs[FS_PATH_MAX];
        if (copiar_ruta(rel, f->x[0]) < 0) { ret = -1; break; }

        /* Se resuelve contra el directorio actual ANTES de bajar al
         * servidor, que solo entiende rutas absolutas. */
        if (path_resolve(current->cwd, rel, abs, sizeof(abs)) < 0) { ret = -1; break; }

        struct fichero *fi = file_open(abs, (int)f->x[1]);
        if (!fi) { ret = -ENOENT; break; }

        ret = task_fd_alloc(fi);
        if (ret < 0) { file_close(fi); ret = -EMFILE; }
        break;
    }

    /* --- El directorio actual --------------------------------------
     * Las tres juntas porque son la misma idea: el cwd es del proceso, lo
     * guarda el kernel, y el servidor de ficheros no se entera de que
     * existe. */

    case SYS_chdir: {
        char abs[FS_PATH_MAX];
        if (traer_ruta(f->x[0], abs) < 0) { ret = -1; break; }

        /* Y comprobar que existe Y es un directorio. Sin esto, un "cd
         * nada" dejaria al proceso apuntando a un sitio inventado y el
         * error saldria mucho despues, al abrir cualquier cosa. */
        if (!fs_es_directorio(abs)) { ret = -1; break; }

        for (uint64_t i = 0; i < FS_PATH_MAX; i++) current->cwd[i] = abs[i];
        ret = 0;
        break;
    }

    case SYS_getcwd: {
        uint64_t n = f->x[1];
        if (n > FS_PATH_MAX) n = FS_PATH_MAX;
        if (copy_to_user(f->x[0], current->cwd, n) != 0) { ret = -1; break; }
        ret = 0;
        break;
    }

    case SYS_realpath: {
        char abs[FS_PATH_MAX];
        if (traer_ruta(f->x[0], abs) < 0) { ret = -1; break; }
        if (copy_to_user(f->x[1], abs, FS_PATH_MAX) != 0) { ret = -1; break; }
        ret = 0;
        break;
    }

    /* Mapear un fichero. La ruta se resuelve aqui, como en open: el
     * servidor solo entiende absolutas. */
    case SYS_mmap: {
        char abs[FS_PATH_MAX];
        if (traer_ruta(f->x[0], abs) < 0) { ret = -1; break; }

        uint64_t tam = 0;
        int64_t base = task_mmap(abs, &tam);
        if (base < 0) { ret = -1; break; }

        if (f->x[1]) {
            if (copy_to_user(f->x[1], &tam, sizeof(tam)) != 0) { ret = -1; break; }
        }
        ret = base;
        break;
    }

    /* --- Las dos que solo puede pedir init --------------------------
     *
     * La comprobacion es una linea, y es toda la frontera de privilegio
     * que hay entre procesos en este sistema. No hay usuarios, ni grupos,
     * ni capacidades: hay el primero y hay los demas.
     *
     * Es poco, y es honesto decir cuanto es: basta porque init es el
     * unico que puede existir antes de que exista nadie mas. En cuanto
     * hiciera falta un segundo proceso de confianza, esto se quedaria
     * corto y habria que inventar algo de verdad. */
    case SYS_bootstrap:
        ret = sys_bootstrap(f);
        break;

    /* Ceder la consola a un grupo.
     *
     * Antes era "solo init", que es una frontera de privilegio pero no es
     * LA correcta: el shell tiene que poder poner delante al trabajo que
     * acaba de arrancar y recuperarla cuando termine, y no es init.
     *
     * La regla de verdad no habla de quien eres sino de que tienes: puedes
     * dar la consola si tu grupo la tiene ahora mismo. Es la misma idea
     * que un testigo en una carrera -solo lo pasa quien lo lleva- y hace
     * imposible que un proceso de segundo plano se ponga delante solo. A
     * init se le deja igualmente, porque es quien tiene que devolverla
     * cuando el shell entero se muere y no queda nadie que la lleve. */
    case SYS_consola:
        ret = task_dar_consola(f->x[0]);
        break;

    case SYS_time:
        ret = (int64_t)reloj_ahora();
        break;

    /* Solo init: la hora es de la maquina entera, y dejar que cualquiera
     * la cambie seria dejar que cualquiera mueva las fechas de todos los
     * ficheros. */
    case SYS_settime:
        if (!current || current->pid != task_init_pid()) { ret = -1; break; }
        reloj_poner(f->x[0]);
        ret = 0;
        break;

    /* --- Ficheros por su nombre ------------------------------------
     * Las cinco son la misma forma: traerse la ruta, resolverla contra el
     * directorio actual, y pasarsela al servidor. La resolucion se hace
     * aqui y no abajo porque el cwd es del proceso y el servidor no tiene
     * estado. */
    case SYS_stat:
    case SYS_unlink:
    case SYS_mkdir:
    case SYS_rmdir:
    case SYS_rename:
    case SYS_opendir:
        ret = por_nombre(nr, f);
        break;

    case SYS_readdir: {
        struct fs_info info;
        int r = file_readdir(task_fd((int)f->x[0]), &info);
        if (r != 1) { ret = r; break; }
        if (copy_to_user(f->x[1], &info, sizeof(info)) != 0) { ret = -1; break; }
        ret = 1;
        break;
    }

    case SYS_lseek: {
        struct fichero *fi = task_fd((int)f->x[0]);
        if (!fi) { ret = -EBADF; break; }

        /* Una tuberia no se puede rebobinar: los bytes ya no estan. Eso
         * no es EINVAL, es ESPIPE, y existe un codigo aparte justo porque
         * la diferencia importa. */
        if (fi->tipo != F_FICHERO) { ret = -ESPIPE; break; }

        ret = file_seek(fi, (int64_t)f->x[1], (int)f->x[2]);
        if (ret < 0) ret = -EINVAL;
        break;
    }

    case SYS_mmap_anon:
        ret = task_mmap_anon(f->x[0]);
        break;

    case SYS_munmap:
        ret = task_munmap(f->x[0]);
        break;

    case SYS_close:
        ret = task_fd_close((int)f->x[0]);
        break;

    case SYS_dup2:
        ret = task_fd_dup2((int)f->x[0], (int)f->x[1]);
        break;

    /* Esperar a un hijo. Sin esto no hay shell posible: el prompt volveria
     * antes de que el programa hubiera dicho nada. */
    /* Devuelve el codigo con el que salio el hijo, o -1 si nos
     * interrumpieron esperando. */
    case SYS_waitpid: {
        int64_t codigo = -1;
        int     que    = W_SALIDA;
        int     r      = task_wait(f->x[0], &codigo, &que, (int)f->x[1]);

        /* El "que paso" va aparte, en memoria del que pregunta, porque en
         * el valor de retorno ya no cabe: ahi esta el numero. Si no da un
         * sitio donde escribirlo, es que no le interesa. */
        if (r == 0 && f->x[2]) {
            if (!user_rango(f->x[2], sizeof(int))) { ret = -EFAULT; break; }
            if (copy_to_user(f->x[2], &que, sizeof(int)) != 0) { ret = -EFAULT; break; }
        }

        /* El errno pasa por delante del codigo de salida: -EAGAIN quiere
         * decir "no ha terminado", que no es lo mismo que "termino
         * devolviendo -11". Con el convenio del paso 48 los dos caben en
         * el mismo numero sin pisarse. */
        ret = (r < 0) ? r : codigo;
        break;
    }

    /* Los grupos. Mover a alguien de trabajo y preguntar en cual esta: sin
     * privilegios de por medio, porque solo se puede tocar a uno mismo o a
     * un hijo y eso ya es toda la frontera que hace falta. */
    case SYS_setpgid:
        ret = task_set_pgid(f->x[0], f->x[1]);
        break;

    case SYS_getpgid:
        ret = task_get_pgid(f->x[0]);
        break;

    /* --- Servicios para drivers de EL0 -------------------------------
     * Las cuatro llamadas que hacen falta para que un proceso lleve el
     * teclado: pedir la interrupcion, devolverla, entregar lo leido y
     * decir que alguien ha pulsado Ctrl-C. */

    case SYS_irq_register:
        ret = irq_register(f->x[0], (int)f->x[1]);
        break;

    case SYS_irq_ack:
        ret = irq_ack(f->x[0]);
        break;

    case SYS_console_push: {
        char tmp[64];
        uint64_t n = f->x[1];
        if (n > sizeof(tmp)) n = sizeof(tmp);
        if (copy_from_user(tmp, f->x[0], n) != 0) { ret = -1; break; }
        uart_push(tmp, n);
        ret = 0;
        break;
    }

    /* Quien decide que Ctrl-C significa "interrumpe" es el terminal, y el
     * terminal ahora vive en EL0. Lo que NO puede saber un proceso es a
     * quien hay que interrumpir: eso esta en la tabla de procesos, que es
     * del kernel. De ahi el reparto: el driver detecta la tecla, el kernel
     * decide a quien le cae. */
    case SYS_console_int:
        task_console_interrupt();
        ret = 0;
        break;

    /* Ctrl-Z es lo mismo con otra senyal: el driver ve la tecla, el kernel
     * sabe a quien le cae. Que sean dos llamadas y no una con parametro es
     * a proposito: la lista de lo que un driver puede provocar esta escrita
     * en el kernel, y no la elige quien llama. */
    case SYS_console_stop:
        task_console_stop();
        ret = 0;
        break;

    /* El modo del terminal es del TERMINAL, no de quien pregunta: hay uno
     * y lo comparten todos. Por eso solo lo toca quien esta delante, con
     * la misma regla que el Ctrl-C. Si lo pudiera cambiar un proceso de
     * segundo plano, podria dejarte el eco apagado y marcharse. */
    case SYS_termios:
        if (!task_en_primer_plano()) { ret = -EPERM; break; }
        ret = uart_modo((int)(int64_t)f->x[0]);
        break;

    case SYS_mmio_base:
        /* El kernel concede el MMIO al crear el proceso; aqui solo le
         * decimos en que direccion virtual se lo dejo. Un sistema serio
         * usaria capabilities en vez de un permiso fijado al nacer. */
        ret = (int64_t)current->mmio_va;
        break;

    default:
        uart_puts("\n  [kernel] llamada al sistema desconocida: ");
        uart_dec(nr);
        uart_puts("\n");
        ret = -1;
        break;
    }

    /* El valor de retorno se deja en el frame. kernel_exit lo restaurara en
     * x0 al hacer el 'eret', asi que el proceso lo recibe como si fuera el
     * resultado de una funcion normal. */
    f->x[0] = (uint64_t)ret;
}
