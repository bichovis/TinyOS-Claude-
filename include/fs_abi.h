/* fs_abi.h - Contrato con el servidor de ficheros
 *
 * El servidor vive en EL0, como el de consola. Se le habla por el puerto
 * PORT_FILES y contesta al puerto que el cliente le diga.
 *
 * El protocolo NO tiene open/close, y es a proposito: cada peticion lleva
 * el nombre del fichero y el desplazamiento. Un servidor sin estado no
 * tiene descriptores que perder cuando un cliente muere sin avisar, ni
 * tabla que limpiar, ni limite de ficheros abiertos. Se paga con una
 * busqueda en el directorio por peticion, que el servidor se cachea.
 */
#pragma once
#include "ipc_abi.h"

#define PORT_FILES     1

/* --- Peticiones (message.type) ---------------------------------------- */
#define FS_SIZE        1     /* name -> tamanyo en bytes                   */
#define FS_READ        2     /* name + arg=offset -> hasta 48 bytes        */
#define FS_LIST        3     /* arg=indice -> name y tamanyo de esa entrada*/

/* --- Respuestas (message.type) ---------------------------------------- */
#define FS_OK        100
#define FS_ERROR     101     /* no existe, o la tarjeta fallo              */
#define FS_EOF       102     /* no queda nada que leer ahi                 */

/* Lo que va en message.data de una peticion. Ocupa los 48 bytes justos. */
struct fs_request {
    unsigned long port;      /* a donde contestar                          */
    unsigned long arg;       /* desplazamiento (READ) o indice (LIST)      */
    char          name[32];  /* "HOLA.TXT", en mayusculas y formato 8.3    */
};

/* En una respuesta a FS_SIZE o FS_LIST, esto es lo que va en data. */
struct fs_info {
    unsigned long size;
    char          name[32];
};
