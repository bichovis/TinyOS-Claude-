/* user/mv.c - Mover o renombrar
 *
 * Las dos cosas son la misma operacion: escribir la entrada de directorio
 * en otro sitio y quitar la de antes. Los DATOS no se tocan, de ahi que
 * renombrar un fichero de un giga cueste lo mismo que uno vacio.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    if (argc < 3) { printf("\n  uso: mv ORIGEN DESTINO\n"); exit(1); }

    char destino[FS_PATH_MAX];
    ucopiar(destino, argv[2], sizeof(destino));

    /* "mv fichero directorio" quiere decir "metelo dentro", que es lo que
     * espera cualquiera. Se resuelve AQUI y no en el kernel porque es una
     * comodidad de la orden, no una operacion del sistema de ficheros:
     * abajo solo hay "renombra esto asi". */
    struct estado e;
    if (stat(destino, &e) == 0 && (e.flags & FS_ES_DIR)) {
        const char *base = argv[1];
        for (const char *p = argv[1]; *p; p++) if (*p == '/') base = p + 1;

        uint64_t d = strlen(destino), b = strlen(base);
        if (d + 1 + b + 1 > FS_PATH_MAX) {
            printf("\n  la ruta de destino no cabe\n");
            return 1;
        }
        if (d > 1) destino[d++] = '/';       /* el raiz ya trae la barra */
        memcpy(destino + d, base, b + 1);
    }

    if (rename(argv[1], destino) < 0) {
        printf("\n  no he podido mover %s: %s\n", argv[1], strerror(errno));
        if (errno == EEXIST)
            printf("  (el destino ya esta cogido; no lo machaco)\n");
        return 1;
    }

    printf("\n  %s -> %s\n", argv[1], destino);
    return 0;
}
