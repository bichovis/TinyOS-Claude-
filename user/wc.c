/* user/wc.c - Contar lo que pasa por la entrada
 *
 * Como 'upper', no sabe de donde vienen los bytes. Y como escribe su
 * resultado en la salida, se le puede encadenar otra cosa detras.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

static char buf[128];

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    uint64_t bytes = 0, lineas = 0;

    for (;;) {
        int64_t n = read(0, buf, sizeof(buf));
        if (n <= 0) break;

        bytes += (uint64_t)n;
        for (int64_t i = 0; i < n; i++)
            if (buf[i] == '\n') lineas++;
    }

    printf("\n  %lu lineas, ", lineas);
    printf("%lu bytes\n", bytes);
    exit(0);
}
