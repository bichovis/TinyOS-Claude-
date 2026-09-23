/* ipc.c - Puertos de mensajes entre procesos
 *
 * Es el mismo patron que el 'channel' del paso 6 (cola circular con dos
 * colas de espera), con dos diferencias que solo aparecen cuando los
 * interlocutores son procesos de verdad y no hilos del kernel:
 *
 *   1. Los mensajes se COPIAN. Emisor y receptor viven en espacios de
 *      direcciones distintos: no hay ningun puntero que ambos puedan
 *      entender, asi que el kernel hace de correo.
 *   2. Hay un duenyo. Cualquiera puede enviar a un puerto, pero solo quien
 *      lo creo puede recibir de el; si no, un proceso podria robarle los
 *      mensajes a un servidor.
 *
 * Esto es lo minimo que un microkernel tiene que ofrecer para que los
 * servicios puedan vivir fuera de el.
 */
#include <stdint.h>
#include "ipc.h"
#include "mm.h"
#include "irq.h"
#include "sched.h"
#include "irq.h"
#include "spinlock.h"
#include "uart.h"

static struct port ports[MAX_PORTS];

static void copy_msg(struct message *dst, const struct message *src)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < sizeof(struct message); i++)
        d[i] = s[i];
}

void ipc_init(void)
{
    for (int i = 0; i < MAX_PORTS; i++)
        ports[i].in_use = 0;
}

/* 'want' es el puerto que se pide, o -1 para cualquiera.
 *
 * Poder pedir uno concreto es lo que permite que existan puertos CONOCIDOS:
 * un cliente tiene que saber a donde escribirle al servidor de ficheros sin
 * preguntarle a nadie. Si el numero dependiera del orden de arranque, todo
 * el invento se vendria abajo el dia que se arranque al reves. */
int port_create(uint64_t owner_pid, int64_t want)
{
    /* La cola se pide ANTES de coger el cerrojo del planificador: kmalloc
     * puede tener que ir al gestor de paginas y borrar 64 KB, y eso con
     * los otros tres nucleos parados no. */
    struct message *cola = kmalloc(sizeof(struct message) * PORT_QUEUE);
    if (!cola) return -1;

    uint64_t f = sched_lock_irqsave();
    int id = -1;

    if (want >= 0) {
        if (want < MAX_PORTS && !ports[want].in_use) {
            struct port *p = &ports[want];
            p->q      = cola;
            p->in_use = 1;
            p->owner  = owner_pid;
            p->head = p->tail = p->count = 0;
            p->sent = p->received = 0;
            p->receivers.head = p->receivers.tail = 0;
            p->senders.head   = p->senders.tail   = 0;
            id = (int)want;
        }
        sched_unlock_irqrestore(f);
        if (id < 0) kfree(cola);         /* el puerto estaba cogido */
        return id;
    }

    for (int i = 0; i < MAX_PORTS; i++) {
        if (!ports[i].in_use) {
            struct port *p = &ports[i];
            p->q      = cola;
            p->in_use = 1;
            p->owner  = owner_pid;
            p->head = p->tail = p->count = 0;
            p->sent = p->received = 0;
            p->receivers.head = p->receivers.tail = 0;
            p->senders.head   = p->senders.tail   = 0;
            id = i;
            break;
        }
    }

    sched_unlock_irqrestore(f);
    if (id < 0) kfree(cola);             /* no habia ni un puerto libre */
    return id;
}

int port_send(int id, const struct message *m)
{
    if (id < 0 || id >= MAX_PORTS) return -1;
    struct port *p = &ports[id];

    uint64_t f = sched_lock_irqsave();
    if (!p->in_use) { sched_unlock_irqrestore(f); return -1; }

    /* Si el buzon esta lleno, el emisor espera. Eso da control de flujo:
     * un cliente desbocado se frena solo en vez de tumbar al servidor. */
    while (p->count == PORT_QUEUE) {
        if (wq_wait(&p->senders) < 0) { sched_unlock_irqrestore(f); return -1; }
        if (!p->in_use) { sched_unlock_irqrestore(f); return -1; }  /* murio el servidor */
    }

    copy_msg(&p->q[p->tail], m);
    p->tail = (p->tail + 1) % PORT_QUEUE;
    p->count++;
    p->sent++;

    wq_wake_one(&p->receivers);
    sched_unlock_irqrestore(f);
    return 0;
}

