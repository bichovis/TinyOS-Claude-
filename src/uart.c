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
 * 'volatile' porque el handler lo modifica a espaldas del hilo principal.
 *
 * OJO A LO QUE HAY AQUI DENTRO: desde este paso, lo que entra en rxbuf no
 * es lo que se ha tecleado, es lo que YA SE PUEDE LEER. Entre una cosa y
 * la otra esta la disciplina de linea, mas abajo. */
#define RXBUF_SIZE   256
static volatile char     rxbuf[RXBUF_SIZE];
static volatile uint32_t rx_head;         /* donde escribe la IRQ           */
static volatile uint32_t rx_tail;         /* donde lee el kernel            */
static struct waitqueue  rx_waiters;      /* hilos esperando un byte        */

/* --- La disciplina de linea -------------------------------------------
 *
 * Entre la tecla y el programa hay algo mas que un cable, y ese algo es lo
 * que convierte un terminal en un terminal:
 *
 *   - Lo que escribes NO llega al programa hasta que pulsas Enter. Hasta
 *     entonces se puede corregir, y el programa no ve las correcciones:
 *     recibe la linea ya limpia. Por eso un backspace no es un caracter
 *     que haya que entender, es una tecla que deshace.
 *
 *   - El ECO lo hace esto, no el programa. Es la parte que parece un
 *     detalle y no lo es: si cada programa pintara lo que lee, apagar el
 *     eco para pedir una contrasenya exigiria que TODOS se acordaran de
 *     saber hacerlo. Estando aqui, se apaga en un sitio y vale para
 *     cualquiera, incluso para uno escrito antes de que existieran las
 *     contrasenyas.
 *
 *   - Y Ctrl-D no es una tecla que valga un caracter: es "entrega ya lo
 *     que tengas". Si no tienes nada, lo que se entrega es el final de la
 *     entrada. De ahi que Ctrl-D a mitad de linea no cierre nada y dos
 *     seguidos si.
 *
 * En Unix esto se llama "line discipline" y vive en el kernel, no en el
 * driver del hardware ni en el programa. Aqui, igual, y por el mismo
 * motivo: es lo unico que esta a la vez entre todos los teclados posibles
 * y todos los programas posibles. */
#define LINEA_MAX    128

static void putc_raw(char c);        /* mas abajo: escribir sin cerrojo */

static char     linea[LINEA_MAX];    /* lo que se esta escribiendo TODAVIA */
static uint32_t linea_n;
static int      term_modo = T_ECO | T_CANONICO;
static int      eof_pendiente;       /* un Ctrl-D sobre una linea vacia */

/* Meter un byte en la cola de lo legible. Devuelve 0 si no cabia. */
static int rx_meter(char c)
{
    uint32_t next = (rx_head + 1) % RXBUF_SIZE;
    if (next == rx_tail) return 0;
    rxbuf[rx_head] = c;
    rx_head = next;
    return 1;
}

/* El eco. Va por el mismo camino que cualquier escritura del kernel, con
 * el cerrojo de la UART cogido: el que llama puede estar dentro de una
 * interrupcion, y uart_acquire desactiva las IRQ de este nucleo, asi que
 * no puede pelearse consigo mismo. */
static void eco(const char *s, uint64_t n)
{
    if (!(term_modo & T_ECO)) return;

    uint64_t f = uart_begin();
    for (uint64_t i = 0; i < n; i++) {
        if (s[i] == '\n') putc_raw('\r');
        putc_raw(s[i]);
    }
    uart_end(f);
}

/* Volcar la linea a lo legible. Se hace entera o no se hace: media linea
 * en la cola seria una linea que el programa leeria como completa. */
static void entregar_linea(void)
{
    for (uint32_t i = 0; i < linea_n; i++)
        if (!rx_meter(linea[i])) break;
    linea_n = 0;
}

/* Un caracter que acaba de llegar, venga del hardware o de un driver de
 * EL0. Este es el unico sitio donde se decide que significa cada tecla. */
