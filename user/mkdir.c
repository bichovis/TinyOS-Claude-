/* user/mkdir.c - Crear un directorio
 *
 * Todo el trabajo esta en el servidor: pedir un cluster, ponerlo a ceros y
 * escribir dentro "." y "..". Aqui solo se manda la peticion, que es como
 * tiene que ser.
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
        printf("\n  uso: mkdir NOMBRE\n");
        exit(1);
    }

    int64_t mio = port_create(-1);
    if (mio < 0) { printf("  [mkdir] sin puertos\n"); exit(1); }

    struct fs_request r;
    r.port = (unsigned long)mio;
    r.arg  = 0;
    for (int i = 0; i < FS_PATH_MAX; i++) r.name[i] = 0;
    if (realpath(argv[1], r.name) < 0) {
        printf("  [mkdir] ruta imposible\n");
        exit(1);
    }

    m.type = FS_MKDIR;
    m.len  = 0;
    memcpy(m.data, (const char *)&r, sizeof(r));

    if (msg_send(PORT_FILES, &m) < 0 || msg_recv((uint64_t)mio, &m) < 0) {
        printf("  [mkdir] no hay servidor de ficheros: arrancalo con 'f'\n");
        exit(1);
    }

    if (m.type != FS_OK) {
        printf("  no he podido crear %s\n", r.name);
        printf("  puede ser: que ya exista, que el directorio de encima no\n");
        printf("  este, o que el nombre no quepa en 8.3 (hasta 8 letras,\n");
        printf("  un punto y 3 mas, y sin espacios)\n");
        exit(1);
    }

    printf("  creado %s\n", r.name);
    return 0;
}
