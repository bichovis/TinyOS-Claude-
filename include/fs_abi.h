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
#define FS_READ        2     /* name + arg=offset -> hasta FS_CHUNK bytes  */
#define FS_LIST        3     /* arg=indice -> name y tamanyo de esa entrada*/
#define FS_WRITE       4     /* name + arg=offset + data -> escribe        */
#define FS_CREATE      5     /* name -> lo crea, o lo vacia si ya estaba   */
#define FS_DELETE      6     /* name -> lo borra                           */

/* --- Respuestas (message.type) ---------------------------------------- */
#define FS_OK        100
#define FS_ERROR     101     /* no existe, o la tarjeta fallo              */
#define FS_EOF       102     /* no queda nada que leer ahi                 */

/* Lo que va en message.data de una peticion. Ocupa los 128 bytes justos.
 *
 * El nombre baja a 16 bytes porque un 8.3 son doce caracteres y el cero:
 * lo que sobra se aprovecha para los datos, que es lo que escasea. */
#define FS_NAME_MAX    16
#define FS_CHUNK       96    /* bytes utiles por mensaje de lectura/escritura */

struct fs_request {
    unsigned long port;              /* a donde contestar                  */
    unsigned long arg;               /* desplazamiento, o indice en LIST   */
    char          name[FS_NAME_MAX]; /* "HOLA.TXT", en formato 8.3         */
    char          data[FS_CHUNK];    /* lo que se escribe                  */
};

/* En una respuesta a FS_SIZE o FS_LIST, esto es lo que va en data. */
struct fs_info {
    unsigned long size;
    char          name[FS_NAME_MAX];
};
