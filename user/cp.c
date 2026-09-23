/* user/cp.c - Copiar un fichero de la tarjeta
 *
 *   cp HELLO.ELF COPIA.ELF
 *
 * Lee de uno y escribe en otro, de trozo en trozo, sin guardarse el
 * fichero entero en memoria. Es la primera orden que mueve volumen de
 * verdad: copiar 8 KB son casi noventa idas y venidas por el IPC y varios
 * clusters encadenados en la FAT.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include "fs_abi.h"

static struct message m;
static int64_t        mio;
static char           trozo[FS_CHUNK];

/* Una peticion y su respuesta. Devuelve el tipo, y deja los datos (si los
 * hay) en m.data con m.len bytes. */
static uint64_t pedir(uint64_t tipo, const char *nombre, uint64_t arg,
                      const char *datos, uint64_t n)
{
    struct fs_request r;
    r.port = (unsigned long)mio;
    r.arg  = arg;

    for (int i = 0; i < FS_NAME_MAX; i++) r.name[i] = 0;
    memcpy(r.name, nombre, strlen(nombre) + 1);
    for (uint64_t i = 0; i < FS_CHUNK; i++)
        r.data[i] = (i < n && datos) ? datos[i] : 0;

    m.type = tipo;
    m.len  = n;
    memcpy(m.data, (const char *)&r, sizeof(r));

    if (msg_send(PORT_FILES, &m) < 0)    return FS_ERROR;
    if (msg_recv((uint64_t)mio, &m) < 0) return FS_ERROR;
    return m.type;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("\n  uso: cp ORIGEN DESTINO\n");
        exit(1);
    }

    mio = port_create(-1);
    if (mio < 0) { printf("  [cp] sin puertos\n"); exit(1); }

    const char *origen  = argv[1];
    const char *destino = argv[2];

    if (pedir(FS_SIZE, origen, 0, 0, 0) != FS_OK) {
        printf("\n  ");
        printf("%s", origen);
        printf(": no esta en la tarjeta\n");
        exit(1);
    }

    if (pedir(FS_CREATE, destino, 0, 0, 0) != FS_OK) {
        printf("\n  [cp] no he podido crear el destino\n");
        exit(1);
    }

    uint64_t off = 0;
    for (;;) {
        if (pedir(FS_READ, origen, off, 0, 0) != FS_OK) break;
        if (m.len == 0) break;

        uint64_t n = m.len;
        for (uint64_t i = 0; i < n; i++) trozo[i] = m.data[i];

        if (pedir(FS_WRITE, destino, off, trozo, n) != FS_OK) {
            printf("\n  [cp] la tarjeta ha fallado escribiendo\n");
            exit(1);
        }
        off += n;
    }

    printf("\n  copiados ");
    printf("%lu", off);
    printf(" bytes en ");
    printf("%s", destino);
    printf("\n");
    exit(0);
}
