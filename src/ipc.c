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
#include "sched.h"
#include "irq.h"
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

int port_create(uint64_t owner_pid)
{
    uint64_t f = irq_save();
    int id = -1;

    for (int i = 0; i < MAX_PORTS; i++) {
        if (!ports[i].in_use) {
            struct port *p = &ports[i];
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

    irq_restore(f);
    return id;
}

int port_send(int id, const struct message *m)
{
    if (id < 0 || id >= MAX_PORTS) return -1;
    struct port *p = &ports[id];

    uint64_t f = irq_save();
    if (!p->in_use) { irq_restore(f); return -1; }

    /* Si el buzon esta lleno, el emisor espera. Eso da control de flujo:
     * un cliente desbocado se frena solo en vez de tumbar al servidor. */
    while (p->count == PORT_QUEUE) {
        wq_wait(&p->senders);
        if (!p->in_use) { irq_restore(f); return -1; }  /* murio el servidor */
    }

    copy_msg(&p->q[p->tail], m);
    p->tail = (p->tail + 1) % PORT_QUEUE;
    p->count++;
    p->sent++;

    wq_wake_one(&p->receivers);
    irq_restore(f);
    return 0;
}

int port_recv(int id, struct message *out, uint64_t pid)
{
    if (id < 0 || id >= MAX_PORTS) return -1;
    struct port *p = &ports[id];

    uint64_t f = irq_save();
    if (!p->in_use || p->owner != pid) { irq_restore(f); return -1; }

    while (p->count == 0) {
        wq_wait(&p->receivers);
        if (!p->in_use || p->owner != pid) { irq_restore(f); return -1; }
    }

    copy_msg(out, &p->q[p->head]);
    p->head = (p->head + 1) % PORT_QUEUE;
    p->count--;
    p->received++;

    wq_wake_one(&p->senders);
    irq_restore(f);
    return 0;
}

/* Un servidor puede morir. Sus puertos desaparecen y hay que despertar a
 * todo el que estuviera esperando, o se quedarian bloqueados para siempre
 * esperando a alguien que ya no existe. */
void ipc_release_ports(uint64_t pid)
{
    uint64_t f = irq_save();

    for (int i = 0; i < MAX_PORTS; i++) {
        if (ports[i].in_use && ports[i].owner == pid) {
            ports[i].in_use = 0;
            wq_wake_all(&ports[i].senders);
            wq_wake_all(&ports[i].receivers);
        }
    }

    irq_restore(f);
}

void ipc_dump(void)
{
    uint64_t f = irq_save();

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

    irq_restore(f);
}