static void disciplina(char c)
{
    /* Modo crudo: cada tecla es un byte y nadie interpreta nada. Lo que
     * necesita un editor de pantalla, y lo que NO necesita un shell. */
    if (!(term_modo & T_CANONICO)) {
        rx_meter(c);
        eco(&c, 1);
        return;
    }

    switch (c) {
    case '\r':
    case '\n':
        /* El salto SI va dentro de la linea: es lo que le dice a quien lee
         * donde acaba, y es lo que hace que fgets funcione. */
        if (linea_n < LINEA_MAX) linea[linea_n++] = '\n';
        eco("\n", 1);
        entregar_linea();
        return;

    case 8:
    case 127:
        /* Borrar es tres cosas en la pantalla -ir atras, tapar, ir atras-
         * y una sola en el buffer. El programa no se entera de ninguna. */
        if (linea_n) { linea_n--; eco("\b \b", 3); }
        return;

    case 21:                            /* Ctrl-U: tirar la linea entera */
        while (linea_n) { linea_n--; eco("\b \b", 3); }
        return;

    case 4:                             /* Ctrl-D */
        if (linea_n) entregar_linea();  /* entrega lo que haya, sin salto */
        else         eof_pendiente = 1; /* nada que entregar: se acabo */
        return;

    default:
        /* Los demas caracteres de control se tiran en vez de guardarse:
         * meterlos en la linea hace que el programa reciba basura que no
         * pidio y que el eco descoloque la pantalla. */
        if ((unsigned char)c < ' ' && c != '\t') return;
        if (linea_n >= LINEA_MAX - 1) return;     /* sitio para el salto */

        linea[linea_n++] = c;
        eco(&c, 1);
        return;
    }
}

/* Tirar lo que hubiera a medias: la linea sin terminar Y lo ya entregado.
 *
 * Lo llaman Ctrl-C y Ctrl-Z, porque una senyal del terminal cancela
 * tambien lo que estabas escribiendo. Sin esto, la media orden se queda
 * esperando y se pega a la siguiente: tras un "hol" interrumpido, teclear
 * "pwd" ejecutaba "holpwd". El sintoma no apunta a la causa, porque la
 * linea a medias ya no vive en el shell -vive aqui- y el shell no tiene
 * forma de saber que habia algo que tirar.
 *
 * Que el eco haya cambiado de sitio se lleva consigo esta
 * responsabilidad, y olvidarla es facil precisamente porque antes no
 * existia: cuando la linea era del shell, el shell la perdia al volver a
 * empezar sin que nadie tuviera que hacer nada. */
void uart_descartar_entrada(void)
{
    uint64_t f = sched_lock_irqsave();
    linea_n       = 0;
    rx_tail       = rx_head;
    eof_pendiente = 0;
    sched_unlock_irqrestore(f);
}

/* Cambiar el modo del terminal, y devolver el que habia. Con -1 solo se
 * consulta, que es lo que necesita quien quiere restaurarlo despues. */
int uart_modo(int nuevo)
{
    int antes = term_modo;

    if (nuevo >= 0) {
        /* Al salir del modo crudo, lo que estuviera a medias se tira: son
         * teclas que se escribieron bajo otras reglas. */
        if ((nuevo & T_CANONICO) && !(antes & T_CANONICO)) linea_n = 0;
        term_modo = nuevo;
    }
    return antes;
}

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
    int interrumpir = 0, parar = 0;

    while (!(mmio_read(UART0_FR) & FR_RXFE)) {
        char c = (char)(mmio_read(UART0_DR) & 0xFF);

        /* Ctrl-C no es un caracter que leer: es una orden. Se lo queda el
         * driver y se convierte en una senyal para quien esta en primer
         * plano. Esto es lo que hace un terminal de verdad, y es la razon
         * por la que Ctrl-C funciona aunque el programa no lo lea.
         *
         * Ctrl-Z es la misma idea con otra respuesta: en vez de "acaba con
         * esto", "deja esto donde esta". */
        if (c == 3)  { interrumpir = 1; continue; }
        if (c == 26) { parar = 1; continue; }

        disciplina(c);
    }
    mmio_write(UART0_ICR, INT_RX | INT_RT);   /* reconocer la interrupcion */

    /* Despertar a quien estuviera esperando. wq_wake_all quiere sched_lock
     * cogido, y aqui hay que cogerlo: este camino viene de una IRQ, no de
     * sync.c. Se puede llamar desde un manejador porque solo cambia
     * estados; no planifica ni bloquea. */
    uint64_t f = sched_lock_irqsave();
    wq_wake_all(&rx_waiters);
    sched_unlock_irqrestore(f);

    /* Fuera del cerrojo: los dos lo vuelven a coger. */
    if (interrumpir) task_console_interrupt();
    if (parar)       task_console_stop();
}

/* Meter bytes en el buffer de recepcion desde fuera del driver.
 *
 * Cuando el teclado lo lleva un proceso de EL0, el kernel ya no toca la
 * FIFO de la UART: las teclas le llegan por aqui, via SYS_console_push.
 * De la mitad para abajo -el buffer, los que esperan, read()- nada cambia.
 * Esa es la ventaja de haber separado el driver de la cola: se puede
 * sustituir el hardware por un proceso sin tocar a quien lee. */
uint64_t uart_perdidos;               /* teclas tiradas por falta de sitio */

