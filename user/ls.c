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
#include "syscall.h"
#include "fs_abi.h"

static struct message m;

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
    int64_t mio = port_create(-1);
    if (mio < 0) { printf("  [ls] sin puertos\n"); exit(1); }

    char ruta[FS_PATH_MAX];
    if (realpath(argc > 1 ? argv[1] : ".", ruta) < 0) {
        printf("  [ls] ruta imposible\n");
        exit(1);
    }

    printf("\n  %s\n", ruta);

    for (uint64_t i = 0; ; i++) {
        struct fs_request r;
        r.port = (unsigned long)mio;
        r.arg  = i;
        for (int j = 0; j < FS_PATH_MAX; j++) r.name[j] = 0;
        memcpy(r.name, ruta, strlen(ruta) + 1);

        m.type = FS_LIST;
        m.len  = sizeof(r);
        memcpy(m.data, (const char *)&r, sizeof(r));
        if (msg_send(PORT_FILES, &m) < 0) {
            printf("  [ls] no hay servidor de ficheros: arrancalo con 'f'\n");
            exit(1);
        }

        if (msg_recv((uint64_t)mio, &m) < 0) break;
        if (m.type == FS_ERROR) {
            printf("  [ls] no es un directorio\n");
            exit(1);
        }
        if (m.type != FS_OK) break;

        struct fs_info *info = (struct fs_info *)m.data;

        /* El "%-14s" es la columna entera: alinear a la izquierda
         * rellenando con espacios hasta 14. Antes eso era un bucle. */
        char cuando[20];
        fecha_corta(cuando, info->mtime);

        if (info->flags & FS_ES_DIR)
            printf("    %s  %8s  %s\n", cuando, "<dir>", info->name);
        else
            printf("    %s  %8lu  %s\n", cuando, info->size, info->name);
    }

    exit(0);
}
