/* ipc.h - Puertos de mensajes (lado kernel) */
#pragma once
#include <stdint.h>
#include "ipc_abi.h"
#include "sync.h"

#define MAX_PORTS      8
#define PORT_QUEUE     8

struct port {
    int          in_use;
    uint64_t     owner;                  /* pid que puede recibir de el     */
    struct message q[PORT_QUEUE];
    uint32_t     head, tail, count;
    uint64_t     sent, received;
    struct waitqueue receivers;          /* el duenyo, esperando mensaje    */
    struct waitqueue senders;            /* clientes, esperando hueco       */
};

void ipc_init(void);
int  port_create(uint64_t owner_pid, int64_t want);                       /* id o -1      */
int  port_send(int id, const struct message *m);            /* 0 o -1       */
int  port_recv(int id, struct message *out, uint64_t pid);  /* 0 o -1       */
void ipc_release_ports(uint64_t pid);   /* al morir un proceso              */
void ipc_dump(void);
