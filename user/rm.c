/* user/rm.c - Borrar un fichero de la tarjeta
 *
 * Borrar en FAT es poner un 0xE5 en la primera letra del nombre y devolver
 * sus clusters a la tabla. Los datos siguen ahi intactos, y por eso los
 * ficheros borrados se pueden recuperar mientras nadie escriba encima.
 */
#include "syscall.h"
#include "fs_abi.h"

static struct message m;

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    if (argc < 2) {
        kprint("\n  uso: rm NOMBRE.EXT\n");
        exit(1);
    }

    int64_t mio = port_create(-1);
    if (mio < 0) { kprint("  [rm] sin puertos\n"); exit(1); }

    struct fs_request r;
    r.port = (unsigned long)mio;
    r.arg  = 0;
    for (int i = 0; i < FS_NAME_MAX; i++) r.name[i] = 0;
    ucopy(r.name, argv[1], ustrlen(argv[1]) + 1);

    m.type = FS_DELETE;
    m.len  = 0;
    ucopy(m.data, (const char *)&r, sizeof(r));

    if (msg_send(PORT_FILES, &m) < 0 || msg_recv((uint64_t)mio, &m) < 0) {
        kprint("\n  [rm] no hay servidor de ficheros\n");
        exit(1);
    }

    if (m.type != FS_OK) {
        kprint("\n  ");
        kprint(argv[1]);
        kprint(": no esta en la tarjeta\n");
        exit(1);
    }

    kprint("\n  borrado ");
    kprint(argv[1]);
    kprint("\n");
    exit(0);
}
