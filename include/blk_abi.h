/* blk_abi.h - Contrato entre un driver de disco y quien lo use
 *
 * Un disco es lo mas simple que hay: sectores numerados de 512 bytes que se
 * leen y se escriben. Y por eso el contrato tambien lo es: un mensaje por
 * sector, con el numero del sector y a donde contestar. El driver de USB lo
 * ofrece por su puerto; el servidor de ficheros lo usa igual que usa la SD,
 * sin saber que hay un cable por medio.
 *
 * Es UN sector por mensaje. No es lo mas rapido -un fichero de 100 KB son
 * 200 viajes- pero es lo que cabe en el mensaje, y el servidor de ficheros
 * ya lee de uno en uno por su cache de un sector.
 */
#pragma once
#include "ipc_abi.h"

/* --- Peticiones (message.type), al puerto del driver ------------------ */
#define BMSG_LEER      20    /* struct blk_request -> BMSG_OK con los datos  */
#define BMSG_ESCRIBIR  21    /* struct blk_request con datos -> BMSG_OK      */

/* --- Respuestas, al puerto que diga la peticion ----------------------- */
#define BMSG_OK       120
#define BMSG_ERROR    121    /* el disco no contesta, o ya no esta           */

struct blk_request {
    unsigned long port;              /* a donde contestar                  */
    unsigned long lba;               /* que sector                          */
    unsigned char datos[512];        /* lo que se escribe; en LEER, nada   */
};                                   /* 528 bytes: cabe en MSG_DATA_MAX     */
