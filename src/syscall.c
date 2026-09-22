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
        uart_puts("\n  [kernel] el proceso ");
        uart_puts(current->name);
        uart_puts(" ha terminado con codigo ");
        uart_dec(a0);
        uart_puts("\n");
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
        ret = port_create(current->pid);
        break;

    case SYS_send:
        ret = sys_send(a0, a1);
        break;

    case SYS_recv:
        ret = sys_recv(a0, a1);
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