int port_recv(int id, struct message *out, uint64_t pid)
{
    if (id < 0 || id >= MAX_PORTS) return -1;
    struct port *p = &ports[id];

    uint64_t f = sched_lock_irqsave();
    if (!p->in_use || p->owner != pid) { sched_unlock_irqrestore(f); return -1; }

    while (p->count == 0) {
        if (wq_wait(&p->receivers) < 0) { sched_unlock_irqrestore(f); return -1; }
        if (!p->in_use || p->owner != pid) { sched_unlock_irqrestore(f); return -1; }
    }

    copy_msg(out, &p->q[p->head]);
    p->head = (p->head + 1) % PORT_QUEUE;
    p->count--;
    p->received++;

    wq_wake_one(&p->senders);
    sched_unlock_irqrestore(f);
    return 0;
}

/* Un servidor puede morir. Sus puertos desaparecen y hay que despertar a
 * todo el que estuviera esperando, o se quedarian bloqueados para siempre
 * esperando a alguien que ya no existe.
 *
 * Se llama desde task_exit(), que YA tiene sched_lock cogido y no lo va a
 * soltar hasta despues del cambio de contexto. Pedirlo aqui otra vez seria
 * un interbloqueo contra uno mismo. */
int port_notify(int id, uint64_t tipo)
{
    if (id < 0 || id >= MAX_PORTS) return -1;

    uint64_t f = sched_lock_irqsave();
    struct port *p = &ports[id];
    int ok = -1;

    if (p->in_use && p->count < PORT_QUEUE) {
        struct message *m = &p->q[p->tail];
        m->from = 0;                     /* viene del kernel */
        m->type = tipo;
        m->len  = 0;
        p->tail = (p->tail + 1) % PORT_QUEUE;
        p->count++;
        p->sent++;
        wq_wake_one(&p->receivers);
        ok = 0;
    }

    sched_unlock_irqrestore(f);
    return ok;
}

void ipc_release_ports(uint64_t pid)
{
    for (int i = 0; i < MAX_PORTS; i++) {
        if (ports[i].in_use && ports[i].owner == pid) {
            ports[i].in_use = 0;
            wq_wake_all(&ports[i].senders);
            wq_wake_all(&ports[i].receivers);

            /* Y devolver la cola. Se coge heap_lock teniendo sched_lock, y
             * ese orden es el bueno: nadie pide el del planificador
             * teniendo el del monton, asi que no hay abrazo posible. */
            kfree(ports[i].q);
            ports[i].q = 0;

            /* Si este puerto era el de un driver, devolverle la
             * interrupcion al kernel. Sin esto, un driver que se muere se
             * lleva el periferico con el: la fuente quedaria enmascarada
             * esperando un irq_ack que ya no va a llegar nunca, y el
             * teclado dejaria de existir hasta el siguiente reinicio.
             *
             * Que un driver pueda morirse es la gracia de tenerlo en EL0;
             * solo cuenta si el sistema sabe recoger lo que suelta. */
            irq_release_port(i);
        }
    }
}

void ipc_dump(void)
{
    uint64_t f = sched_lock_irqsave();

    uart_puts("\n  puerto  duenyo  cola  enviados  recibidos\n");
    for (int i = 0; i < MAX_PORTS; i++) {
        if (!ports[i].in_use) continue;
        uart_puts("    ");
        uart_dec((uint64_t)i);
        uart_puts("       ");
        uart_dec(ports[i].owner);
        uart_puts("      ");
        uart_dec(ports[i].count);
        uart_puts("/8    ");
        uart_dec(ports[i].sent);
        uart_puts("         ");
        uart_dec(ports[i].received);
        uart_puts("\n");
    }

    sched_unlock_irqrestore(f);
}
