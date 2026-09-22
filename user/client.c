/* user/client.c - Cliente del servidor de consola
 *
 * No toca hardware ni usa SYS_write: para imprimir, le manda un mensaje al
 * servidor. Es como funciona una aplicacion en un microkernel: todo lo que
 * no es CPU ni memoria se pide por mensajes a otro proceso de usuario.
 */
#include "syscall.h"

void _start(void) __attribute__((section(".text.start")));

void _start(void)
{
    uint64_t pid = getpid();
    struct message m;

    for (int i = 1; i <= 5; i++) {
        m.type = CMSG_PRINT;

        /* Componer "hola numero N, uptime M ms" */
        uint64_t n = 0;
        const char *p = "hola numero ";
        while (*p) m.data[n++] = *p++;
        n += udec(m.data + n, (uint64_t)i);
        p = ", uptime ";
        while (*p) m.data[n++] = *p++;
        n += udec(m.data + n, uptime());
        p = " ms";
        while (*p) m.data[n++] = *p++;
        m.len = n;

        if (msg_send(PORT_CONSOLE, &m) < 0) {
            kprint("  [cliente] el servidor de consola no responde\n");
            exit(1);
        }
        sleep(45);
    }

    /* Ultimo mensaje y adios */
    m.type = CMSG_PRINT;
    uint64_t n = 0;
    const char *p = "me despido, pid ";
    while (*p) m.data[n++] = *p++;
    n += udec(m.data + n, pid);
    m.len = n;
    msg_send(PORT_CONSOLE, &m);

    exit(0);
}
