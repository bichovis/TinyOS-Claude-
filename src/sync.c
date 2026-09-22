/* sync.c - Colas de espera, mutex, semaforos y canales
 *
 * Hasta ahora, la unica forma que teniamos de proteger algo era tapar las
 * interrupciones: un mazazo que para el sistema entero para arreglar una
 * disputa entre dos hilos. Aqui construimos lo correcto: el hilo que no
 * puede continuar se duerme y deja la CPU a los demas.
 */
#include <stdint.h>
#include "sync.h"
#include "sched.h"
#include "irq.h"

/* ====================== COLAS DE ESPERA ============================== */

void wq_wait(struct waitqueue *wq)
{
    /* Se entra con las IRQ tapadas (responsabilidad del llamante). Eso es
     * lo que cierra la carrera clasica: si entre comprobar la condicion y
     * dormirnos entrara una interrupcion que la cumpliera y despertara a
     * esta cola, nos dormiriamos despues del aviso y no volveriamos jamas. */
    current->state     = TASK_BLOCKED;
    current->wait_next = 0;

    if (wq->tail) wq->tail->wait_next = current;
    else          wq->head = current;
    wq->tail = current;

    /* schedule() salva y restaura DAIF junto con el contexto del hilo, asi
     * que al volver aqui (puede que mucho despues) las IRQ siguen tapadas
     * exactamente igual que las dejamos. */
    schedule();
}

void wq_wake_one(struct waitqueue *wq)
{
    uint64_t f = irq_save();

    struct task *t = wq->head;
    if (t) {
        wq->head = t->wait_next;
        if (!wq->head) wq->tail = 0;
        t->wait_next = 0;
        if (t->state == TASK_BLOCKED)
            t->state = TASK_READY;
    }

    irq_restore(f);
}

void wq_wake_all(struct waitqueue *wq)
{
    uint64_t f = irq_save();

    struct task *t = wq->head;
    while (t) {
        struct task *next = t->wait_next;
        t->wait_next = 0;
        if (t->state == TASK_BLOCKED)
            t->state = TASK_READY;
        t = next;
    }
    wq->head = wq->tail = 0;

    irq_restore(f);
}

/* ============================ MUTEX ================================== */

void mutex_init(struct mutex *m)
{
    m->locked = 0;
    m->owner  = 0;
    m->waiters.head = m->waiters.tail = 0;
}

void mutex_lock(struct mutex *m)
{
    uint64_t f = irq_save();

    /* 'while' y no 'if': al despertarnos, otro hilo puede habernos ganado
     * el mutex por delante. Hay que volver a comprobarlo siempre. */
    while (m->locked)
        wq_wait(&m->waiters);

    m->locked = 1;
    m->owner  = current;

    irq_restore(f);
}

int mutex_trylock(struct mutex *m)
{
    uint64_t f = irq_save();
    int got = 0;

    if (!m->locked) {
        m->locked = 1;
        m->owner  = current;
        got = 1;
    }

    irq_restore(f);
    return got;
}

void mutex_unlock(struct mutex *m)
{
    uint64_t f = irq_save();

    m->locked = 0;
    m->owner  = 0;
    wq_wake_one(&m->waiters);

    irq_restore(f);
}

/* ========================== SEMAFOROS ================================ */

void sem_init(struct semaphore *s, int64_t initial)
{
    s->count = initial;
    s->waiters.head = s->waiters.tail = 0;
}

void sem_wait(struct semaphore *s)
{
    uint64_t f = irq_save();

    while (s->count == 0)
        wq_wait(&s->waiters);
    s->count--;

    irq_restore(f);
}

void sem_post(struct semaphore *s)
{
    uint64_t f = irq_save();

    s->count++;
    wq_wake_one(&s->waiters);

    irq_restore(f);
}

/* =========================== CANALES =================================
 * Un canal es una cola circular con DOS colas de espera: una para quien
 * quiere enviar y no cabe, otra para quien quiere recibir y no hay nada.
 * Este es el esqueleto del IPC de un microkernel; cuando haya procesos de
 * usuario, la misma estructura vivira detras de una llamada al sistema.
 */

void chan_init(struct channel *c)
{
    c->head = c->tail = c->count = 0;
    c->sent = c->received = 0;
    c->senders.head   = c->senders.tail   = 0;
    c->receivers.head = c->receivers.tail = 0;
}

void chan_send(struct channel *c, uint64_t msg)
{
    uint64_t f = irq_save();

    while (c->count == CHAN_CAPACITY)
        wq_wait(&c->senders);           /* lleno: esperar a que alguien lea */

    c->slot[c->tail] = msg;
    c->tail = (c->tail + 1) % CHAN_CAPACITY;
    c->count++;
    c->sent++;

    wq_wake_one(&c->receivers);         /* hay comida */

    irq_restore(f);
}

int chan_try_send(struct channel *c, uint64_t msg)
{
    uint64_t f = irq_save();
    int ok = 0;

    if (c->count < CHAN_CAPACITY) {
        c->slot[c->tail] = msg;
        c->tail = (c->tail + 1) % CHAN_CAPACITY;
        c->count++;
        c->sent++;
        wq_wake_one(&c->receivers);
        ok = 1;
    }

    irq_restore(f);
    return ok;
}

uint64_t chan_recv(struct channel *c)
{
    uint64_t f = irq_save();

    while (c->count == 0)
        wq_wait(&c->receivers);         /* vacio: esperar a que alguien envie */

    uint64_t msg = c->slot[c->head];
    c->head = (c->head + 1) % CHAN_CAPACITY;
    c->count--;
    c->received++;

    wq_wake_one(&c->senders);           /* hay hueco */

    irq_restore(f);
    return msg;
}
