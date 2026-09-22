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
