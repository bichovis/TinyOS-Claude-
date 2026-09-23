/* user/conserver.c - Servidor de consola. UN DRIVER EN ESPACIO DE USUARIO.
 *
 * Este proceso corre en EL0, sin privilegios, y aun asi habla directamente
 * con el hardware: el kernel le ha mapeado la pagina de registros de la
 * PL011 en su espacio de direcciones como memoria Device.
 *
 * No usa SYS_write ni ninguna otra llamada para imprimir. Escribe en los
 * registros de la UART igual que lo hacia el driver del kernel en el paso 1,
 * solo que ahora, si tiene un bug, muere el y el sistema sigue.
 *
 * Su trabajo: recibir mensajes del puerto 0 y sacarlos por el puerto serie.
 */
#include "syscall.h"

#define UART_DR   0x00              /* registro de datos               */
#define UART_FR   0x18              /* registro de estado              */
#define FR_TXFF   (1u << 5)         /* FIFO de transmision llena       */

static void hw_putc(uint64_t base, char c)
{
    volatile unsigned int *fr = (volatile unsigned int *)(base + UART_FR);
    volatile unsigned int *dr = (volatile unsigned int *)(base + UART_DR);

    while (*fr & FR_TXFF)           /* esperar hueco, igual que en el kernel */
        ;
    *dr = (unsigned int)c;
}

static void hw_write(uint64_t base, const char *s, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        if (s[i] == '\n') hw_putc(base, '\r');
        hw_putc(base, s[i]);
    }
}

static void hw_puts(uint64_t base, const char *s)
{
    hw_write(base, s, ustrlen(s));
}

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint64_t uart = mmio_base();
    if (!uart) {
        kprint("  [conserver] no tengo MMIO, no puedo trabajar\n");
        exit(1);
    }

    int64_t port = port_create(PORT_CONSOLE);
    if (port != PORT_CONSOLE) {
        kprint("  [conserver] no he podido quedarme el puerto 0\n");
        exit(1);
    }

    /* A partir de aqui ya no volvemos a pedirle nada al kernel para
     * imprimir: escribimos en el hardware nosotros mismos. */
    hw_puts(uart, "\n  [conserver] driver de consola vivo en EL0, puerto 0\n");

    char line[80];
    for (;;) {
        struct message m;
        if (msg_recv((uint64_t)port, &m) < 0)
            break;                       /* el puerto ha desaparecido */

        if (m.type != CMSG_PRINT)
            continue;

        /* Prefijo con el pid del remitente. El kernel lo rellena, asi que
         * un cliente no puede hacerse pasar por otro. */
        uint64_t n = 0;
        line[n++] = ' '; line[n++] = ' ';
        line[n++] = '<'; line[n++] = 'p'; line[n++] = 'i'; line[n++] = 'd';
        line[n++] = ' ';
        n += udec(line + n, m.from);
        line[n++] = '>'; line[n++] = ' ';

        uint64_t len = m.len;
        if (len > MSG_DATA_MAX) len = MSG_DATA_MAX;
        ucopy(line + n, m.data, len);
        n += len;
        line[n++] = '\n';

        hw_write(uart, line, n);
    }

    hw_puts(uart, "  [conserver] me voy\n");
    exit(0);
}
