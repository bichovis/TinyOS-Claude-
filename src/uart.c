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
#include "sync.h"
#include "sched.h"
#include "smp.h"
#include "irq.h"
#include "spinlock.h"

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
static struct waitqueue  rx_waiters;      /* hilos esperando un byte        */

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

/* La PL011 es una sola y los nucleos son cuatro. Sin cerrojo, entre el
 * "¿hay hueco?" y el "escribe" se cuela otro nucleo y se pierde un
 * caracter; con lineas enteras, el texto sale entrelazado.
 *
 * El cerrojo se coge en las funciones publicas y no aqui dentro: asi lo
 * que queda indivisible es la LINEA, que es la unidad que tiene sentido
 * leer, y no el caracter suelto. */
static struct spinlock   uart_lock = SPINLOCK("uart");
static volatile uint64_t uart_owner = (uint64_t)-1;  /* que nucleo lo tiene */
static volatile uint32_t uart_depth;                 /* cuantas veces       */

/* Coger el cerrojo, o apuntar una vuelta mas si ya era nuestro.
 *
 * Las IRQ se tapan ANTES de mirar nada: eso garantiza que, mientras
 * decidimos, este nucleo no va a saltar a un manejador que quiera escribir
 * tambien. Y 'owner' solo lo escribe quien tiene el cerrojo, asi que
 * leerlo sin el es seguro: si dice que es nuestro, es que lo es. */
static uint64_t uart_acquire(void)
{
    uint64_t f = irq_save();

    if (uart_owner == this_core() && uart_depth) {
        uart_depth++;
        return f;
    }

    spin_lock(&uart_lock);
    uart_owner = this_core();
    uart_depth = 1;
    return f;
}

static void uart_release(uint64_t f)
{
    if (--uart_depth == 0) {
        uart_owner = (uint64_t)-1;
        spin_unlock(&uart_lock);
    }
    irq_restore(f);
}

uint64_t uart_begin(void)        { return uart_acquire(); }
void     uart_end(uint64_t f)    { uart_release(f); }

static void putc_raw(char c)
{
    /* Espera a que la FIFO de transmision tenga hueco. */
    while (mmio_read(UART0_FR) & FR_TXFF) { }
    mmio_write(UART0_DR, (uint32_t)c);
}

void uart_putc(char c)
{
    uint64_t f = uart_acquire();
    putc_raw(c);
    uart_release(f);
}

char uart_getc(void)
{
    /* Espera a que llegue un byte. */
    while (mmio_read(UART0_FR) & FR_RXFE) { }
    return (char)(mmio_read(UART0_DR) & 0xFF);
}

void uart_puts(const char *s)
{
    uint64_t f = uart_acquire();
    for (; *s; s++) {
        if (*s == '\n') putc_raw('\r');   /* los terminales quieren CRLF */
        putc_raw(*s);
    }
    uart_release(f);
}

static void uart_hex(uint64_t value, int nibbles)
{
    uint64_t f = uart_acquire();
    for (int i = nibbles - 1; i >= 0; i--) {
        uint32_t d = (value >> (i * 4)) & 0xF;
        putc_raw(d < 10 ? (char)('0' + d) : (char)('A' + d - 10));
    }
    uart_release(f);
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

    uint64_t f = uart_acquire();
    while (n--) putc_raw(tmp[n]);
    uart_release(f);
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

    /* Despertar a quien estuviera esperando. wq_wake_all quiere sched_lock
     * cogido, y aqui hay que cogerlo: este camino viene de una IRQ, no de
     * sync.c. Se puede llamar desde un manejador porque solo cambia
     * estados; no planifica ni bloquea. */
    uint64_t f = sched_lock_irqsave();
    wq_wake_all(&rx_waiters);
    sched_unlock_irqrestore(f);
}

/* Version bloqueante: en vez de preguntar cada 10 ms si ha llegado algo,
 * el hilo se duerme y la interrupcion de la UART lo despierta. Mientras
 * tanto no consume ni un ciclo. */
char uart_getc_blocking(void)
{
    uint64_t f = sched_lock_irqsave();
    char c;

    while (!uart_read(&c))
        wq_wait(&rx_waiters);

    sched_unlock_irqrestore(f);
    return c;
}

int uart_read(char *out)
{
    if (rx_tail == rx_head) return 0;     /* buffer vacio */
    *out    = rxbuf[rx_tail];
    rx_tail = (rx_tail + 1) % RXBUF_SIZE;
    return 1;
}
