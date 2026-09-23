/* user/sd.c - Driver de la tarjeta SD (EMMC / SDHCI), en EL0
 *
 * Arrancar una tarjeta SD es una conversacion con un protocolo fijo, y hay
 * que seguirla en orden porque la tarjeta es una maquina de estados:
 *
 *   CMD0    "reiniciate"            -> estado idle
 *   CMD8    "¿aguantas 2.7-3.6 V?"  -> distingue las tarjetas modernas
 *   ACMD41  "enciendete"            -> se repite hasta que dice que si
 *   CMD2    "dime quien eres"       -> numero de serie (CID)
 *   CMD3    "toma una direccion"    -> la RCA, para hablarle solo a ella
 *   CMD7    "te elijo a ti"         -> pasa a estado transfer
 *   CMD16   "bloques de 512"        -> por si acaso
 *
 * Y ademas el reloj tiene que subir por etapas: la identificacion va a
 * 400 kHz porque es lo unico que toda tarjeta garantiza entender, y solo
 * despues se sube a velocidad de trabajo.
 */
#include "sd.h"
#include "syscall.h"

/* --- Registros, en palabras de 32 bits desde la base ------------------ */
#define ARG2          (0x00 / 4)
#define BLKSIZECNT    (0x04 / 4)
#define ARG1          (0x08 / 4)
#define CMDTM         (0x0C / 4)
#define RESP0         (0x10 / 4)
#define RESP1         (0x14 / 4)
#define RESP2         (0x18 / 4)
#define RESP3         (0x1C / 4)
#define DATA          (0x20 / 4)
#define STATUS        (0x24 / 4)
#define CONTROL0      (0x28 / 4)
#define CONTROL1      (0x2C / 4)
#define INTERRUPT     (0x30 / 4)
#define IRPT_MASK     (0x34 / 4)
#define IRPT_EN       (0x38 / 4)
#define SLOTISR_VER   (0xFC / 4)

/* STATUS */
#define SR_CMD_INHIBIT   (1u << 0)
#define SR_DAT_INHIBIT   (1u << 1)

/* INTERRUPT: aqui no se usan como interrupciones sino como banderas que
 * se consultan. Escribir un 1 las borra. */
#define INT_CMD_DONE     (1u << 0)
#define INT_DATA_DONE    (1u << 1)
#define INT_WRITE_RDY    (1u << 4)
#define INT_READ_RDY     (1u << 5)
#define INT_ERROR_MASK   0xFFFF0000u

/* CONTROL1 */
#define C1_CLK_INTLEN    (1u << 0)
#define C1_CLK_STABLE    (1u << 1)
#define C1_CLK_EN        (1u << 2)
#define C1_TOUNIT_MAX    (0xEu << 16)
#define C1_SRST_HC       (1u << 24)

/* CMDTM: como se codifica una orden */
#define CMD_INDEX(n)     ((uint32_t)(n) << 24)
#define CMD_ISDATA       (1u << 21)
#define CMD_IXCHK_EN     (1u << 20)
#define CMD_CRCCHK_EN    (1u << 19)
#define CMD_RESP_NONE    (0u << 16)
#define CMD_RESP_136     (1u << 16)
#define CMD_RESP_48      (2u << 16)
#define CMD_RESP_48BUSY  (3u << 16)
#define TM_DAT_CARD2HOST (1u << 4)
#define TM_BLKCNT_EN     (1u << 1)

/* El reloj base del EMMC. NO se puede dar por supuesto: depende de como
 * este configurada la placa, y de el sale el divisor de todo lo demas. Se
 * lo preguntamos al kernel, que lo saca del buzon de la GPU. El valor de
 * reserva es el tipico de una Pi 3, por si la GPU no contesta. */
#define BASE_POR_DEFECTO 41666666u

#define C1_SRST_CMD      (1u << 25)

static volatile uint32_t *emmc;
static uint32_t           rca;          /* la direccion que nos dio CMD3  */
static int                sdhc;         /* 1 si direcciona por bloques    */
static uint32_t           ultima_orden; /* la que estamos mandando        */
static uint32_t           base_clock;   /* Hz del reloj base del EMMC     */
static uint32_t           div_actual;   /* el ultimo divisor programado   */

uint32_t sd_base_clock(void) { return base_clock; }
uint32_t sd_sd_clock(void)   { return div_actual ? base_clock / (2 * div_actual) : 0; }

