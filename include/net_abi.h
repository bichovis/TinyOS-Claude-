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
