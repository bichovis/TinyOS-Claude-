/* file.c - Descriptores de fichero y tuberias
 *
 * Una tuberia es una cola de bytes con dos extremos y una regla que la
 * hace util: quien lee se para si no hay nada, y quien escribe se para si
 * no cabe. Esa espera es todo el mecanismo de sincronizacion que hace
 * falta para encadenar programas, y por eso "a | b" funciona sin que a y
 * b sepan el uno del otro.
 *
 * El detalle que la cierra es el final de fichero: cuando se cierra el
 * ULTIMO extremo de escritura, quien lee deja de esperar y recibe un cero.
 * Sin eso, el segundo programa de una tuberia no terminaria nunca.
 */
#include <stdint.h>
#include "file.h"
#include "sched.h"
#include "mm.h"
#include "uart.h"
#include "spinlock.h"

/* La consola es una sola para todo el mundo y no se cierra nunca. */
static struct fichero consola = { F_CONSOLA, 1, 0 };

struct fichero *file_consola(void) { return &consola; }

void file_init(void) { }

/* Copiar entre el espacio del proceso y el kernel con un buffer de por
 * medio.
 *
 * El rodeo no es capricho: las colas de la tuberia se manejan con
 * sched_lock cogido, y tocar memoria de usuario ahi dentro puede provocar
 * un fallo de pagina -la pila crece, una pagina es COW- que acabaria
 * pidiendo ese mismo cerrojo. Un interbloqueo contra uno mismo. Asi que
 * primero se copia fuera, y luego se entra. */
#define BOUNCE 128

static uint64_t copiar_de_usuario(char *dst, uint64_t uva, uint64_t n)
{
    uint64_t i = 0;
    while (i < n) {
        if (!vmm_translate_user(uva + i)) break;
        dst[i] = ((const char *)uva)[i];
        i++;
    }
    return i;
}

static uint64_t copiar_a_usuario(uint64_t uva, const char *src, uint64_t n)
{
    uint64_t i = 0;
    while (i < n) {
        /* user_touch_w y no vmm_translate_user_w: despues de un fork la
         * pagina puede estar compartida y de solo lectura, y entonces hay
         * que darle su copia antes de escribir. */
        if (!user_touch_w(uva + i)) break;
        ((char *)uva)[i] = src[i];
        i++;
    }
    return i;
}

/* --- La tuberia ------------------------------------------------------- */

static int64_t pipe_read(struct pipe *p, uint64_t uva, uint64_t n)
{
    char tmp[BOUNCE];
    if (n > BOUNCE) n = BOUNCE;

    uint64_t flags = sched_lock_irqsave();

    while (p->count == 0) {
        /* Nadie va a escribir nunca mas: esto es el final del fichero, y
         * es lo que hace que el segundo programa de una tuberia termine. */
        if (p->escritores == 0) { sched_unlock_irqrestore(flags); return 0; }

        if (wq_wait(&p->hay_datos) < 0) {
            sched_unlock_irqrestore(flags);
            return -1;                      /* nos interrumpio una senyal */
        }
    }

    uint64_t i = 0;
    while (i < n && p->count) {
        tmp[i++] = p->buf[p->tail];
        p->tail  = (p->tail + 1) % PIPE_BUF;
        p->count--;
    }

    wq_wake_all(&p->hay_hueco);
    sched_unlock_irqrestore(flags);

    return (int64_t)copiar_a_usuario(uva, tmp, i);
}

static int64_t pipe_write(struct pipe *p, uint64_t uva, uint64_t n)
{
    char tmp[BOUNCE];
    if (n > BOUNCE) n = BOUNCE;

    uint64_t hay = copiar_de_usuario(tmp, uva, n);
    if (!hay) return -1;

    uint64_t flags = sched_lock_irqsave();

    while (p->count == PIPE_BUF) {
        /* Escribir en una tuberia que ya no lee nadie no tiene sentido.
         * Un Unix manda SIGPIPE; aqui basta con decir que fallo. */
        if (p->lectores == 0) { sched_unlock_irqrestore(flags); return -1; }

        if (wq_wait(&p->hay_hueco) < 0) {
            sched_unlock_irqrestore(flags);
            return -1;
        }
    }

    uint64_t i = 0;
    while (i < hay && p->count < PIPE_BUF) {
        p->buf[p->head] = tmp[i++];
        p->head = (p->head + 1) % PIPE_BUF;
        p->count++;
    }

    wq_wake_all(&p->hay_datos);
    sched_unlock_irqrestore(flags);
    return (int64_t)i;
}

