/* ipc.h - Puertos de mensajes (lado kernel) */
#pragma once
#include <stdint.h>
#include "ipc_abi.h"
#include "sync.h"

#define MAX_PORTS      8
#define PORT_QUEUE     8

/* El puerto por el que el KERNEL recibe respuestas del servidor de
 * ficheros. Su duenyo es el pid 0, que no existe -los procesos empiezan en
 * CORES-, asi que ningun proceso puede vaciarlo.
 *
 * Esta reservado: port_create(-1) lo salta. Sin eso se lo lleva el primer
 * proceso que pida un puerto sin decir cual, que resulta ser el shell, y
 * la redireccion falla con un "no puedo escribir" que no dice por que. */
#define PORT_KERNEL    2

struct port {
    int          in_use;
    uint64_t     owner;                  /* pid que puede recibir de el     */
    struct message *q;                   /* la cola, del monton del kernel  */
    uint32_t     head, tail, count;
    uint64_t     sent, received;
    struct waitqueue receivers;          /* el duenyo, esperando mensaje    */
    struct waitqueue senders;            /* clientes, esperando hueco       */
};

void ipc_init(void);
int  port_create(uint64_t owner_pid, int64_t want);                       /* id o -1      */
int  port_send(int id, const struct message *m);            /* 0 o -1       */

/* Enviar SIN bloquear, y tirar el mensaje si no cabe. Lo usa el kernel
 * desde el manejador de interrupciones, donde esperar no es una opcion:
 * quedarse ahi parado seria colgar la maquina entera. */
int  port_notify(int id, uint64_t tipo);
uint64_t port_owner(int id);            /* pid del duenyo, 0 si no hay      */
int  port_recv(int id, struct message *out, uint64_t pid);  /* 0 o -1       */
void ipc_release_ports(uint64_t pid);   /* al morir un proceso              */
void ipc_dump(void);
