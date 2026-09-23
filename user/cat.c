/* user/cat.c - Volcar un fichero de la tarjeta por la consola
 *
 * El nombre del fichero llega como argumento: "cat HOLA.TXT". El kernel
 * deja argc en x0 y argv en x1, con las cadenas en la propia pila del
 * proceso, que es el unico sitio que ya es suyo antes de existir.
 */
#include "syscall.h"
#include "fs_abi.h"

static struct message m;
static char linea[MSG_DATA_MAX + 1];

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    if (argc < 2) {
        kprint("\n  uso: cat NOMBRE.EXT\n");
        exit(1);
    }
    const char *fichero = argv[1];

    int64_t mio = port_create(-1);
    if (mio < 0) { kprint("  [cat] sin puertos\n"); exit(1); }

    kprint("\n  --- ");
    kprint(fichero);
    kprint(" ---\n");

    for (uint64_t off = 0; ; ) {
        struct fs_request r;
        r.port = (unsigned long)mio;
        r.arg  = off;
        ucopy(r.name, fichero, ustrlen(fichero) + 1);

        m.type = FS_READ;
        m.len  = sizeof(r);
        ucopy(m.data, (const char *)&r, sizeof(r));
        if (msg_send(PORT_FILES, &m) < 0) {
            kprint("  [cat] no hay servidor de ficheros: arrancalo con 'f'\n");
            exit(1);
        }

        if (msg_recv((uint64_t)mio, &m) < 0) break;
        if (m.type == FS_ERROR) { kprint("  [cat] no existe\n"); exit(1); }
        if (m.type != FS_OK || m.len == 0) break;

        for (uint64_t i = 0; i < m.len; i++) linea[i] = m.data[i];
        linea[m.len] = 0;
        kprint(linea);

        off += m.len;
    }

    kprint("  --- fin ---\n");
    exit(0);
}
