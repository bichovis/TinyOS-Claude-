/* user/mv.c - Mover o renombrar
 *
 * Las dos cosas son la misma operacion: escribir la entrada de directorio
 * en otro sitio y quitar la de antes. Los DATOS no se tocan.
 *
 * De ahi que renombrar un fichero de un giga cueste lo mismo que uno de
 * cero bytes. Y de ahi tambien que no se pueda mover entre particiones:
 * ahi ya no vale cambiar un nombre de sitio, hay que copiar los bytes, y
 * eso lo hacen "cp" y "rm".
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include "fs_abi.h"

static struct message m;

/* Preguntarle al servidor si una ruta es un directorio. */
static int es_directorio(int64_t mio, const char *ruta)
{
    struct fs_request r;
    r.port = (unsigned long)mio;
    r.arg  = 0;
    for (int i = 0; i < FS_PATH_MAX; i++) r.name[i] = 0;
    memcpy(r.name, ruta, strlen(ruta) + 1);

    m.type = FS_SIZE;
    m.len  = 0;
    memcpy(m.data, (const char *)&r, sizeof(r));

    if (msg_send(PORT_FILES, &m) < 0 || msg_recv((uint64_t)mio, &m) < 0) return 0;
    if (m.type != FS_OK) return 0;

    return (((struct fs_info *)m.data)->flags & FS_ES_DIR) ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("\n  uso: mv ORIGEN DESTINO\n"); exit(1); }

    int64_t mio = port_create(-1);
    if (mio < 0) { printf("  [mv] sin puertos\n"); exit(1); }

    struct fs_request r;
    r.port = (unsigned long)mio;
    r.arg  = 0;
    for (int i = 0; i < FS_PATH_MAX; i++) r.name[i] = 0;
    for (int i = 0; i < FS_CHUNK; i++)    r.data[i] = 0;

    if (realpath(argv[1], r.name) < 0 || realpath(argv[2], r.data) < 0) {
        printf("  [mv] ruta imposible\n");
        exit(1);
    }

    /* "mv fichero directorio" quiere decir "metelo dentro", que es lo que
     * espera cualquiera. Se resuelve AQUI y no en el servidor porque es
     * una comodidad de la orden, no una operacion del sistema de
     * ficheros: abajo solo hay "renombra esto asi". */
    if (es_directorio(mio, r.data)) {
        const char *base = r.name;
        for (const char *p = r.name; *p; p++) if (*p == '/') base = p + 1;

        uint64_t d = strlen(r.data), b = strlen(base);
        if (d + 1 + b + 1 > FS_PATH_MAX) {
            printf("  [mv] la ruta de destino no cabe\n");
            exit(1);
        }
        if (d > 1) r.data[d++] = '/';          /* el raiz ya trae la barra */
        memcpy(r.data + d, base, b + 1);
    }

    m.type = FS_RENAME;
    m.len  = 0;
    memcpy(m.data, (const char *)&r, sizeof(r));

    if (msg_send(PORT_FILES, &m) < 0 || msg_recv((uint64_t)mio, &m) < 0) {
        printf("  [mv] no hay servidor de ficheros: arrancalo con 'f'\n");
        exit(1);
    }

    switch (m.type) {
    case FS_OK:
        printf("  %s -> %s\n", r.name, r.data);
        return 0;
    case FS_EXISTE:
        printf("  %s ya existe: no lo machaco\n", r.data);
        return 1;
    default:
        printf("  no he podido mover %s\n", r.name);
        printf("  (o no existe, o el destino esta en otra particion)\n");
        return 1;
    }
}
