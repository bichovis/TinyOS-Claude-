/* user/cat.c - Volcar un fichero de la tarjeta por la consola
 *
 * El nombre esta fijo aqui dentro porque todavia no hay forma de pasarle
 * argumentos a un proceso: el kernel lo crea y lo suelta, sin mas.
 */
#include "syscall.h"
#include "fs_abi.h"

#define FICHERO  "HOLA.TXT"

static struct message m;
static char linea[MSG_DATA_MAX + 1];

void _start(void) __attribute__((section(".text.start")));

void _start(void)
{
    int64_t mio = port_create(-1);
    if (mio < 0) { kprint("  [cat] sin puertos\n"); exit(1); }

    kprint("\n  --- " FICHERO " ---\n");

    for (uint64_t off = 0; ; ) {
        struct fs_request r;
        r.port = (unsigned long)mio;
        r.arg  = off;
        ucopy(r.name, FICHERO, sizeof(FICHERO));

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
