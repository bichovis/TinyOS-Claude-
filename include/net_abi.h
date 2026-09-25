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
    /* CAMPOS NUEVOS, AL FINAL, SIEMPRE (la leccion del paso 35). Cuentas de
     * la pila: sin ellas, "la red va lenta" no se puede convertir en una
     * causa. */
    unsigned long tramas_rx, tramas_tx;
    unsigned long tcp_seg, tcp_fuera, tcp_repes, tcp_retx, tcp_ack;
};

/* --- Ping: mandar un eco y esperar que vuelva -------------------------------
 *
 * ICMP no es UDP: no tiene puertos, asi que no cabe en un enchufe. Se pide
 * a la pila, que manda el echo request, espera el reply y contesta con lo
 * que traia: el viaje en milisegundos y el TTL con el que llego, que dice
 * cuantos routers ha cruzado.
 *
 * 'seq' lo pone quien pregunta y la pila lo devuelve tal cual: es como se
 * sabe que la respuesta es de ESTE ping y no del anterior, que llego tarde. */
#define UMSG_PING      46        /* struct umsg_ping -> UMSG_PING_OK | UMSG_ERROR */
#define UMSG_PING_OK  146        /* struct umsg_ping con ms y ttl                 */

struct umsg_ping {
    unsigned long port;              /* a donde contestar                  */
    unsigned long ip;                /* a quien                            */
    unsigned long seq;               /* el numero de este, para casarlo    */
    unsigned long datos;             /* bytes de relleno (0 = 56, los de siempre) */
    unsigned long ms;                /* en la respuesta: el viaje          */
    unsigned long ttl;               /* en la respuesta: el TTL que traia  */
};

/* --- TCP: una tuberia fiable sobre un cable que no lo es --------------------
 *
 * UDP manda un paquete y se olvida: si se pierde, se perdio. TCP promete
 * cuatro cosas que el cable no da, y las cuatro salen del mismo truco -numerar
 * los bytes y no dar uno por entregado hasta que el otro lo confirme-:
 *
 *   llega todo        lo que no se reconoce, se vuelve a mandar
 *   llega en orden    cada byte tiene su numero
 *   llega una vez     un numero repetido se descarta
 *   sin ahogar        el que recibe dice cuanto le cabe (la "ventana")
 *
 * El contrato de aqui es el de siempre en este sistema -pedir por mensaje y
 * que conteste al puerto que digas- con una diferencia que no es capricho:
 * los datos que llegan NO se empujan al programa, se quedan en la pila y el
 * programa los pide. Empujarlos obligaria a la pila a esperar si el programa
 * no lee, y una pila esperando es la red entera parada. Guardandolos, lo que
 * se llena es la ventana, el otro extremo se frena solo, y eso es justamente
 * lo que TCP inventó para esto.
 *
 * Una conexion se identifica con un numero pequenyo que devuelve el ABRIR.
 * Solo el proceso que la abrio puede usarla: la pila comprueba el pid que el
 * kernel pone en cada mensaje. */
#define UMSG_TCP_ABRIR    47     /* struct umsg_tcp -> UMSG_TCP_ABIERTA | UMSG_ERROR */
#define UMSG_TCP_ENVIAR   48     /* struct umsg_tcp con datos -> UMSG_TCP_HUECO      */
#define UMSG_TCP_LEER     49     /* struct umsg_tcp -> UMSG_TCP_DATOS (n=0: se acabo) */
#define UMSG_TCP_CERRAR   50     /* struct umsg_tcp -> nada                          */

#define UMSG_TCP_ABIERTA 147     /* struct umsg_tcp con la conexion                  */
#define UMSG_TCP_DATOS   148     /* struct umsg_tcp con lo leido                     */
#define UMSG_TCP_HUECO   149     /* lo anterior ya esta reconocido: manda mas        */

#define TCP_DATOS_MAX   1400     /* por mensaje; la pila parte si hace falta         */

struct umsg_tcp {
    unsigned long port;              /* a donde contestar                  */
    unsigned long conexion;          /* la que devolvio ABRIR              */
    unsigned long ip;                /* ABRIR: a quien                     */
    unsigned long puerto;            /* ABRIR: a que puerto                */
    unsigned long n;                 /* bytes utiles de datos[]            */
    unsigned char datos[TCP_DATOS_MAX];
};
