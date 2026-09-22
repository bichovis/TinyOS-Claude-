/* uart.c - Driver de la UART0 (PL011) del BCM2837
 *
 * La Pi 3B tiene dos UARTs:
 *   - UART0 = PL011, un IP de ARM, con reloj fijo -> baudrate estable.
 *   - UART1 = "mini UART", su baudrate depende del reloj del core (inestable).
 * Usamos la PL011. En hardware real hay que liberarla del Bluetooth
 * (dtoverlay=disable-bt en config.txt), en QEMU ya esta libre.
 */
#include "mmio.h"
#include "uart.h"

/* --- Registros GPIO --------------------------------------------------- */
#define GPIO_BASE     (PERIPHERAL_BASE + 0x200000)
#define GPFSEL1       (GPIO_BASE + 0x04)   /* funcion de los pines 10..19  */
#define GPPUD         (GPIO_BASE + 0x94)   /* control pull-up/pull-down    */
#define GPPUDCLK0     (GPIO_BASE + 0x98)   /* a que pines aplicarlo        */

/* --- Registros PL011 -------------------------------------------------- */
#define UART0_BASE    (PERIPHERAL_BASE + 0x201000)
#define UART0_DR      (UART0_BASE + 0x00) /* Data Register: leer/escribir  */
#define UART0_FR      (UART0_BASE + 0x18) /* Flag Register: estado de FIFOs*/
#define UART0_IBRD    (UART0_BASE + 0x24) /* divisor de baudios, parte ent.*/
#define UART0_FBRD    (UART0_BASE + 0x28) /* divisor de baudios, parte frac*/
#define UART0_LCRH    (UART0_BASE + 0x2C) /* formato de linea (8N1, FIFO)  */
#define UART0_CR      (UART0_BASE + 0x30) /* Control Register              */
#define UART0_IMSC    (UART0_BASE + 0x38) /* mascara de interrupciones     */
#define UART0_ICR     (UART0_BASE + 0x44) /* limpiar interrupciones        */

#define FR_RXFE       (1 << 4)            /* RX FIFO vacia                 */
#define FR_TXFF       (1 << 5)            /* TX FIFO llena                 */

#define INT_RX        (1 << 4)            /* RXIM/RXMIS: hay datos          */
#define INT_RT        (1 << 6)            /* RTIM/RTMIS: receive timeout    */

/* Buffer circular entre la interrupcion (productor) y el kernel (consumidor).
 * 'volatile' porque el handler lo modifica a espaldas del hilo principal. */
#define RXBUF_SIZE    64
static volatile char     rxbuf[RXBUF_SIZE];
static volatile uint32_t rx_head;         /* donde escribe la IRQ           */
static volatile uint32_t rx_tail;         /* donde lee el kernel            */

void uart_init(void)
{
    /* 1. Apagar la UART mientras la reconfiguramos. */
    mmio_write(UART0_CR, 0);

    /* 2. Poner los pines GPIO 14 (TXD) y 15 (RXD) en funcion alternativa 0.
     *    GPFSEL1 usa 3 bits por pin; el pin 14 empieza en el bit 12 y el 15
     *    en el bit 15. El valor 0b100 (=4) significa ALT0.                */
    uint32_t sel = mmio_read(GPFSEL1);
    sel &= ~((7u << 12) | (7u << 15));      /* limpiar ambos campos        */
    sel |=  ((4u << 12) | (4u << 15));      /* ALT0 en los dos             */
    mmio_write(GPFSEL1, sel);

    /* 3. Desactivar pull-up/pull-down en esos pines. El manual del BCM2835
     *    exige esta secuencia exacta con esperas de 150 ciclos.           */
    mmio_write(GPPUD, 0);
    delay_cycles(150);
    mmio_write(GPPUDCLK0, (1u << 14) | (1u << 15));
    delay_cycles(150);
    mmio_write(GPPUDCLK0, 0);

    /* 4. Limpiar cualquier interrupcion pendiente. */
    mmio_write(UART0_ICR, 0x7FF);

    /* 5. Baudrate 115200 con reloj de UART de 48 MHz:
     *      divisor = 48e6 / (16 * 115200) = 26.0416...
     *      parte entera   = 26
     *      parte fraccion = round(0.0416 * 64) = 3                        */
    mmio_write(UART0_IBRD, 26);
    mmio_write(UART0_FBRD, 3);

    /* 6. 8 bits, sin paridad, 1 bit de stop (8N1) y FIFOs activadas.
     *    bit4 = FEN (FIFO enable), bits 6:5 = 0b11 -> 8 bits de dato.     */
    mmio_write(UART0_LCRH, (1 << 4) | (3 << 5));

    /* 7. Sin interrupciones por ahora: en el paso 1 vamos por polling.    */
    mmio_write(UART0_IMSC, 0);

    /* 8. Encender: UARTEN(bit0) + TXE(bit8) + RXE(bit9).                  */
    mmio_write(UART0_CR, (1 << 0) | (1 << 8) | (1 << 9));
}

