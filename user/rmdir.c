/* user/rmdir.c - Borrar un directorio vacio
 *
 * Solo vacio, y a proposito. Borrar en cascada es facil de escribir y
 * dificil de deshacer: una orden mal escrita se lleva por delante todo lo
 * que cuelgue. Que haya que vaciarlo antes es lo que da la oportunidad de
 * darse cuenta.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include "fs_abi.h"

static struct message m;

int main(int argc, char **argv)
{
    if (argc < 2) { printf("\n  uso: rmdir DIRECTORIO\n"); exit(1); }

    int64_t mio = port_create(-1);
    if (mio < 0) { printf("  [rmdir] sin puertos\n"); exit(1); }

    struct fs_request r;
    r.port = (unsigned long)mio;
    r.arg  = 0;
    for (int i = 0; i < FS_PATH_MAX; i++) r.name[i] = 0;
    if (realpath(argv[1], r.name) < 0) { printf("  [rmdir] ruta imposible\n"); exit(1); }

    m.type = FS_RMDIR;
    m.len  = 0;
    memcpy(m.data, (const char *)&r, sizeof(r));

    if (msg_send(PORT_FILES, &m) < 0 || msg_recv((uint64_t)mio, &m) < 0) {
        printf("  [rmdir] no hay servidor de ficheros: arrancalo con 'f'\n");
        exit(1);
    }

    /* Cada error manda a un sitio distinto, asi que se dicen distintos. */
    switch (m.type) {
    case FS_OK:
        printf("  borrado %s\n", r.name);
        return 0;
    case FS_NO_VACIO:
        printf("  %s no esta vacio\n", r.name);
        printf("  (borra antes lo que tenga dentro; no hay borrado en cascada)\n");
        return 1;
    default:
        printf("  %s no existe, o no es un directorio\n", r.name);
        return 1;
    }
}
