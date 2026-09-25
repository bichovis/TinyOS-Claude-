/* net_abi.h - Contrato entre el driver de la tarjeta de red y la pila
 *
 * La tarjeta esta en el driver de USB, que es quien tiene el controlador. La
 * pila -ARP, IP, UDP, DHCP...- es OTRO proceso: el servidor de red, que vive
 * en el puerto PORT_RED como el de ficheros vive en el 1. Entre los dos
 * viajan tramas Ethernet enteras, una por mensaje, y nada mas: el driver no
 * sabe que es una direccion IP, y la pila no sabe que hay un cable USB.
 *
 * Es el mismo reparto que con el disco (blk_abi.h): el driver mueve bytes,
 * el servidor les da sentido. Y por el mismo motivo: si la pila se cuelga,
 * se cuelga ella; el teclado y el pendrive, que viven en el driver, siguen.
 */
#pragma once
#include "ipc_abi.h"

#define PORT_RED       3         /* la pila de red, siempre aqui              */

#define TRAMA_MAX   1514         /* una trama Ethernet sin CRC                */

/* --- Del driver a la pila (a PORT_RED) --------------------------------- */
#define NMSG_TARJETA  30         /* struct net_tarjeta: "hay tarjeta"         */
#define NMSG_TRAMA    31         /* data = la trama entera, len = bytes       */

/* --- De la pila al driver (al puerto que dijo en NMSG_TARJETA) ---------- */
#define NMSG_ENVIAR   32         /* data = la trama entera, len = bytes       */

struct net_tarjeta {
    unsigned long port;              /* donde se le mandan las tramas      */
    unsigned char mac[6];            /* la direccion de la tarjeta         */
    char          nombre[16];        /* "LAN9514", "CDC-ECM"               */
};

/* --- Clientes de la pila (a PORT_RED; contesta al puerto que digan) ----
 *
 * Lo primero que se le puede pedir a la red es la hora: la pila la pide a
 * un servidor NTP, la pone en el reloj del kernel y la devuelve. La pone
 * ella y no el cliente porque el reloj del sistema no es de cualquiera
 * (SYS_settime): es de init, que arranca, y de la pila, que es por donde
 * llega la hora de verdad. */
#define UMSG_HORA      40        /* struct umsg_pedir -> UMSG_HORA_OK | UMSG_ERROR */
#define UMSG_HORA_OK  140        /* struct umsg_hora                           */
#define UMSG_ERROR    141        /* data = el motivo, en texto                 */

struct umsg_pedir {
    unsigned long port;              /* a donde contestar                  */
};

struct umsg_hora {
    unsigned long segundos;          /* hora LOCAL, segundos desde 1970    */
    unsigned long desfase;           /* segundos de la zona sobre UTC (+/-) */
    unsigned long servidor;          /* la IP del servidor NTP que contesto */
};

/* --- Enchufes UDP: la red para cualquier programa ---------------------------
 *
 * Un "enchufe" (socket) es un puerto UDP local que un programa se queda.
 * Lo que llegue a ese puerto se le entrega como mensaje a SU puerto de IPC;
 * lo que quiera mandar lo manda por la pila diciendo desde cual. La pila
 * lleva la tabla (quien tiene que puerto UDP), y la identidad es el pid que
 * el kernel pone en cada mensaje: nadie puede mandar por el enchufe de
 * otro. Si el programa muere, su puerto de IPC desaparece, la entrega
 * falla, y la pila cierra el enchufe sola.
 *
 * Es un socket de Unix sin el descriptor: abrir, enviar, recibir, cerrar;
 * y resolver nombres, que sin eso solo se puede hablar con numeros. */
#define UMSG_ABRIR       41      /* struct umsg_abrir -> UMSG_ABIERTO | UMSG_ERROR */
#define UMSG_CERRAR      42      /* struct umsg_abrir (local) -> nada             */
#define UMSG_ENVIAR      43      /* struct umsg_dgrama -> nada (UMSG_ERROR si no) */
#define UMSG_RESOLVER    44      /* struct umsg_resolver -> UMSG_RESUELTO | UMSG_ERROR */
#define UMSG_INFO        45      /* struct umsg_pedir -> UMSG_INFO_OK               */

#define UMSG_ABIERTO    142      /* struct umsg_abrir con el puerto local asignado */
#define UMSG_DATAGRAMA  143      /* struct umsg_dgrama: ha llegado esto            */
#define UMSG_RESUELTO   144      /* struct umsg_resolver con ip                     */
#define UMSG_INFO_OK    145      /* struct umsg_info                                */

#define UDP_DATOS_MAX  1472      /* lo que cabe en una trama detras de IP y UDP    */

struct umsg_abrir {
    unsigned long port;              /* a donde contestar y entregar       */
    unsigned long local;             /* puerto UDP pedido; 0 = el que sea  */
};

struct umsg_dgrama {
    unsigned long local;             /* por que enchufe                    */
    unsigned long ip;                /* el otro extremo                    */
    unsigned long puerto;
    unsigned long n;                 /* bytes utiles de datos[]            */
    unsigned char datos[UDP_DATOS_MAX];
};

struct umsg_resolver {
    unsigned long port;
    unsigned long ip;                /* la respuesta                       */
    char          nombre[64];
};

struct umsg_info {
    unsigned long ip, mascara, router, dns, ntp;
    unsigned long estado;            /* 0 sin tarjeta, 1 buscando, 2 pidiendo, 3 con direccion */
    unsigned char mac[6];
    char          tarjeta[16];
};
