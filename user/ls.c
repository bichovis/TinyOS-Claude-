/* user/ls.c - Listar el directorio raiz de la tarjeta
 *
 * No toca la SD ni sabe lo que es FAT: solo habla por el puerto de
 * ficheros. Esa es toda la gracia de tener un servidor.
 */
#include "syscall.h"
#include "fs_abi.h"

static struct message m;

void _start(void) __attribute__((section(".text.start")));

void _start(void)
{
    int64_t mio = port_create(-1);
    if (mio < 0) { kprint("  [ls] sin puertos\n"); exit(1); }

    kprint("\n  Contenido de la tarjeta:\n");

    for (uint64_t i = 0; ; i++) {
        struct fs_request r;
        r.port = (unsigned long)mio;
        r.arg  = i;
        for (int j = 0; j < 32; j++) r.name[j] = 0;

        m.type = FS_LIST;
        m.len  = sizeof(r);
        ucopy(m.data, (const char *)&r, sizeof(r));
        if (msg_send(PORT_FILES, &m) < 0) {
            kprint("  [ls] no hay servidor de ficheros: arrancalo con 'f'\n");
            exit(1);
        }

        if (msg_recv((uint64_t)mio, &m) < 0) break;
        if (m.type != FS_OK) break;

        struct fs_info *info = (struct fs_info *)m.data;
        char num[24];
        uint64_t n = udec(num, info->size);
        num[n] = 0;

        kprint("    ");
        kprint(info->name);
        for (uint64_t k = ustrlen(info->name); k < 14; k++) kprint(" ");
        kprint(num);
        kprint(" bytes\n");
    }

    exit(0);
}
