/* user/cp.c - Copiar un fichero
 *
 * Leer de uno y escribir en otro, que es literalmente lo que dice el
 * nombre. Antes habia que componer peticiones para las dos mitades; ahora
 * son dos descriptores y un bucle.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("\n  uso: cp ORIGEN DESTINO\n");
        exit(1);
    }

    int64_t o = openf(argv[1], O_LEER);
    if (o < 0) { printf("\n  %s: no esta en la tarjeta\n", argv[1]); return 1; }

    /* O_ESCRIBIR crea el fichero, y si ya estaba lo vacia. Eso es lo que
     * significa copiar encima de algo. */
    int64_t d = openf(argv[2], O_ESCRIBIR);
    if (d < 0) {
        printf("\n  no puedo escribir %s\n", argv[2]);
        closefd((int)o);
        return 1;
    }

    char buf[256];
    uint64_t total = 0;
    int64_t n;

    while ((n = read((int)o, buf, sizeof(buf))) > 0) {
        for (int64_t p = 0; p < n; ) {
            int64_t k = write((int)d, buf + p, (uint64_t)(n - p));
            if (k <= 0) { printf("\n  se acabo el sitio\n"); n = -1; break; }
            p += k;
        }
        if (n < 0) break;
        total += (uint64_t)n;
    }

    closefd((int)o);
    closefd((int)d);

    if (n < 0) return 1;

    printf("\n  %s -> %s, %lu bytes\n", argv[1], argv[2], total);
    return 0;
}
