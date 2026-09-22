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

extern struct mutex *console_mutex(void);

/* Un puntero que viene de EL0 NO es de fiar: puede apuntar al kernel, a un
 * periferico o a memoria de otro proceso. Antes de tocarlo hay que
 * comprobar que el proceso tiene derecho a leerlo, y eso lo sabe la MMU.
 * 'at s1e0r' traduce como lo haria EL0; si falla, el puntero es invalido
 * por muy legible que sea para el kernel. */
static int user_readable(uint64_t va, uint64_t len)
{
    if (len == 0) return 1;
    if (va < USER_BASE || va >= USER_LIMIT) return 0;
    if (va + len < va || va + len > USER_LIMIT) return 0;   /* desbordamiento */

    /* Comprobar pagina a pagina: el rango puede cruzar varias. */
    for (uint64_t p = va & ~(PAGE_SIZE - 1); p < va + len; p += PAGE_SIZE)
        if (!vmm_translate_user(p))
            return 0;
    return 1;
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