/* --- La consola ------------------------------------------------------- */

static int64_t consola_write(uint64_t uva, uint64_t n)
{
    char tmp[BOUNCE];
    if (n > BOUNCE) n = BOUNCE;

    uint64_t hay = copiar_de_usuario(tmp, uva, n);
    if (!hay) return -1;

    uint64_t lf = uart_begin();
    for (uint64_t i = 0; i < hay; i++) {
        if (tmp[i] == '\n') uart_putc('\r');
        uart_putc(tmp[i]);
    }
    uart_end(lf);
    return (int64_t)hay;
}

static int64_t consola_read(uint64_t uva, uint64_t n)
{
    if (n == 0) return 0;

    int c = uart_getc_blocking();
    if (c < 0) return -1;                   /* senyal */

    char b = (char)c;
    return (int64_t)copiar_a_usuario(uva, &b, 1);
}

/* --- La interfaz comun ------------------------------------------------ */

int64_t file_read(struct fichero *f, uint64_t uva, uint64_t n)
{
    if (!f) return -1;
    if (f->tipo == F_CONSOLA) return consola_read(uva, n);
    if (f->tipo == F_PIPE_R)  return pipe_read(f->p, uva, n);
    return -1;                              /* el extremo que no toca */
}

int64_t file_write(struct fichero *f, uint64_t uva, uint64_t n)
{
    if (!f) return -1;
    if (f->tipo == F_CONSOLA) return consola_write(uva, n);
    if (f->tipo == F_PIPE_W)  return pipe_write(f->p, uva, n);
    return -1;
}

void file_dup(struct fichero *f)
{
    if (!f || f == &consola) return;        /* la consola no se cuenta */

    uint64_t flags = sched_lock_irqsave();
    f->refs++;
    sched_unlock_irqrestore(flags);
}

void file_close(struct fichero *f)
{
    if (!f || f == &consola) return;

    uint64_t flags = sched_lock_irqsave();

    if (--f->refs > 0) { sched_unlock_irqrestore(flags); return; }

    /* Este era el ultimo descriptor de este extremo. Avisar al otro lado:
     * si era el de escritura, quien lee tiene que despertarse para
     * enterarse de que se acabo. */
    struct pipe *p = f->p;
    if (p) {
        if (f->tipo == F_PIPE_R) { p->lectores--;   wq_wake_all(&p->hay_hueco); }
        else                     { p->escritores--; wq_wake_all(&p->hay_datos); }
    }

    int muerta = p && p->lectores == 0 && p->escritores == 0;
    sched_unlock_irqrestore(flags);

    kfree(f);
    if (muerta) kfree(p);
}

int file_pipe(struct fichero **lectura, struct fichero **escritura)
{
    struct pipe    *p = kmalloc(sizeof(struct pipe));
    struct fichero *r = kmalloc(sizeof(struct fichero));
    struct fichero *w = kmalloc(sizeof(struct fichero));

    if (!p || !r || !w) { kfree(p); kfree(r); kfree(w); return -1; }

    p->head = p->tail = p->count = 0;
    p->lectores = p->escritores = 1;
    p->hay_datos.head = p->hay_datos.tail = 0;
    p->hay_hueco.head = p->hay_hueco.tail = 0;

    r->tipo = F_PIPE_R; r->refs = 1; r->p = p;
    w->tipo = F_PIPE_W; w->refs = 1; w->p = p;

    *lectura = r;
    *escritura = w;
    return 0;
}