/* El error no se cuenta con una frase, se cuenta con numeros: que orden
 * fallo y que decian los registros en ese momento. Una frase bonita no
 * sirve para depurar un protocolo. */
static char errbuf[96];

const char *sd_last_error(void) { return errbuf; }

static char *pon(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

static char *pon_hex(char *p, uint32_t v)
{
    *p++ = '0'; *p++ = 'x';
    for (int i = 28; i >= 0; i -= 4) {
        uint32_t d = (v >> i) & 0xF;
        *p++ = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    return p;
}

static char *pon_dec(char *p, uint32_t v)
{
    char t[12]; int n = 0;
    if (!v) { *p++ = '0'; return p; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) *p++ = t[n];
    return p;
}

static void anotar(const char *que, uint32_t irpt, uint32_t status)
{
    char *p = errbuf;
    p = pon(p, que);
    p = pon(p, " en CMD");
    p = pon_dec(p, ultima_orden);
    p = pon(p, "  INT=");
    p = pon_hex(p, irpt);
    p = pon(p, " STA=");
    p = pon_hex(p, status);
    *p = 0;
}

/* Espera tosca. No hay temporizador aqui: sleep() va en ticks de 10 ms, y
 * para los microsegundos que pide el protocolo eso es una eternidad. */
static void spin(int n)
{
    for (volatile int i = 0; i < n; i++) { }
}

/* Esperar a que una bandera de INTERRUPT se encienda. Devuelve 0 si llego,
 * -1 si se agoto la paciencia o si la tarjeta reporto un error. */
static int wait_flag(uint32_t flag, int vueltas)
{
    while (vueltas--) {
        uint32_t i = emmc[INTERRUPT];
        if (i & INT_ERROR_MASK) {
            anotar("error", i, emmc[STATUS]);
            emmc[INTERRUPT] = i;         /* borrar y rendirse */
            return -1;
        }
        if (i & flag) {
            emmc[INTERRUPT] = flag;      /* consumir solo esa */
            return 0;
        }
        spin(50);
    }
    anotar("sin respuesta", emmc[INTERRUPT], emmc[STATUS]);
    return -1;
}

static int wait_idle(int vueltas)
{
    while (vueltas--) {
        if (!(emmc[STATUS] & (SR_CMD_INHIBIT | SR_DAT_INHIBIT)))
            return 0;
        spin(50);
    }
    anotar("ocupado", emmc[INTERRUPT], emmc[STATUS]);
    return -1;
}

/* Despues de que una orden falle hay que reiniciar la linea de mandos.
 * Lo dice el manual de SDHCI y yo me lo habia saltado: sin esto, el
 * controlador se queda atascado en el error y TODAS las ordenes
 * siguientes fallan, aunque la culpa fuera solo de la primera. Y eso
 * convierte un fallo aislado en un fallo total, que es justo lo que
 * despista al depurar. */
static void reset_cmd_line(void)
{
    emmc[CONTROL1] |= C1_SRST_CMD;
    for (int i = 0; i < 100000; i++) {
        if (!(emmc[CONTROL1] & C1_SRST_CMD)) return;
        spin(50);
    }
}

/* Mandar una orden y esperar a que la tarjeta la acuse. */
static int cmd(uint32_t code, uint32_t arg)
{
    ultima_orden = (code >> 24) & 0x3F;

    if (wait_idle(100000) < 0) { reset_cmd_line(); return -1; }

    emmc[INTERRUPT] = emmc[INTERRUPT];   /* limpiar lo de antes */
    emmc[ARG1]      = arg;
    emmc[CMDTM]     = code;

    if (wait_flag(INT_CMD_DONE, 100000) < 0) { reset_cmd_line(); return -1; }
    return 0;
}

/* Las ACMD son ordenes de aplicacion: van precedidas de un CMD55 que dice
 * "la siguiente no es del estandar basico". */
static int acmd(uint32_t code, uint32_t arg)
{
    uint32_t rc = rca ? (rca << 16) : 0;
    if (cmd(CMD_INDEX(55) | CMD_RESP_48 | CMD_CRCCHK_EN, rc) < 0) return -1;
    return cmd(code, arg);
}

/* El reloj de la tarjeta = BASE_CLOCK / (2 * divisor). El divisor son 10
 * bits repartidos de forma rara entre dos campos de CONTROL1. */
/* La version del controlador decide como se codifica el divisor del reloj,
 * asi que conviene mirarla y decirla en voz alta. */
uint32_t sd_host_version(void)
{
    return (emmc[SLOTISR_VER] >> 16) & 0xFF;
}

static int set_clock(uint32_t hz)
{
    /* Redondeando HACIA ARRIBA, y esto no es una manía: una division entera
     * que trunca da un divisor mas pequeño, y un divisor mas pequeño es un
     * reloj MAS RAPIDO del que se ha pedido. Pedir 25 MHz y sacar 31 es lo
     * que hace que una tarjeta deje de contestar. */
    uint32_t d = (base_clock + 2 * hz - 1) / (2 * hz);
    if (d == 0) d = 1;
    if (d > 0x3FF) d = 0x3FF;
    div_actual = d;

    if (wait_idle(100000) < 0) return -1;

    emmc[CONTROL1] &= ~C1_CLK_EN;
    spin(2000);

    emmc[CONTROL1] = (emmc[CONTROL1] & 0xFFFF003F)
                   | ((d & 0xFF) << 8)          /* bits bajos  */
                   | ((d & 0x300) >> 2);        /* bits altos  */
    spin(2000);

    emmc[CONTROL1] |= C1_CLK_EN;

    for (int i = 0; i < 100000; i++) {
        if (emmc[CONTROL1] & C1_CLK_STABLE) return 0;
        spin(50);
    }
    anotar("el reloj no se estabiliza", emmc[CONTROL1], emmc[STATUS]);
    return -1;
}

int sd_init(volatile uint32_t *base)
{
    emmc = base;
    rca  = 0;
    sdhc = 0;

    base_clock = (uint32_t)clock_rate(CLK_EMMC);
    if (base_clock < 1000000 || base_clock > 1000000000)
        base_clock = BASE_POR_DEFECTO;

    /* 1. Reiniciar el controlador entero y esperar a que se recomponga. */
    emmc[CONTROL0] = 0;
    emmc[CONTROL1] |= C1_SRST_HC;
    for (int i = 0; ; i++) {
        if (!(emmc[CONTROL1] & C1_SRST_HC)) break;
        if (i > 100000) {
            anotar("no se reinicia", emmc[CONTROL1], emmc[STATUS]);
            return -1;
        }
        spin(50);
    }

    /* 2. Reloj interno y tiempo maximo de espera de datos. */
    emmc[CONTROL1] |= C1_CLK_INTLEN | C1_TOUNIT_MAX;
    spin(2000);

    /* 3. A 400 kHz, que es lo unico que toda tarjeta entiende de entrada. */
    if (set_clock(400000) < 0) return -1;

    /* 4. Que las banderas se enciendan aunque no haya interrupciones
     *    cableadas: las consultamos a mano. */
    emmc[IRPT_EN]   = 0xFFFFFFFF;
    emmc[IRPT_MASK] = 0xFFFFFFFF;

    /* 5. CMD0: a todas las tarjetas, "vuelve al principio". */
    if (cmd(CMD_INDEX(0) | CMD_RESP_NONE, 0) < 0) return -1;

    /* 6. CMD8: pregunta de voltaje que solo entienden las tarjetas de la
     *    version 2 en adelante. El 0x1AA es "3.3 V" mas un patron que la
     *    tarjeta tiene que devolver tal cual, para comprobar que nos
     *    entiende de verdad y no esta contestando por contestar. */
    int v2 = (cmd(CMD_INDEX(8) | CMD_RESP_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN,
                  0x1AA) == 0)
             && ((emmc[RESP0] & 0xFFF) == 0x1AA);

    /* 7. ACMD41: "enciendete". Se repite porque encender la tarjeta lleva
     *    su tiempo y ella avisa cuando ha terminado, poniendo el bit 31.
     *    HCS (bit 30) le dice que nosotros entendemos las de alta
     *    capacidad; si no se lo dijeramos, una de 32 GB se haria pasar por
     *    una pequeña. */
    uint32_t ocr_arg = 0x00FF8000u | (v2 ? (1u << 30) : 0);
    uint32_t resp    = 0;
    for (int i = 0; i < 1000; i++) {
        if (acmd(CMD_INDEX(41) | CMD_RESP_48, ocr_arg) < 0) return -1;
        resp = emmc[RESP0];
        if (resp & (1u << 31)) break;    /* lista */
        sleep(1);                        /* 10 ms y volvemos a preguntar */
    }
    if (!(resp & (1u << 31))) {
        anotar("no termina de encenderse (OCR)", resp, emmc[STATUS]);
        return -1;
    }

    /* Bit 30 de la respuesta: direcciona por BLOQUES, no por bytes. Es la
     * diferencia entre una SDHC y una de las antiguas, y se nota en el
     * argumento de cada lectura. */
    sdhc = (resp & (1u << 30)) != 0;

    /* 8. CMD2 y CMD3: quien eres y toma tu direccion. A partir de la RCA
     *    se le puede hablar a ella sola, que importa cuando hay varias. */
    if (cmd(CMD_INDEX(2) | CMD_RESP_136 | CMD_CRCCHK_EN, 0) < 0) return -1;
    if (cmd(CMD_INDEX(3) | CMD_RESP_48  | CMD_CRCCHK_EN, 0) < 0) return -1;
    rca = (emmc[RESP0] >> 16) & 0xFFFF;

    /* 9. Ya se le puede hablar rapido. */
    if (set_clock(25000000) < 0) return -1;

    /* 10. CMD7: seleccionarla. Pasa a estado "transfer" y acepta lecturas. */
    if (cmd(CMD_INDEX(7) | CMD_RESP_48BUSY | CMD_CRCCHK_EN, rca << 16) < 0)
        return -1;

    /* 11. Bloques de 512. Una SDHC ya los tiene fijos, pero decirlo no
     *     estorba y una tarjeta antigua lo necesita. */
    if (cmd(CMD_INDEX(16) | CMD_RESP_48 | CMD_CRCCHK_EN, 512) < 0) return -1;

    errbuf[0] = 0;
    return 0;
}

int sd_read_block(uint64_t lba, void *dst)
{
    uint32_t *out = dst;

    if (!emmc) {
        anotar("sin inicializar", 0, 0);
        return -1;
    }
    if (wait_idle(100000) < 0) return -1;

    /* Una tarjeta de alta capacidad se direcciona por bloques; una antigua
     * por bytes. Es el unico sitio donde se nota la diferencia. */
    uint32_t arg = sdhc ? (uint32_t)lba : (uint32_t)(lba * 512);

    emmc[BLKSIZECNT] = (1u << 16) | 512;    /* un bloque de 512 bytes */

    if (cmd(CMD_INDEX(17) | CMD_RESP_48 | CMD_CRCCHK_EN
                          | CMD_ISDATA | TM_DAT_CARD2HOST | TM_BLKCNT_EN,
            arg) < 0)
        return -1;

    if (wait_flag(INT_READ_RDY, 100000) < 0) return -1;

    /* Sin DMA: los 512 bytes se sacan de cuatro en cuatro por el registro
     * de datos. Es lento y es facil de entender, que aqui importa mas. */
    for (int i = 0; i < 128; i++)
        out[i] = emmc[DATA];

    return wait_flag(INT_DATA_DONE, 100000);
}

int sd_write_block(uint64_t lba, const void *src)
{
    const uint32_t *in = src;

    if (!emmc) {
        anotar("sin inicializar", 0, 0);
        return -1;
    }
    if (wait_idle(100000) < 0) return -1;

    uint32_t arg = sdhc ? (uint32_t)lba : (uint32_t)(lba * 512);

    emmc[BLKSIZECNT] = (1u << 16) | 512;

    /* CMD24 = escribir un bloque. La unica diferencia con la lectura, en
     * la orden, es que NO se pone TM_DAT_CARD2HOST: el sentido de los
     * datos lo marca ese bit y nada mas. */
    if (cmd(CMD_INDEX(24) | CMD_RESP_48 | CMD_CRCCHK_EN
                          | CMD_ISDATA | TM_BLKCNT_EN,
            arg) < 0)
        return -1;

    /* Y aqui la tarjeta avisa cuando esta lista para RECIBIR, no cuando
     * tiene algo que dar. */
    if (wait_flag(INT_WRITE_RDY, 100000) < 0) return -1;

    for (int i = 0; i < 128; i++)
        emmc[DATA] = in[i];

    /* DATA_DONE aqui significa algo mas serio que en una lectura: la
     * tarjeta ha terminado de grabar de verdad. Puede tardar, porque por
     * dentro es memoria flash y tiene que borrar antes de escribir. */
    return wait_flag(INT_DATA_DONE, 1000000);
}
