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

#define PROGRAMA  "HELLO.BIN"
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

void _start(void) __attribute__((section(".text.start")));

void _start(void)
{
    int64_t mio = port_create(-1);
    if (mio < 0) { kprint("  [run] sin puertos\n"); exit(1); }

    kprint("\n  [run] leyendo " PROGRAMA " de la tarjeta...\n");

    uint64_t total = 0;
    while (total < MAX_IMG) {
        struct fs_request r;
        r.port = (unsigned long)mio;
        r.arg  = total;
        ucopy(r.name, PROGRAMA, sizeof(PROGRAMA));

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

    int64_t pid = spawn(imagen, total);
    if (pid < 0) kprint("  [run] el kernel no lo ha querido\n");
    else         dec("  [run] arrancado como pid ", (uint64_t)pid, "\n");

    exit(0);
}
