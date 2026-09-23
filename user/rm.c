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
    for (int i = 0; i < FS_NAME_MAX; i++) r.name[i] = 0;
    memcpy(r.name, argv[1], strlen(argv[1]) + 1);

    m.type = FS_DELETE;
    m.len  = 0;
    memcpy(m.data, (const char *)&r, sizeof(r));

    if (msg_send(PORT_FILES, &m) < 0 || msg_recv((uint64_t)mio, &m) < 0) {
        printf("\n  [rm] no hay servidor de ficheros\n");
        exit(1);
    }

    if (m.type != FS_OK) {
        printf("\n  ");
        printf("%s", argv[1]);
        printf(": no esta en la tarjeta\n");
        exit(1);
    }

    printf("\n  borrado ");
    printf("%s", argv[1]);
    printf("\n");
    exit(0);
}
