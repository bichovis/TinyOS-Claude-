/* user/wc.c - Contar lo que pasa por la entrada
 *
 * Como 'upper', no sabe de donde vienen los bytes. Y como escribe su
 * resultado en la salida, se le puede encadenar otra cosa detras.
 */
#include "syscall.h"

static char buf[128];

static void num(const char *antes, uint64_t v, const char *despues)
{
    char b[24];
    uint64_t n = udec(b, v);
    b[n] = 0;
    kprint(antes); kprint(b); kprint(despues);
}

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
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

    num("\n  ", lineas, " lineas, ");
    num("", bytes, " bytes\n");
    exit(0);
}
