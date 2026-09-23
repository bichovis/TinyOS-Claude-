/* user/rm.c - Borrar un fichero
 *
 * Esto eran cincuenta lineas: crear un puerto, componer un fs_request,
 * mandarlo al servidor y esperar la respuesta. Ahora es una llamada.
 *
 * La IPC sigue ahi debajo, intacta: el kernel manda el mismo mensaje al
 * mismo servidor. Lo que ha cambiado es que ya no hay que conocerla para
 * borrar un fichero, y eso es lo que permite portar codigo escrito para
 * otro sistema.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    if (argc < 2) { printf("\n  uso: rm FICHERO\n"); exit(1); }

    /* Distinguir "no esta" de "es un directorio" no lo hace el kernel:
     * lo hace quien pregunta, porque es una diferencia de mensaje, no de
     * mecanismo. */
    struct estado e;
    if (stat(argv[1], &e) == 0 && (e.flags & FS_ES_DIR)) {
        printf("\n  %s es un directorio: usa rmdir\n", argv[1]);
        return 1;
    }

    if (unlink(argv[1]) < 0) {
        printf("\n  %s: no esta en la tarjeta\n", argv[1]);
        return 1;
    }

    printf("\n  borrado %s\n", argv[1]);
    return 0;
}
