/* user/ls.c - Listar el directorio raiz de la tarjeta
 *
 * No toca la SD ni sabe lo que es FAT: solo habla por el puerto de
 * ficheros. Esa es toda la gracia de tener un servidor.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include "fs_abi.h"

static struct message m;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int64_t mio = port_create(-1);
    if (mio < 0) { printf("  [ls] sin puertos\n"); exit(1); }

    printf("\n  Contenido de la tarjeta:\n");

    for (uint64_t i = 0; ; i++) {
        struct fs_request r;
        r.port = (unsigned long)mio;
        r.arg  = i;
        for (int j = 0; j < FS_NAME_MAX; j++) r.name[j] = 0;

        m.type = FS_LIST;
        m.len  = sizeof(r);
        memcpy(m.data, (const char *)&r, sizeof(r));
        if (msg_send(PORT_FILES, &m) < 0) {
            printf("  [ls] no hay servidor de ficheros: arrancalo con 'f'\n");
            exit(1);
        }

        if (msg_recv((uint64_t)mio, &m) < 0) break;
        if (m.type != FS_OK) break;

        struct fs_info *info = (struct fs_info *)m.data;

        /* El "%-14s" es la columna entera: alinear a la izquierda
         * rellenando con espacios hasta 14. Antes eso era un bucle. */
        printf("    %-14s %6lu bytes\n", info->name, info->size);
    }

    exit(0);
}
