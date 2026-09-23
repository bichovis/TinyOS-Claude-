/* user/rmdir.c - Borrar un directorio vacio
 *
 * Solo vacio, y a proposito. Borrar en cascada es facil de escribir y
 * dificil de deshacer: una orden mal escrita se lleva por delante todo lo
 * que cuelgue. Que haya que vaciarlo antes es lo que da la oportunidad de
 * darse cuenta.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    if (argc < 2) { printf("\n  uso: rmdir DIRECTORIO\n"); exit(1); }

    if (rmdir(argv[1]) < 0) {
        printf("\n  no he podido borrar %s\n", argv[1]);
        printf("  (o no existe, o no es un directorio, o no esta vacio:\n");
        printf("   no hay borrado en cascada)\n");
        return 1;
    }

    printf("\n  borrado %s\n", argv[1]);
    return 0;
}
