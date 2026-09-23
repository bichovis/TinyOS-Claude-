/* user/run.c - Cargar un programa de la tarjeta y ejecutarlo
 *
 * Esto es lo que cierra el circulo: hasta ahora todos los programas venian
 * empotrados en la imagen del kernel por tools/bin2c.py. Este los saca de
 * la SD.
 *
 * Fijate en quien hace que. El kernel NO sabe leer ficheros y no le hace
 * falta: este proceso lee los bytes del servidor de ficheros y luego le
 * pide al kernel, con spawn(), que los convierta en un proceso. Si fuera
 * al reves -el kernel leyendo de un servidor de usuario- el kernel
 * dependeria de un proceso que puede morirse, y eso es justo lo que un
 * microkernel no hace.
 */
#include "syscall.h"
#include "fs_abi.h"

#define MAX_IMG   (32 * 1024)

static struct message m;
static unsigned char imagen[MAX_IMG];

static void dec(const char *antes, uint64_t v, const char *despues)
{
    char num[24];
    uint64_t n = udec(num, v);
    num[n] = 0;
    kprint(antes); kprint(num); kprint(despues);
}

void _start(int argc, char **argv) __attribute__((section(".text.start")));

/* Rehacer la linea de argumentos para el hijo: todo lo que venga despues
 * de "run". Asi "run HELLO.ELF uno dos" arranca HELLO.ELF viendose a si
 * mismo como argv[0] y con "uno dos" detras. */
static char args_hijo[128];

static void juntar_args(int argc, char **argv)
{
    uint64_t o = 0;
    for (int i = 1; i < argc; i++) {
        if (o && o < sizeof(args_hijo) - 1) args_hijo[o++] = ' ';
        for (const char *s = argv[i]; *s && o < sizeof(args_hijo) - 1; s++)
            args_hijo[o++] = *s;
    }
    args_hijo[o] = 0;
}

void _start(int argc, char **argv)
{
    if (argc < 2) {
        kprint("\n  uso: run PROGRAMA.ELF [argumentos]\n");
        exit(1);
    }
    const char *programa = argv[1];
    juntar_args(argc, argv);

    int64_t mio = port_create(-1);
    if (mio < 0) { kprint("  [run] sin puertos\n"); exit(1); }

    kprint("\n  [run] leyendo ");
    kprint(programa);
    kprint(" de la tarjeta...\n");

    uint64_t total = 0;
    while (total < MAX_IMG) {
        struct fs_request r;
        r.port = (unsigned long)mio;
        r.arg  = total;
        ucopy(r.name, programa, ustrlen(programa) + 1);

        m.type = FS_READ;
        m.len  = sizeof(r);
        ucopy(m.data, (const char *)&r, sizeof(r));
        if (msg_send(PORT_FILES, &m) < 0) {
            kprint("  [run] no hay servidor de ficheros: arrancalo con 'f'\n");
            exit(1);
        }

        if (msg_recv((uint64_t)mio, &m) < 0) break;
        if (m.type == FS_ERROR) { kprint("  [run] no existe\n"); exit(1); }
        if (m.type != FS_OK || m.len == 0) break;

        for (uint64_t i = 0; i < m.len; i++)
            imagen[total + i] = (unsigned char)m.data[i];
        total += m.len;
    }

    dec("  [run] ", total, " bytes leidos, se los paso al kernel\n");

    int64_t pid = spawn(imagen, total, args_hijo);
    if (pid < 0) kprint("  [run] el kernel no lo ha querido\n");
    else         dec("  [run] arrancado como pid ", (uint64_t)pid, "\n");

    exit(0);
}
