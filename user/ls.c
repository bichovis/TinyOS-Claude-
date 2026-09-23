/* user/ls.c - Listar un directorio de la tarjeta
 *
 * No toca la SD ni sabe lo que es FAT: solo habla por el puerto de
 * ficheros. Esa es toda la gracia de tener un servidor.
 *
 * Sin argumentos lista el directorio actual, y para eso pregunta cual es:
 * el servidor no lo sabe -no tiene estado- y el programa tampoco -lo
 * guarda el kernel-. realpath(".") es justo esa pregunta.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "syscall.h"
#include "fs_abi.h"

/* "2026-09-23 07:14", o guiones si el fichero no trae fecha.
 *
 * Se calcula aqui y no en el servidor porque es presentacion: el servidor
 * entrega segundos desde 1970, que es un numero, y como se escriba un
 * numero es asunto de quien lo ensenya. */
static void fecha_corta(char *dst, uint64_t t)
{
    if (!t) { memcpy(dst, "       (sin fecha)", 19); dst[18] = 0; return; }

    uint64_t dias = t / 86400, resto = t % 86400;

    uint64_t anyo = 1970;
    for (;;) {
        int bis = (anyo % 4 == 0 && anyo % 100 != 0) || anyo % 400 == 0;
        uint64_t largo = bis ? 366 : 365;
        if (dias < largo) break;
        dias -= largo; anyo++;
    }

    static const uint64_t meses[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int bis = (anyo % 4 == 0 && anyo % 100 != 0) || anyo % 400 == 0;
    uint64_t mes = 0;
    for (; mes < 12; mes++) {
        uint64_t largo = meses[mes] + ((mes == 1 && bis) ? 1u : 0u);
        if (dias < largo) break;
        dias -= largo;
    }

    snprintf(dst, 20, "%04lu-%02lu-%02lu %02lu:%02lu",
             anyo, mes + 1, dias + 1,
             resto / 3600, (resto % 3600) / 60);
}

int main(int argc, char **argv)
{
    char ruta[FS_PATH_MAX];
    if (realpath(argc > 1 ? argv[1] : ".", ruta) < 0) {
        printf("  [ls] ruta imposible\n");
        exit(1);
    }

    /* opendir y readdir, como en cualquier sitio. Aqui habia treinta
     * lineas de componer un fs_request y mandarlo por un puerto: eso
     * sigue pasando, pero lo hace el kernel y no este programa. */
    int64_t d = opendir(ruta);
    if (d < 0) {
        printf("  %s: %s\n", ruta, strerror(errno));
        exit(1);
    }

    printf("\n  %s\n", ruta);

    struct fs_info info;
    while (readdir((int)d, &info) == 1) {
        char cuando[20];
        fecha_corta(cuando, info.mtime);

        if (info.flags & FS_ES_DIR)
            printf("    %s  %8s  %s\n", cuando, "<dir>", info.name);
        else
            printf("    %s  %8lu  %s\n", cuando, info.size, info.name);
    }

    closefd((int)d);
    return 0;
}
