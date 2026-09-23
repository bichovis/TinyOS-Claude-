/* user/write.c - Escribir una linea en un fichero
 *
 * El programa mas tonto que hay, y hace falta: sin el no habria forma de
 * crear un fichero desde dentro del sistema, y todo lo que se probara
 * vendria de fuera.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("\n  uso: write FICHERO texto...\n");
        exit(1);
    }

    int64_t fd = openf(argv[1], O_ESCRIBIR);
    if (fd < 0) { printf("\n  no puedo escribir %s\n", argv[1]); return 1; }

    for (int i = 2; i < argc; i++) {
        if (i > 2) write((int)fd, " ", 1);

        const char *s = argv[i];
        uint64_t n = strlen(s);
        for (uint64_t o = 0; o < n; ) {
            int64_t k = write((int)fd, s + o, n - o);
            if (k <= 0) break;
            o += (uint64_t)k;
        }
    }
    write((int)fd, "\n", 1);

    closefd((int)fd);
    printf("\n  escrito en %s\n", argv[1]);
    return 0;
}