void uart_push(const char *buf, uint64_t n)
{
    uint64_t tirados = 0;
    uint32_t antes = linea_n;

    for (uint64_t i = 0; i < n; i++) {
        /* Sin sitio en la linea Y sin sitio en la cola: eso si es perder
         * teclas. Que la linea crezca no es perderlas, es esperarlas. */
        uint32_t libre = (rx_tail + RXBUF_SIZE - rx_head - 1) % RXBUF_SIZE;
        if (!libre && linea_n >= LINEA_MAX - 1) { tirados = n - i; break; }

        disciplina(buf[i]);
    }
    (void)antes;

    uint64_t f = sched_lock_irqsave();
    wq_wake_all(&rx_waiters);
    sched_unlock_irqrestore(f);

    /* El anillo son 64 bytes y quien lee va a su ritmo. Si llega mas de lo
     * que cabe, sobra, y lo que sobra SE TIRA: no hay a donde meterlo y no
     * hay forma de decirle al otro lado que pare, porque no hay control de
     * flujo en este cable.
     *
     * Lo que si se puede es no perderlo en silencio. Sin este aviso el
     * sintoma es un shell esperando una orden que nunca termina -se perdio
     * el salto de linea- y no hay manera de adivinar por que. Un aviso por
     * rafaga, no por byte, o el remedio seria peor. */
    if (tirados) {
        uart_perdidos += tirados;
        uint64_t lf = uart_begin();
        uart_puts("\n  [kernel] entrada demasiado rapida: ");
        uart_dec(tirados);
        uart_puts(" caracteres perdidos (");
        uart_dec(uart_perdidos);
        uart_puts(" en total)\n");
        uart_end(lf);
    }
}

/* Version bloqueante: en vez de preguntar cada 10 ms si ha llegado algo,
 * el hilo se duerme y la interrupcion de la UART lo despierta. Mientras
 * tanto no consume ni un ciclo. */
/* Sacar hasta 'n' bytes de lo que ya es legible, esperando si no hay nada.
 *
 * Antes esto entregaba UN byte por llamada, y cada byte era una excepcion,
 * un cambio de privilegio y una vuelta entera por la tabla de vectores.
 * Una orden de treinta letras costaba treinta viajes. Ahora, como la
 * disciplina no suelta nada hasta el Enter, un read trae la linea entera y
 * el viaje es uno. No se ha optimizado nada: sale de haber puesto la
 * decision de "cuando hay algo que leer" en el sitio correcto.
 *
 * Devuelve 0 si se acabo la entrada -un Ctrl-D sobre una linea vacia- y
 * -EINTR si lo interrumpio una senyal. El errno y no un -1 a secas: quien
 * lee tiene que poder distinguir "te han interrumpido" de "se acabo", y
 * son dos cosas que llevan a sitios opuestos -volver a intentarlo, o
 * marcharse-. Escribir aqui un -1 costo que el shell se despidiera
 * educadamente con cada Ctrl-C. Otra vez. */
int64_t uart_leer(char *dst, uint64_t n)
{
    uint64_t f = sched_lock_irqsave();

    while (rx_tail == rx_head) {
        /* El final de la entrada solo cuenta con la cola vacia: un Ctrl-D
         * detras de texto entrega el texto, y el final llega despues. */
        if (eof_pendiente) {
            eof_pendiente = 0;
            sched_unlock_irqrestore(f);
            return 0;
        }

        if (wq_wait(&rx_waiters) < 0) {   /* una senyal, no una tecla */
            sched_unlock_irqrestore(f);
            return -EINTR;
        }
    }

    /* Una linea COMO MUCHO, aunque quepan mas y aunque las pidan.
     *
     * Esto parece tacanyeria y es lo que hace seguro leer del terminal con
     * un cubo. Si un read pudiera llevarse dos lineas, el shell se
     * quedaria dentro con la segunda -que el usuario escribio para el
     * programa que viene despues- y ese programa esperaria algo que ya no
     * va a llegar. Era el motivo por el que stdio leia el terminal de uno
     * en uno desde el paso 49.
     *
     * Con la linea como frontera, el cubo deja de poder robar: lo que se
     * lleva es exactamente lo que se escribio para ti. Y de paso una orden
     * de treinta letras pasa de treinta viajes al kernel a uno. La mejora
     * de velocidad es un efecto secundario de una decision sobre de quien
     * son los caracteres. */
    uint64_t i = 0;
    while (i < n && rx_tail != rx_head) {
        char c = rxbuf[rx_tail];
        rx_tail = (rx_tail + 1) % RXBUF_SIZE;
        dst[i++] = c;
        if ((term_modo & T_CANONICO) && c == '\n') break;
    }

    sched_unlock_irqrestore(f);
    return (int64_t)i;
}

int uart_read(char *out)
{
    if (rx_tail == rx_head) return 0;     /* buffer vacio */
    *out    = rxbuf[rx_tail];
    rx_tail = (rx_tail + 1) % RXBUF_SIZE;
    return 1;
}