void uart_putc(char c)
{
    /* Espera a que la FIFO de transmision tenga hueco. */
    while (mmio_read(UART0_FR) & FR_TXFF) { }
    mmio_write(UART0_DR, (uint32_t)c);
}

char uart_getc(void)
{
    /* Espera a que llegue un byte. */
    while (mmio_read(UART0_FR) & FR_RXFE) { }
    return (char)(mmio_read(UART0_DR) & 0xFF);
}

void uart_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') uart_putc('\r');   /* los terminales quieren CRLF */
        uart_putc(*s);
    }
}

static void uart_hex(uint64_t value, int nibbles)
{
    for (int i = nibbles - 1; i >= 0; i--) {
        uint32_t d = (value >> (i * 4)) & 0xF;
        uart_putc(d < 10 ? (char)('0' + d) : (char)('A' + d - 10));
    }
}

void uart_hex32(uint32_t v) { uart_hex(v, 8);  }
void uart_hex64(uint64_t v) { uart_hex(v, 16); }

void uart_hex8(uint8_t v) { uart_hex(v, 2); }

void uart_dec(uint64_t v)
{
    char tmp[21];
    int  n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) uart_putc(tmp[n]);
}

/* --- Recepcion por interrupcion ---------------------------------------
 * A partir de aqui la PL011 nos avisa cuando llegan bytes, en vez de tener
 * que preguntarle en un bucle. El handler los mete en el buffer circular y
 * el kernel los saca cuando puede.
 */
void uart_enable_rx_irq(void)
{
    /* IMSC = Interrupt Mask Set/Clear. Un 1 aqui HABILITA esa interrupcion
     * (es "mask" en el sentido de "dejar pasar", no de "tapar").
     *   RXIM: la FIFO de recepcion ha llegado a su umbral de llenado.
     *   RTIM: hay bytes sueltos y ha pasado un rato sin llegar mas. Sin
     *         esto, teclear un solo caracter no generaria interrupcion. */
    mmio_write(UART0_ICR,  0x7FF);        /* limpiar lo que hubiera pendiente */
    mmio_write(UART0_IMSC, INT_RX | INT_RT);
}

void uart_irq(void)
{
    /* Vaciar la FIFO de recepcion: una sola interrupcion puede traer varios
     * bytes, y si dejamos alguno dentro la IRQ se volveria a disparar. */
    while (!(mmio_read(UART0_FR) & FR_RXFE)) {
        char c = (char)(mmio_read(UART0_DR) & 0xFF);
        uint32_t next = (rx_head + 1) % RXBUF_SIZE;
        if (next != rx_tail) {            /* si esta lleno, tiramos el byte */
            rxbuf[rx_head] = c;
            rx_head = next;
        }
    }
    mmio_write(UART0_ICR, INT_RX | INT_RT);   /* reconocer la interrupcion */
}

int uart_read(char *out)
{
    if (rx_tail == rx_head) return 0;     /* buffer vacio */
    *out    = rxbuf[rx_tail];
    rx_tail = (rx_tail + 1) % RXBUF_SIZE;
    return 1;
}
