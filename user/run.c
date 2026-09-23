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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include "fs_abi.h"

#define MAX_IMG   (32 * 1024)

static struct message m;
static unsigned char imagen[MAX_IMG];

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

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("\n  uso: run PROGRAMA.ELF [argumentos]\n");
        exit(1);
    }
    const char *programa = argv[1];
    juntar_args(argc, argv);

    int64_t mio = port_create(-1);
    if (mio < 0) { printf("  [run] sin puertos\n"); exit(1); }

    printf("\n  [run] leyendo ");
    printf("%s", programa);
    printf(" de la tarjeta...\n");

    uint64_t total = 0;
    while (total < MAX_IMG) {
        struct fs_request r;
        r.port = (unsigned long)mio;
        r.arg  = total;
        memcpy(r.name, programa, strlen(programa) + 1);

        m.type = FS_READ;
        m.len  = sizeof(r);
        memcpy(m.data, (const char *)&r, sizeof(r));
        if (msg_send(PORT_FILES, &m) < 0) {
            printf("  [run] no hay servidor de ficheros: arrancalo con 'f'\n");
            exit(1);
        }

        if (msg_recv((uint64_t)mio, &m) < 0) break;
        if (m.type == FS_ERROR) { printf("  [run] no existe\n"); exit(1); }
        if (m.type != FS_OK || m.len == 0) break;

        for (uint64_t i = 0; i < m.len; i++)
            imagen[total + i] = (unsigned char)m.data[i];
        total += m.len;
    }

    printf("  [run] %lu bytes leidos, se los paso al kernel\n", total);

    int64_t pid = spawn(imagen, total, args_hijo);
    if (pid < 0) printf("  [run] el kernel no lo ha querido\n");
    else         printf("  [run] arrancado como pid %lu\n", (uint64_t)pid);

    exit(0);
}
