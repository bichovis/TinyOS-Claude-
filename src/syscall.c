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
#include "ipc.h"
#include "mbox.h"

extern struct mutex *console_mutex(void);

/* Un puntero que viene de EL0 NO es de fiar: puede apuntar al kernel, a un
 * periferico o a memoria de otro proceso. Antes de tocarlo hay que
 * comprobar que el proceso tiene derecho a leerlo, y eso lo sabe la MMU.
 * 'at s1e0r' traduce como lo haria EL0; si falla, el puntero es invalido
 * por muy legible que sea para el kernel. */
static int user_range_ok(uint64_t va, uint64_t len, int for_write)
{
    if (len == 0) return 1;
    if (va < USER_BASE || va >= USER_LIMIT) return 0;
    if (va + len < va || va + len > USER_LIMIT) return 0;   /* desbordamiento */

    /* Comprobar pagina a pagina: el rango puede cruzar varias. */
    for (uint64_t p = va & ~(PAGE_SIZE - 1); p < va + len; p += PAGE_SIZE) {
        uint64_t ok = for_write ? vmm_translate_user_w(p) : vmm_translate_user(p);
        if (!ok) return 0;
    }
    return 1;
}

static int user_readable(uint64_t va, uint64_t len) { return user_range_ok(va, len, 0); }
static int user_writable(uint64_t va, uint64_t len) { return user_range_ok(va, len, 1); }

/* --- Copias entre espacios de direcciones -----------------------------
 * Emisor y receptor no comparten ni una direccion, asi que el kernel copia
 * byte a byte desde el espacio activo a una variable propia (que vive en la
 * pila de kernel, visible siempre) y luego al destino. */
static void copy_bytes(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
}

static int64_t sys_send(uint64_t port, uint64_t umsg)
{
    if (!user_readable(umsg, sizeof(struct message))) return -1;

    struct message m;
    copy_bytes(&m, (const void *)umsg, sizeof(m));
    m.from = current->pid;          /* el remitente NO se puede falsificar */
    if (m.len > MSG_DATA_MAX) m.len = MSG_DATA_MAX;

    return port_send((int)port, &m);
}

static int64_t sys_recv(uint64_t port, uint64_t umsg)
{
    if (!user_writable(umsg, sizeof(struct message))) return -1;

    struct message m;
    if (port_recv((int)port, &m, current->pid) < 0)
        return -1;

    /* Ojo: port_recv puede haber bloqueado al proceso y, al despertar,
     * seguimos en su espacio de direcciones porque schedule() restaura
     * TTBR0 con el contexto. Por eso este puntero sigue siendo valido. */
    copy_bytes((void *)umsg, &m, sizeof(m));
    return 0;
}

static int64_t sys_write(uint64_t buf, uint64_t len)
{
    if (len > 512) len = 512;
    if (!user_readable(buf, len))
        return -1;                       /* el proceso miente: no lo servimos */

    const char *s = (const char *)buf;
    struct mutex *m = console_mutex();

    mutex_lock(m);
    for (uint64_t i = 0; i < len; i++) {
        if (s[i] == '\n') uart_putc('\r');
        uart_putc(s[i]);
    }
    mutex_unlock(m);
    return (int64_t)len;
}

void syscall_dispatch(struct trap_frame *f)
{
    uint64_t nr = f->x[8];
    uint64_t a0 = f->x[0], a1 = f->x[1];
    int64_t  ret = -1;

    switch (nr) {
    case SYS_write:
        ret = sys_write(a0, a1);
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
        task_exit();                     /* no vuelve */
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
        if (!user_readable(buf, len))     { ret = -1; break; }

        /* La linea de argumentos se copia a una variable nuestra antes de
         * usarla. Comprobar pagina a pagina mientras se copia, porque una
         * cadena puede acabarse en cualquier sitio y el proceso podria
         * habernos dado un puntero que se sale a mitad. */
        char args[128];
        uint64_t i = 0;
        for (; i < sizeof(args) - 1; i++) {
            if (i == 0 || ((uargs + i) & (PAGE_SIZE - 1)) == 0)
                if (!uargs || !user_readable(uargs + i, 1)) break;
            char c = ((const char *)uargs)[i];
            if (!c) break;
            args[i] = c;
        }
        args[i] = 0;

        ret = task_create_user(0, (const uint8_t *)buf, len, 0, args);
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

    case SYS_fork:
        ret = task_fork(f);
        break;

    case SYS_sbrk:
        ret = (int64_t)task_sbrk((int64_t)f->x[0]);
        break;

    case SYS_read:
        ret = (int64_t)(uint8_t)uart_getc_blocking();
        break;

    /* Esperar a un hijo. Sin esto no hay shell posible: el prompt volveria
     * antes de que el programa hubiera dicho nada. */
    case SYS_waitpid:
        ret = task_wait(f->x[0]);
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
