/* sync.h - Sincronizacion y comunicacion entre hilos
 *
 * Todo lo de aqui se apoya en una sola idea: una lista de hilos dormidos
 * esperando algo, y alguien que los despierta cuando ese algo ocurre.
 */
#pragma once
#include <stdint.h>

struct task;

/* --- Cola de espera: la primitiva de la que salen todas las demas ------ */
struct waitqueue {
    struct task *head;        /* FIFO: se despierta al que lleva mas tiempo */
    struct task *tail;
};

/* Duerme al hilo actual en la cola. DEBE llamarse con las IRQ tapadas, y
 * al volver siguen tapadas: eso es lo que hace atomico el par
 * "comprobar condicion / dormirse" y evita perder el despertar. */
/* Devuelve 0 si lo han despertado como es debido, y -1 si lo ha despertado
 * una SENYAL. Esa segunda posibilidad es lo que hace que un proceso
 * bloqueado se pueda matar: sin ella, quien se duerme esperando algo que
 * no llega no se entera nunca de nada.
 *
 * Quien lo llama tiene que mirar el valor. Volver de aqui con -1 significa
 * que la condicion NO se ha cumplido, y la llamada al sistema que estuviera
 * en curso debe abandonar y decir que la interrumpieron. */
int  wq_wait(struct waitqueue *wq);

/* La version que no se deja interrumpir, para los cerrojos internos del
 * kernel: son cortos, no dependen de nadie de fuera, y dejarlos a medias
 * seria peor que esperar. */
void wq_wait_uninterruptible(struct waitqueue *wq);

/* Sacar a una tarea de la cola en la que este durmiendo. Lo usa el reparto
 * de senyales. Con sched_lock cogido. */
void wq_remove(struct task *t);
void wq_wake_one(struct waitqueue *wq);   /* seguro desde un handler IRQ    */
void wq_wake_all(struct waitqueue *wq);

/* --- Mutex: exclusion mutua con bloqueo -------------------------------- */
struct mutex {
    int          locked;
    struct task *owner;
    struct waitqueue waiters;
};

void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
int  mutex_trylock(struct mutex *m);      /* 1 si lo consiguio, 0 si no     */
void mutex_unlock(struct mutex *m);

/* --- Semaforo contador ------------------------------------------------- */
struct semaphore {
    int64_t count;
    struct waitqueue waiters;
};

void sem_init(struct semaphore *s, int64_t initial);
void sem_wait(struct semaphore *s);       /* baja el contador, o espera     */
void sem_post(struct semaphore *s);       /* lo sube y despierta a uno      */

/* --- Canal de mensajes: la base del IPC -------------------------------- */
#define CHAN_CAPACITY 8

struct channel {
    uint64_t slot[CHAN_CAPACITY];
    uint32_t head, tail, count;
    uint64_t sent, received;              /* estadisticas                   */
    struct waitqueue senders;             /* esperando hueco                */
    struct waitqueue receivers;           /* esperando mensaje              */
};

void     chan_init(struct channel *c);
void     chan_send(struct channel *c, uint64_t msg);   /* espera si lleno   */
uint64_t chan_recv(struct channel *c);                 /* espera si vacio   */
int      chan_try_send(struct channel *c, uint64_t msg);
