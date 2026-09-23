/* user/cat.c - Volcar un fichero
 *
 * Eran sesenta lineas de hablar con el servidor por su puerto. Ahora son
 * open, read y close, como en cualquier sitio: el kernel manda los mismos
 * mensajes, pero este programa ya no tiene por que saberlo.
 *
 * Y de paso funciona con LO QUE SEA que haya detras del descriptor. Si le
 * dan una tuberia en vez de un fichero, "cat" no se entera.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("\n  uso: cat FICHERO...\n");
        exit(1);
    }

    for (int i = 1; i < argc; i++) {
        int64_t fd = openf(argv[i], O_LEER);
        if (fd < 0) {
            printf("\n  %s: no esta en la tarjeta\n", argv[i]);
            return 1;
        }

        printf("\n  --- %s ---\n", argv[i]);

        char buf[256];
        int64_t n;
        while ((n = read((int)fd, buf, sizeof(buf))) > 0)
            for (int64_t o = 0; o < n; ) {
                int64_t k = write(1, buf + o, (uint64_t)(n - o));
                if (k <= 0) break;
                o += k;
            }

        closefd((int)fd);
        printf("  --- fin ---\n");
    }
    return 0;
}
