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
    case SYS_spawn: {
        uint64_t buf = f->x[0], len = f->x[1], uargs = f->x[2];

        if (len == 0 || len > 256 * 1024) { ret = -1; break; }
        if (!user_rango(buf, len))        { ret = -1; break; }

        struct args args, entorno;
        if (copiar_args(&args, uargs) < 0)            { ret = -1; break; }
        if (copiar_args(&entorno, f->x[3]) < 0)       { ret = -1; break; }

        ret = task_create_user(0, (const uint8_t *)buf, len, 0, &args, &entorno);
        break;
    }

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
    case SYS_kill:
        ret = task_signal(f->x[0], (int)f->x[1]);
        break;

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
    case SYS_exec: {
        uint64_t buf = f->x[0], len = f->x[1], uargs = f->x[2];

        if (len == 0 || len > 1024 * 1024) { ret = -1; break; }
        if (!user_rango(buf, len))         { ret = -1; break; }

        /* Los argumentos se copian ANTES de tocar nada del proceso, que
         * es lo unico que hace segura esta llamada: exec destruye el
         * espacio de direcciones de donde salen. */
        /* Los dos lotes se copian ANTES de tocar nada del proceso, que es
         * lo unico que hace segura esta llamada: exec destruye el espacio
         * de direcciones de donde salen.
         *
         * Y el entorno se copia igual que los argumentos, no se hereda
         * solo: exec lo REEMPLAZA. Que en la practica casi siempre sea el
         * mismo es cosa del shell, que le pasa el suyo; el kernel no da
         * nada por hecho. */
        struct args args, entorno;
        if (copiar_args(&args, uargs) < 0)      { ret = -1; break; }
        if (copiar_args(&entorno, f->x[3]) < 0) { ret = -1; break; }

        ret = task_exec((const uint8_t *)buf, len, &args, &entorno, f);
        break;
    }

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
        if (!user_rango(f->x[0], 2 * sizeof(int))) { ret = -1; break; }
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
        if (!fi) { ret = -1; break; }

        ret = task_fd_alloc(fi);
        if (ret < 0) file_close(fi);     /* no habia descriptor libre */
        break;
    }

    /* --- El directorio actual --------------------------------------
     * Las tres juntas porque son la misma idea: el cwd es del proceso, lo
     * guarda el kernel, y el servidor de ficheros no se entera de que
     * existe. */

    case SYS_chdir: {
        char rel[FS_PATH_MAX], abs[FS_PATH_MAX];
        if (copiar_ruta(rel, f->x[0]) < 0) { ret = -1; break; }
        if (path_resolve(current->cwd, rel, abs, sizeof(abs)) < 0) { ret = -1; break; }

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
        char rel[FS_PATH_MAX], abs[FS_PATH_MAX];
        if (copiar_ruta(rel, f->x[0]) < 0) { ret = -1; break; }
        if (path_resolve(current->cwd, rel, abs, sizeof(abs)) < 0) { ret = -1; break; }
        if (copy_to_user(f->x[1], abs, FS_PATH_MAX) != 0) { ret = -1; break; }
        ret = 0;
        break;
    }

    /* Mapear un fichero. La ruta se resuelve aqui, como en open: el
     * servidor solo entiende absolutas. */
    case SYS_mmap: {
        char rel[FS_PATH_MAX], abs[FS_PATH_MAX];
        if (copiar_ruta(rel, f->x[0]) < 0) { ret = -1; break; }
        if (path_resolve(current->cwd, rel, abs, sizeof(abs)) < 0) { ret = -1; break; }

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
    case SYS_bootstrap: {
        if (!current || current->pid != task_init_pid()) { ret = -1; break; }

        char nombre[16];
        if (copiar_cadena(nombre, f->x[0], sizeof(nombre)) < 0) { ret = -1; break; }

        struct args args, entorno;
        if (copiar_args(&args, f->x[1]) < 0)    { ret = -1; break; }
        if (copiar_args(&entorno, f->x[2]) < 0) { ret = -1; break; }

        /* argv vacio: que el programa se llame como pidio init. */
        if (args.n == 0) args_de_cadena(&args, nombre);

        ret = task_bootstrap(nombre, &args, &entorno, f->x[3]);
        break;
    }

    case SYS_consola:
        if (!current || current->pid != task_init_pid()) { ret = -1; break; }
        task_set_console(f->x[0]);
        ret = 0;
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
        ret = (task_wait(f->x[0], &codigo) < 0) ? -1 : codigo;
        break;
    }

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
