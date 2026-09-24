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
#define FS_WRITE       4     /* name + arg=offset | FS_AL_FINAL + data     */
#define FS_CREATE      5     /* name -> lo crea, o lo vacia si ya estaba   */
#define FS_DELETE      6     /* name -> lo borra                           */
#define FS_MKDIR       7     /* name -> crea un directorio                 */
#define FS_RMDIR       8     /* name -> lo borra, si esta vacio            */
#define FS_RENAME      9     /* name + data=destino -> lo mueve            */

/* Un driver avisa de que hay un disco nuevo -un pendrive- y de por que
 * puerto se le piden sectores (ver blk_abi.h). El servidor lo monta en
 * /mnt y contesta FS_OK o FS_ERROR al puerto que venga en la peticion. */
#define FS_DISCO      10     /* data = struct fs_disco -> FS_OK | FS_ERROR */

struct fs_disco {
    unsigned long port;              /* a donde contestar                  */
    unsigned long bloques;           /* el puerto que sirve los sectores   */
    unsigned long sectores;
    char          nombre[32];        /* "TOSHIBA TransMemory"              */
};

/* --- Respuestas (message.type) ---------------------------------------- */
#define FS_OK        100
#define FS_ERROR     101     /* no existe, o la tarjeta fallo              */
#define FS_EOF       102     /* no queda nada que leer ahi                 */
#define FS_ES_DIRECTORIO 103 /* pedias un fichero y es un directorio       */
#define FS_NO_VACIO  104     /* rmdir sobre un directorio con cosas dentro */
#define FS_EXISTE    105     /* el destino de un rename ya esta cogido     */

/* --- Un desplazamiento que no es un desplazamiento --------------------
 *
 * FS_AL_FINAL en 'arg' de un FS_WRITE no quiere decir "escribe en el byte
 * 4294967295": quiere decir "escribe donde acabe el fichero AHORA, y dime
 * donde fue".
 *
 * Parece un atajo para ahorrarse un FS_SIZE y no lo es. Si el cliente
 * pregunta el tamanyo y luego escribe ahi, entre las dos peticiones cabe
 * otro cliente, y los dos escriben en el mismo sitio: el segundo tapa al
 * primero y nadie se entera. Preguntar y actuar son dos cosas, y entre
 * dos cosas siempre cabe una tercera.
 *
 * La unica forma de que no quepa es que sean UNA. Y eso obliga a que la
 * decision la tome quien es duenyo del dato, no quien lo consulta: el
 * final del fichero esta en la entrada de directorio, que es del
 * servidor. Aqui sale gratis, porque el servidor atiende un mensaje
 * entero antes de mirar el siguiente: dentro de una peticion no hay
 * nadie mas. La indivisibilidad no se ha construido, se ha colocado
 * donde ya estaba.
 *
 * Esto es O_APPEND, y es por lo que existe. Es tambien por lo que dos
 * procesos pueden escribir en el mismo log sin ponerse de acuerdo. */
#define FS_AL_FINAL   0xFFFFFFFFul

/* Lo que FS_WRITE devuelve en data[]: donde cayo de verdad.
 *
 * Hace falta porque con FS_AL_FINAL el cliente no lo sabe -no lo sabia
 * nadie hasta que se escribio- y sin ello no podria decir por donde va su
 * descriptor. Una peticion que decide algo tiene que contar que decidio. */
struct fs_escrito {
    unsigned long off;               /* primer byte que se escribio        */
};

/* Cuatro errores donde antes habia uno. No es burocracia: "no existe",
 * "es un directorio", "no esta vacio" y "ya existe" mandan a sitios
 * distintos, y juntarlos en FS_ERROR obliga a quien pregunta a adivinar. */

/* Lo que va en message.data de una peticion. Ocupa los 256 bytes justos.
 *
 * La ruta son 64 bytes: con 8.3 por componente eso da unos cinco niveles,
 * de sobra para un volumen FAT16. Lo que sobra es para los datos, que es
 * lo que escasea. */
#define FS_PATH_MAX   256
#define FS_NAME_MAX    64    /* una componente suelta, con nombre largo    */
#define FS_CHUNK      240    /* bytes utiles por mensaje de lectura/escritura */

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
    unsigned long mtime;             /* segundos desde 1970, 0 si no hay   */
};

/* CAMPOS NUEVOS, AL FINAL. SIEMPRE.
 *
 * 'mtime' se metio primero entre flags y name, que es donde quedaba bonito.
 * El resultado fue que un "ls" viejo -uno que quedo en la tarjeta de un
 * paso anterior- leyo el nombre en el sitio donde ahora estaba la fecha y
 * ensenyo esto:
 *
 *     ÍÚ³j             9080 bytes
 *
 * Esos cuatro bytes son 0x6AB3DACD al reves: el timestamp, leido como
 * texto. Y el sintoma no se parecia en nada a la causa, hasta el punto de
 * que parecia un fallo de FAT16.
 *
 * Poniendolo al final, un programa viejo sigue leyendo bien todo lo que
 * ya conocia y simplemente no ve el campo nuevo. No es compatibilidad de
 * verdad -para eso hace falta una version en el protocolo- pero convierte
 * "basura silenciosa" en "una cosa de menos", que es toda la diferencia
 * cuando hay binarios viejos rodando. */

/* Ojo a la tension entre los dos tamanyos: una componente puede tener 63
 * caracteres pero la RUTA entera sigue midiendo 64. O sea que un nombre
 * largo cabe en el raiz y no cabe tres niveles abajo. Subir FS_PATH_MAX
 * obligaria a bajar FS_CHUNK, porque los dos salen del mismo mensaje. */
