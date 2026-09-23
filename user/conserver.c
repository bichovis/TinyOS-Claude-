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
 * Su trabajo: sacar por el puerto serie lo que le mandan, y meter en el
 * sistema lo que se teclea. Las dos direcciones, en EL0.
 *
 * LO SEGUNDO ES LO DIFICIL, y merece explicacion. Un proceso de EL0 no
 * puede recibir interrupciones: las interrupciones entran por la VBAR, que
 * es de EL1, y ahi no se pisa sin privilegios. Un driver de usuario parece
 * imposible por definicion.
 *
 * La salida es no darle la interrupcion, sino el aviso. El kernel se queda
 * con la unica parte que de verdad necesita privilegio -atender el vector,
 * enmascarar la fuente- y todo lo demas se convierte en un mensaje:
 *
 *     llega la IRQ 57 -> el kernel la enmascara y manda CMSG_IRQ al puerto
 *                     -> el driver despierta, vacia la FIFO, y hace irq_ack
 *                     -> el kernel la vuelve a abrir
 *
 * Para este proceso una interrupcion no se distingue de cualquier otro
 * mensaje: entra por el mismo msg_recv y se atiende en el mismo bucle. No
 * hay contexto de interrupcion, ni reentrada, ni carreras con el codigo
 * normal. Esa uniformidad es la razon de ser del microkernel.
 */
#include "syscall.h"

#define UART_DR   0x00              /* registro de datos               */
#define UART_FR   0x18              /* registro de estado              */
#define UART_ICR  0x44              /* reconocer interrupciones        */
#define FR_TXFF   (1u << 5)         /* FIFO de transmision llena       */
#define FR_RXFE   (1u << 4)         /* FIFO de recepcion vacia         */
#define INT_RX    (1u << 4)
#define INT_RT    (1u << 6)

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

/* Vaciar la FIFO de recepcion y entregar lo que traiga.
 *
 * Hay que vaciarla ENTERA: una sola interrupcion puede traer varios bytes,
 * y si queda alguno dentro la UART la volveria a levantar en cuanto se
 * desenmascare, con el agravante de que aqui el viaje de vuelta pasa por
 * el planificador. */
static void drenar(uint64_t base)
{
    volatile unsigned int *fr  = (volatile unsigned int *)(base + UART_FR);
    volatile unsigned int *dr  = (volatile unsigned int *)(base + UART_DR);
    volatile unsigned int *icr = (volatile unsigned int *)(base + UART_ICR);

    char buf[32];
    uint64_t n = 0;
    int interrumpir = 0;

    while (!(*fr & FR_RXFE)) {
        char c = (char)(*dr & 0xFF);

        /* Ctrl-C no es un caracter que leer: es una orden, y decidirlo es
         * trabajo del terminal. Lo que este proceso NO puede saber es a
         * quien hay que interrumpir -eso esta en la tabla de procesos- asi
         * que de eso se encarga el kernel. */
        if (c == 3) { interrumpir = 1; continue; }

        if (n < sizeof(buf)) buf[n++] = c;
    }

    *icr = INT_RX | INT_RT;              /* reconocer en el propio chip */

    if (n) console_push(buf, n);
    if (interrumpir) console_int();
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
    /* Pedir el teclado. A partir de este momento el kernel no vuelve a
     * mirar la FIFO de recepcion: las teclas pasan por aqui. */
    int teclado = (irq_register(IRQ_UART, (uint64_t)port) == 0);

    hw_puts(uart, "\n  [conserver] driver de consola vivo en EL0, puerto 0");
    hw_puts(uart, teclado ? ", con teclado\n" : ", solo salida\n");

    /* Puede haber teclas esperando en la FIFO desde antes de registrarnos.
     * Si no se vacian ahora, la UART no volvera a interrumpir -su nivel ya
     * esta por encima del umbral- y el teclado naceria muerto. */
    if (teclado) drenar(uart);

    char line[80];
    for (;;) {
        struct message m;
        if (msg_recv((uint64_t)port, &m) < 0)
            break;                       /* el puerto ha desaparecido */

        /* Una interrupcion, atendida como un mensaje mas. */
        if (m.type == CMSG_IRQ) {
            drenar(uart);
            irq_ack(IRQ_UART);
            continue;
        }

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
