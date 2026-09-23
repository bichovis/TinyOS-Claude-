/* user/write.c - Escribir un fichero en la tarjeta
 *
 *   write NOTA.TXT esto es una prueba
 *
 * Crea el fichero (o lo vacia si ya estaba) y le mete el resto de la
 * linea. No sabe nada de FAT ni de la tarjeta: solo habla por el puerto
 * de ficheros, igual que 'cat'.
 */
#include "syscall.h"
#include "fs_abi.h"

static struct message m;
static int64_t        mio;

/* Mandar una peticion y esperar la respuesta. Devuelve el tipo que vuelva,
 * o FS_ERROR si la conversacion se rompe. */
static uint64_t pedir(uint64_t tipo, const char *nombre, uint64_t arg,
                      const char *datos, uint64_t n)
{
    struct fs_request r;
    r.port = (unsigned long)mio;
    r.arg  = arg;

    for (int i = 0; i < FS_NAME_MAX; i++) r.name[i] = 0;
    ucopy(r.name, nombre, ustrlen(nombre) + 1);

    for (uint64_t i = 0; i < FS_CHUNK; i++)
        r.data[i] = (i < n && datos) ? datos[i] : 0;

    m.type = tipo;
    m.len  = n;
    ucopy(m.data, (const char *)&r, sizeof(r));

    if (msg_send(PORT_FILES, &m) < 0)   return FS_ERROR;
    if (msg_recv((uint64_t)mio, &m) < 0) return FS_ERROR;
    return m.type;
}

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    if (argc < 3) {
        kprint("\n  uso: write NOMBRE.EXT texto...\n");
        exit(1);
    }

    mio = port_create(-1);
    if (mio < 0) { kprint("  [write] sin puertos\n"); exit(1); }

    const char *fichero = argv[1];

    if (pedir(FS_CREATE, fichero, 0, 0, 0) != FS_OK) {
        kprint("  [write] no he podido crearlo\n");
        exit(1);
    }

    /* Juntar el resto de la linea y mandarla a trozos. */
    uint64_t off = 0;
    char     buf[FS_CHUNK];
    uint64_t n = 0;

    for (int i = 2; i < argc; i++) {
        if (i > 2 && n < FS_CHUNK) buf[n++] = ' ';
        for (const char *s = argv[i]; *s; s++) {
            if (n == FS_CHUNK) {
                if (pedir(FS_WRITE, fichero, off, buf, n) != FS_OK) goto mal;
                off += n;
                n = 0;
            }
            buf[n++] = *s;
        }
    }
    if (n < FS_CHUNK) buf[n++] = '\n';

    if (n && pedir(FS_WRITE, fichero, off, buf, n) != FS_OK) goto mal;

    kprint("\n  escrito en ");
    kprint(fichero);
    kprint("\n");
    exit(0);

mal:
    kprint("\n  [write] la tarjeta ha fallado escribiendo\n");
    exit(1);
}
