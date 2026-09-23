/* user/rm.c - Borrar un fichero de la tarjeta
 *
 * Borrar en FAT es poner un 0xE5 en la primera letra del nombre y devolver
 * sus clusters a la tabla. Los datos siguen ahi intactos, y por eso los
 * ficheros borrados se pueden recuperar mientras nadie escriba encima.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include "fs_abi.h"

static struct message m;

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("\n  uso: rm NOMBRE.EXT\n");
        exit(1);
    }

    int64_t mio = port_create(-1);
    if (mio < 0) { printf("  [rm] sin puertos\n"); exit(1); }

    struct fs_request r;
    r.port = (unsigned long)mio;
    r.arg  = 0;
    for (int i = 0; i < FS_PATH_MAX; i++) r.name[i] = 0;
    if (realpath(argv[1], r.name) < 0) { printf("  [rm] ruta imposible\n"); exit(1); }

    m.type = FS_DELETE;
    m.len  = 0;
    memcpy(m.data, (const char *)&r, sizeof(r));

    if (msg_send(PORT_FILES, &m) < 0 || msg_recv((uint64_t)mio, &m) < 0) {
        printf("\n  [rm] no hay servidor de ficheros\n");
        exit(1);
    }

    /* "no esta en la tarjeta" era verdad a medias cuando le dabas un
     * directorio: el fichero SI esta, lo que pasa es que no es un
     * fichero. Y mandaba a buscar donde no era. */
    if (m.type == FS_ES_DIRECTORIO) {
        printf("\n  %s es un directorio: usa rmdir\n", argv[1]);
        exit(1);
    }

    if (m.type != FS_OK) {
        printf("\n  %s: no esta en la tarjeta\n", argv[1]);
        exit(1);
    }

    printf("\n  borrado %s\n", argv[1]);
    return 0;
}
