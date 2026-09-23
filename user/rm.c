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
#include <errno.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    if (argc < 2) { printf("\n  uso: rm FICHERO\n"); exit(1); }

    /* Una sola llamada, y el motivo viene con ella.
     *
     * Antes habia que preguntar DOS veces -un stat para ver si era un
     * directorio y luego el unlink- porque el fallo no decia por que. Eso
     * no era solo feo: entre las dos preguntas el fichero podia cambiar. */
    if (unlink(argv[1]) < 0) {
        if (errno == EISDIR)
            printf("\n  %s es un directorio: usa rmdir\n", argv[1]);
        else
            printf("\n  %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    printf("\n  borrado %s\n", argv[1]);
    return 0;
}
