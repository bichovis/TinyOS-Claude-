/* user/mkdir.c - Crear un directorio */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    if (argc < 2) { printf("\n  uso: mkdir NOMBRE\n"); exit(1); }

    if (mkdir(argv[1]) < 0) {
        printf("\n  no he podido crear %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    printf("\n  creado %s\n", argv[1]);
    return 0;
}
