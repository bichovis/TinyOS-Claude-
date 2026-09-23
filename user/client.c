/* user/client.c - Cliente del servidor de consola
 *
 * No toca hardware ni usa SYS_write: para imprimir, le manda un mensaje al
 * servidor. Es como funciona una aplicacion en un microkernel: todo lo que
 * no es CPU ni memoria se pide por mensajes a otro proceso de usuario.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint64_t pid = getpid();
    struct message m;

    for (int i = 1; i <= 5; i++) {
        m.type = CMSG_PRINT;

        /* Componer "hola numero N, uptime M ms".
         *
         * Esto eran doce lineas de ir empujando caracteres. Es el sitio
         * donde mas se nota la libc: el mensaje va a MEMORIA, no a la
         * salida, y snprintf es exactamente printf con el destino
         * cambiado. */
        m.len = (uint64_t)snprintf(m.data, sizeof(m.data),
                                   "hola numero %d, uptime %lu ms", i, uptime());

        if (msg_send(PORT_CONSOLE, &m) < 0) {
            printf("  [cliente] el servidor de consola no responde\n");
            exit(1);
        }
        sleep(45);
    }

    /* Ultimo mensaje y adios */
    m.type = CMSG_PRINT;
    m.len  = (uint64_t)snprintf(m.data, sizeof(m.data), "me despido, pid %lu", pid);
    msg_send(PORT_CONSOLE, &m);

    exit(0);
}
