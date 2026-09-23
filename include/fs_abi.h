/* fs_abi.h - Contrato con el servidor de ficheros
 *
 * El servidor vive en EL0, como el de consola. Se le habla por el puerto
 * PORT_FILES y contesta al puerto que el cliente le diga.
 *
 * El protocolo NO tiene open/close, y es a proposito: cada peticion lleva
 * la ruta del fichero y el desplazamiento. Un servidor sin estado no tiene
 * descriptores que perder cuando un cliente muere sin avisar, ni tabla que
 * limpiar, ni limite de ficheros abiertos. Se paga con una busqueda en el
 * directorio por peticion, que el servidor se cachea.
 *
 * DESDE EL PASO 31 LAS RUTAS SON ABSOLUTAS, SIEMPRE. El servidor no sabe
 * que es un "directorio actual" ni quiere saberlo: eso es estado de cada
 * proceso, y un servidor sin estado que preguntara por el dejaria de
 * serlo. Quien manda la peticion resuelve antes la ruta con realpath(),
 * que es una llamada al kernel, que es quien guarda el cwd.
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
#define FS_MKDIR       7     /* name -> crea un directorio                 */
#define FS_RMDIR       8     /* name -> lo borra, si esta vacio            */
#define FS_RENAME      9     /* name + data=destino -> lo mueve            */

/* --- Respuestas (message.type) ---------------------------------------- */
#define FS_OK        100
#define FS_ERROR     101     /* no existe, o la tarjeta fallo              */
#define FS_EOF       102     /* no queda nada que leer ahi                 */
#define FS_ES_DIRECTORIO 103 /* pedias un fichero y es un directorio       */
#define FS_NO_VACIO  104     /* rmdir sobre un directorio con cosas dentro */
#define FS_EXISTE    105     /* el destino de un rename ya esta cogido     */

/* Cuatro errores donde antes habia uno. No es burocracia: "no existe",
 * "es un directorio", "no esta vacio" y "ya existe" mandan a sitios
 * distintos, y juntarlos en FS_ERROR obliga a quien pregunta a adivinar. */

/* Lo que va en message.data de una peticion. Ocupa los 256 bytes justos.
 *
 * La ruta son 64 bytes: con 8.3 por componente eso da unos cinco niveles,
 * de sobra para un volumen FAT16. Lo que sobra es para los datos, que es
 * lo que escasea. */
#define FS_PATH_MAX    64
#define FS_NAME_MAX    64    /* una componente suelta, con nombre largo    */
#define FS_CHUNK      176    /* bytes utiles por mensaje de lectura/escritura */

struct fs_request {
    unsigned long port;              /* a donde contestar                  */
    unsigned long arg;               /* desplazamiento, o indice en LIST   */
    char          name[FS_PATH_MAX]; /* "/DOCS/HOLA.TXT", absoluta         */
    char          data[FS_CHUNK];    /* lo que se escribe                  */
};

/* En una respuesta a FS_SIZE o FS_LIST, esto es lo que va en data. */
#define FS_ES_DIR   1

struct fs_info {
    unsigned long size;
    unsigned long flags;             /* FS_ES_DIR si es un directorio      */
    char          name[FS_NAME_MAX]; /* solo la componente, no la ruta     */
};

/* Ojo a la tension entre los dos tamanyos: una componente puede tener 63
 * caracteres pero la RUTA entera sigue midiendo 64. O sea que un nombre
 * largo cabe en el raiz y no cabe tres niveles abajo. Subir FS_PATH_MAX
 * obligaria a bajar FS_CHUNK, porque los dos salen del mismo mensaje. */
