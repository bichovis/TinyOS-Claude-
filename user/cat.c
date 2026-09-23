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
#include <errno.h>
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
            printf("\n  %s: %s\n", argv[i], strerror(errno));
            return 1;
        }

        printf("\n  --- %s ---\n", argv[i]);

        /* El cuerpo sale por stdout, el MISMO sitio por el que salieron
         * las dos cabeceras.
         *
         * Antes esto era un write(1, ...) a pelo, y funcionaba de milagro:
         * mientras stdout hablaba con la consola se vaciaba en cada salto
         * de linea y el orden cuadraba. Con `cat fichero > otro` no hay
         * saltos que valgan, las cabeceras se quedaban en el cubo hasta el
         * final y aparecian DETRAS del contenido que anunciaban.
         *
         * Mezclar la libc y el descriptor a pelo en la misma salida es
         * eso: dos colas distintas para la misma puerta. */
        char buf[256];
        int64_t n;
        while ((n = read((int)fd, buf, sizeof(buf))) > 0)
            fwrite(buf, 1, (size_t)n, stdout);

        closefd((int)fd);
        printf("  --- fin ---\n");
    }
    return 0;
}
