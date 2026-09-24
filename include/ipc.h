/* ipc.h - Puertos de mensajes (lado kernel) */
#pragma once
#include <stdint.h>
#include "ipc_abi.h"
#include "sync.h"

#define MAX_PORTS     16
#define PORT_QUEUE     8

/* El puerto por el que el KERNEL recibe respuestas del servidor de
 * ficheros. Su duenyo es el pid 0, que no existe -los procesos empiezan en
 * CORES-, asi que ningun proceso puede vaciarlo.
 *
 * Esta reservado: port_create(-1) lo salta. Sin eso se lo lleva el primer
 * proceso que pida un puerto sin decir cual, que resulta ser el shell, y
 * la redireccion falla con un "no puedo escribir" que no dice por que. */
#define PORT_KERNEL    2

/* Los tres de abajo estan RESERVADOS: el 0 para la consola, el 1 para los
 * ficheros y el 2 para el kernel. port_create(-1) empieza a repartir a
 * partir del 3.
 *
 * Que un puerto "de los conocidos" se pueda repartir por sorteo es un
 * fallo que ya paso una vez -el kernel se quedaba sin el suyo- y volvio a
 * pasar en cuanto init pidio un puerto antes que el servidor de ficheros:
 * se llevo el 1, y el servidor se encontro su sitio ocupado y se murio.
 * Un numero reservado que no esta reservado no es un numero reservado. */
#define PORT_PRIMERO_LIBRE  4   /* el 3 es la pila de red (PORT_RED, net_abi.h) */

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

/* Alarmas: CMSG_ALARMA a un puerto cada tantos ticks. Las pone un driver y
 * las dispara el tick, sin cerrojo del planificador cogido. */
int  alarma_poner(uint64_t pid, int puerto, uint64_t cada);   /* 0 o -1 */
void alarmas_tick(uint64_t ahora);
