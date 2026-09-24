/* user/usb.c - La semilla del driver de USB
 *
 * Todavia no conduce nada. Lo que hace es comprobar las dos cosas que el
 * paso 58 acaba de anyadir al kernel, porque son las dos que hacian
 * imposible escribir un driver de USB en EL0:
 *
 *   1. Que se pueda reclamar una interrupcion que no sea la de la UART.
 *   2. Que un driver pueda pedir memoria para DMA y saber donde esta en
 *      memoria fisica.
 *
 * Y de propina, ya que tiene el MMIO del controlador mapeado, lee sus
 * registros de identidad. Eso no es conducir nada -no se escribe ni un bit-
 * pero es la unica forma de saber si la ventana da al sitio correcto, y en
 * la Pi de verdad es la diferencia entre "creo que esta bien" y "el chip ha
 * contestado".
 *
 * Por que el USB y no otro periferico: en la Pi 3B, el DWC2 lleva detras un
 * LAN9514, que es a la vez el hub de los cuatro conectores y la tarjeta de
 * red. Sin USB no hay red ni almacenamiento externo, y hasta la Ethernet
 * esta detras del hub interno.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "syscall.h"
#include "fs_abi.h"
#include "blk_abi.h"

/* --- Registros del DWC2 ----------------------------------------------- */
#define PCGCCTL    0xE00    /* puertas de reloj y pinza de alimentacion      */
#define GOTGCTL    0x000
#define GAHBCFG    0x008    /* DMA, rafagas, interrupcion global             */
#define GUSBCFG    0x00C    /* que PHY y en que modo                         */
#define GRSTCTL    0x010    /* reset del nucleo y de las FIFO                */
#define GINTSTS    0x014
#define GINTMSK    0x018
#define GSNPSID    0x040    /* "quien soy": 0x4F54xxxx, o sea "OT" + version */
#define GHWCFG1    0x044
#define GHWCFG2    0x048    /* arquitectura y numero de canales              */
#define GHWCFG3    0x04C
#define GHWCFG4    0x050
#define GRXFSIZ    0x024    /* tamanyo de la FIFO de recepcion               */
#define GNPTXFSIZ  0x028    /* ...y de la de transmision no periodica        */
#define HPTXFSIZ   0x100    /* ...y de la periodica                          */
#define HAINT      0x414    /* que canales han avisado                       */
#define HAINTMSK   0x418
#define HCFG       0x400    /* configuracion de anfitrion                    */
#define HCFG_FSLSS (1u << 2) /* "solo velocidades completa y baja"            */
#define HFIR       0x404    /* cada cuantos relojes empieza una trama        */
#define HFNUM      0x408    /* numero de micro-trama: el reloj del USB        */
#define HPRT0      0x440    /* EL puerto raiz                                */

/* GRSTCTL */
#define RST_CSFTRST     (1u << 0)     /* reset del nucleo entero            */
#define RST_AHBIDLE     (1u << 31)    /* el bus esta quieto                 */

/* GAHBCFG */
#define AHB_GLBLINTR    (1u << 0)     /* deja salir interrupciones           */
#define AHB_HBSTLEN(n)  ((uint32_t)(n) << 1)
#define AHB_DMAEN       (1u << 5)

/* GUSBCFG */
#define USB_PHYIF16     (1u << 3)     /* el UTMI+ es de 16 bits              */
#define USB_ULPI_SEL    (1u << 4)     /* 0 = UTMI+, 1 = ULPI                 */
#define USB_PHYSEL_FS   (1u << 6)     /* 1 = PHY serie de velocidad completa */
#define USB_SRPCAP      (1u << 8)
#define USB_HNPCAP      (1u << 9)
#define USB_EXT_VBUS    (1u << 20)
#define USB_TS_DLINE    (1u << 22)
#define USB_FORCEHOST   (1u << 29)
#define USB_FORCEDEV    (1u << 30)

/* --- HPRT0, y el registro mas traicionero de este chip -----------------
 *
 * Tiene tres clases de bit mezcladas:
 *
 *   - de solo lectura: si hay algo conectado, a que velocidad, el estado de
 *     las lineas;
 *   - de los que se BORRAN escribiendo un uno (write-1-to-clear): los avisos
 *     de "ha cambiado algo";
 *   - y PRTENA, que es de lectura para saber si el puerto esta habilitado y
 *     de escritura para DESHABILITARLO.
 *
 * O sea que un read-modify-write ingenuo sobre este registro -leer, poner un
 * bit, escribir de vuelta- apaga el puerto y de paso borra los avisos que
 * ibas a leer. Es un clasico de este chip y por eso toda escritura pasa por
 * hprt_escribir(), que quita esos bits antes de escribir. */
#define HPRT_CONNSTS    (1u << 0)     /* hay algo conectado        (RO)     */
#define HPRT_CONNDET    (1u << 1)     /* ...y acaba de cambiar     (W1C)    */
#define HPRT_ENA        (1u << 2)     /* habilitado (RO) / escribir 1 APAGA */
#define HPRT_ENCHNG     (1u << 3)     /*                           (W1C)    */
#define HPRT_OVRCUR     (1u << 4)     /* sobrecorriente            (RO)     */
#define HPRT_OVRCURCHNG (1u << 5)     /*                           (W1C)    */
#define HPRT_RES        (1u << 6)
#define HPRT_SUSP       (1u << 7)
#define HPRT_RST        (1u << 8)     /* mantener para resetear el puerto   */
#define HPRT_PWR        (1u << 12)    /* alimentacion del puerto            */
#define HPRT_SPD(v)     (((v) >> 17) & 3)   /* 0=alta 1=completa 2=baja     */

#define HPRT_W1C  (HPRT_CONNDET | HPRT_ENA | HPRT_ENCHNG | HPRT_OVRCURCHNG)

/* --- Los canales, que son las transferencias en vuelo -----------------
 * Ocho, segun dijo GHWCFG2. Cada uno con su bloque de seis registros. */
#define HCCHAR(n)    (0x500 + 0x20 * (n))
#define HCSPLT(n)    (0x504 + 0x20 * (n))
#define HCINT(n)     (0x508 + 0x20 * (n))
#define HCINTMSK(n)  (0x50C + 0x20 * (n))
#define HCTSIZ(n)    (0x510 + 0x20 * (n))
#define HCDMA(n)     (0x514 + 0x20 * (n))

/* HCCHAR */
#define HCC_MPS(n)     ((uint32_t)(n) & 0x7FF)
#define HCC_EP(n)      ((uint32_t)((n) & 0xF) << 11)
#define HCC_IN         (1u << 15)
#define HCC_LOWSPEED   (1u << 17)
#define HCC_TIPO(n)    ((uint32_t)((n) & 3) << 18)
/* --- El campo que me faltaba -------------------------------------------
 *
 * Bits 21:20. En una transferencia periodica es "cuantos paquetes por trama";
 * en una no periodica -un control, un bulk- es el numero de transacciones que
 * el canal tiene que hacer, o sea cuantas veces reintentar. Y tiene que ser
 * AL MENOS UNA.
 *
 * Yo no lo ponia, asi que valia cero, y cero no es "el valor por defecto": es
 * "no hagas ninguna transaccion". El nucleo leia por DMA los ocho bytes del
 * SETUP -eso se veia, porque HCDMA avanzaba a 0x...008- y se quedaba con ellos
 * en la FIFO sin nada que hacer. Ni los transmitia ni daba error: no habia
 * nada que transmitir.
 *
 * En QEMU funcionaba igual, que a estas alturas ya no sorprende: su modelo no
 * cuenta transacciones. */
#define HCC_MC(n)      ((uint32_t)((n) & 3) << 20)
#define HCC_ADDR(n)    ((uint32_t)((n) & 0x7F) << 22)
#define HCC_ODDFRM     (1u << 29)
#define HCC_DISABLE    (1u << 30)
#define HCC_ENABLE     (1u << 31)

#define EP_CONTROL     0
#define EP_ISO         1
#define EP_BULK        2
#define EP_INT         3

/* HCTSIZ */
#define HCT_BYTES(n)   ((uint32_t)(n) & 0x7FFFF)
#define HCT_PAQUETES(n) ((uint32_t)((n) & 0x3FF) << 19)
#define HCT_PID(n)     ((uint32_t)((n) & 3) << 29)

#define PID_DATA0      0
#define PID_DATA2      1
#define PID_DATA1      2
#define PID_SETUP      3

/* HCINT: por que acabo una transferencia */
#define HCI_XFERCOMPL  (1u << 0)
#define HCI_CHHLTD     (1u << 1)
#define HCI_AHBERR     (1u << 2)
#define HCI_STALL      (1u << 3)
#define HCI_NAK        (1u << 4)
#define HCI_ACK        (1u << 5)
#define HCI_NYET       (1u << 6)
#define HCI_XACTERR    (1u << 7)
#define HCI_BBLERR     (1u << 8)
#define HCI_FRMOVRUN   (1u << 9)
#define HCI_DATATGLERR (1u << 10)

#define HCI_MALO  (HCI_AHBERR | HCI_STALL | HCI_XACTERR | HCI_BBLERR | \
                   HCI_FRMOVRUN | HCI_DATATGLERR)

/* --- HCSPLT: hablar con lo lento a traves de lo rapido ---------------------
 *
 * Un dispositivo de velocidad completa o baja detras de un hub de alta no
 * puede hablar directamente con el anfitrion: el cable entre los dos va a 480
 * Mbit/s y el dispositivo no lo entiende. Lo que hace el USB 2.0 es que el
 * hub TRADUZCA: el anfitrion le manda al hub la transaccion en alta velocidad
 * ("start split"), el hub la hace por su cuenta en velocidad completa con el
 * dispositivo, y el anfitrion vuelve mas tarde a por el resultado ("complete
 * split"). Si vuelve demasiado pronto el hub contesta NYET -"todavia no"- y
 * hay que volver a preguntar.
 *
 * El DWC2 hace la mecanica de cada mitad por hardware; lo que pone el
 * software es a quien -direccion del hub y numero de puerto- y en que mitad
 * esta. Bits de Linux, drivers/usb/dwc2/hw.h. */
#define SPLT_ENA        (1u << 31)
#define SPLT_COMPLETE   (1u << 16)
#define SPLT_XACT_ALL   (3u << 14)    /* la transaccion entera, no un trozo */
#define SPLT_HUB(a)     ((uint32_t)((a) & 0x7F) << 7)
#define SPLT_PUERTO(p)  ((uint32_t)((p) & 0x7F))

/* El contexto de particion del dispositivo con el que se esta hablando. Cero
 * mientras se habla con algo de alta velocidad, o directamente con el raiz. */
static int split_activo;      /* 1 = las transferencias van partidas */
static int split_hub;         /* direccion del hub que traduce */
static int split_puerto;      /* y su puerto */
static int dispositivo_baja;  /* 1 = el dispositivo es de BAJA velocidad */

/* --- Y la direccion que hay que darle al chip, que NO es la fisica -----
 *
 * Esto es de las cosas que fallan en silencio y cuestan un dia.
 *
 * El paso 58 consiguio memoria contigua y su direccion FISICA, que es lo que
 * un periferico necesita porque no pasa por la MMU. Pero en esta placa la CPU
 * y los perifericos no ven la RAM en el mismo sitio: lo que para el ARM es la
 * direccion 0 es, para el bus de la GPU y para los maestros DMA que cuelgan
 * de el, la 0xC0000000.
 *
 * Hay cuatro alias del mismo byte de RAM -0x00000000, 0x40000000, 0x80000000 y
 * 0xC0000000- y se distinguen en como pasan por las caches de la VideoCore. El
 * que se usa para DMA es el 0xC0000000, que es coherente con la L2.
 *
 * Darle al DWC2 la direccion fisica a secas no da un error: da un DMA que
 * escribe en otro sitio, y un buffer que sigue teniendo lo que tenia. Por eso
 * el buffer se rellena con un patron antes de cada transferencia: si el
 * descriptor aparece, la direccion era buena; si el patron sigue intacto, el
 * chip ha escrito en otra parte. Sin ese patron, las dos cosas se ven igual. */
#define BUS(pa)   ((uint32_t)((uint64_t)(pa) | 0xC0000000UL))

/* El hub del LAN9514 es siempre la direccion 1: es lo primero que se enumera. */
#define DIR_HUB   1

/* Y este chip quiere la DE BUS. Esta linea ha cambiado de valor dos veces, y
 * las dos veces por un razonamiento mio que parecia un experimento y no lo era.
 *
 * Primero puse el alias 0xC0000000, que es lo que hacen Linux y Circle. Luego,
 * con otros tres fallos vivos a la vez -MC a cero, el reloj del PHY mal, el
 * dominio de alimentacion a medias-, vi que con la fisica a secas salia un
 * XACTERR y con el alias no salia nada, y "conclui" que este chip queria la
 * fisica. La comparacion estaba confundida por los otros tres fallos: no media
 * la direccion, media el ruido.
 *
 * Lo que de verdad lo zanja es esto: con la fisica a secas, el hub recibe el
 * SETUP tal como estaba ANTES de que la CPU lo escribiera. En el buffer
 * reescrito recibia el paquete viejo (wLength 8) y mandaba 8 bytes; en un
 * buffer virgen recibia ceros y contestaba STALL. Y la CPU, releyendo la RAM,
 * veia el paquete correcto. Entre la RAM y el chip solo hay una cosa que pueda
 * guardar una copia vieja: la L2 de la VideoCore.
 *
 * En el BCM283x, la RAM se ve desde el bus de la GPU por cuatro alias, y se
 * distinguen en si pasan por esa L2. El alias 0x00000000 -que es la fisica del
 * ARM tal cual- pasa por ella: la primera lectura del DWC2 mete la linea en la
 * L2 con el contenido de ese momento, la CPU escribe la RAM sin que la L2 se
 * entere, y la siguiente lectura del DWC2 acierta en la L2 y trae lo viejo. El
 * alias 0xC0000000 no pasa por ella. Por eso Linux declara "dma-ranges" con
 * 0xC0000000 para este SoC y Circle envuelve toda direccion de DMA en
 * BUS_ADDRESS(): no es un capricho de numeracion, es coherencia de cache.
 *
 * La leccion, dos veces aprendida: un experimento con tres variables sueltas
 * no mide ninguna. */
static int dma_bus = 1;

/* Lo que quedo en el canal al detenerse la ultima transferencia. */
static uint32_t ultimo_hctsiz, ultimo_hcdma;
static int      detallado;           /* 1 = contar cada transferencia */

static volatile uint32_t *reg;

/* --- La barrera, o el orden en que las cosas llegan a la memoria ---------
 *
 * El paquete de SETUP lo escribe la CPU en el tramo de DMA, que es memoria
 * Normal sin cachear. El arranque del canal es una escritura en un registro,
 * que es memoria Device. Y ARMv8 no promete que un almacenamiento Normal se
 * haga visible antes que uno Device posterior: la CPU puede tener el paquete
 * todavia en su buffer de escritura cuando el DWC2 ya ha recibido la orden de
 * arrancar y va a leerlo por DMA.
 *
 * Lo que leia entonces era el SETUP de la transferencia ANTERIOR, con su
 * wLength de 8, y el hub -obediente- mandaba 8 bytes cuando se le pedian 18.
 * Los otros diez se quedaban con el patron: a5a5:a5a5, 165 configuraciones.
 *
 * En QEMU no se ve nunca, porque su memoria es secuencialmente consistente.
 * El kernel lo tiene bien en mbox.c, con su dsb antes de escribir al buzon.
 * DSB se puede ejecutar desde EL0, asi que aqui tambien. */
static inline void barrera(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uint32_t leer(uint64_t off) { return reg[off / 4]; }
static void escribir(uint64_t off, uint32_t v) { reg[off / 4] = v; }

/* Escribir HPRT0 sin pisarse los pies. Ver el comentario de arriba. */
static void hprt_escribir(uint32_t v) { escribir(HPRT0, v & ~HPRT_W1C); }

/* Y reconocer los avisos, que es lo que faltaba.
 *
 * hprt_escribir quita los bits que se borran escribiendo un uno, para que un
 * leer-poner-escribir no los borre por accidente. Con eso solo, resulta que
 * tampoco se pueden borrar A PROPOSITO: quedan puestos para siempre y el
 * driver no puede enterarse de un segundo cambio. Se vio en la placa, en un
 * HPRT0 que acababa en 'f' con las dos banderas de cambio encendidas.
 *
 * Asi que hacen falta las dos funciones. Una protege y la otra reconoce, y lo
 * que no puede haber es solo la primera. */
static void hprt_reconocer(uint32_t bits)
{
    uint32_t v = leer(HPRT0);
    escribir(HPRT0, (v & ~HPRT_W1C) | (bits & HPRT_W1C & ~HPRT_ENA));
}

/* Esperar a que un bit se ponga (o se quite), con tope.
 *
 * Dos fases, y la razon es que aqui se juntan dos escalas de tiempo que no se
 * parecen: el bus AHB contesta en MICROsegundos y un reset de puerto USB tarda
 * decenas de MILIsegundos. Con el reloj de este sistema un tick son 10 ms, o
 * sea mil veces mas de lo que tarda lo primero.
 *
 * Asi que primero se mira a pelo unas cuantas veces -que resuelve todo lo
 * rapido sin dormir a nadie- y solo si eso no basta se pasa a dormir por
 * ticks. Es en pequenyo el problema que tendria un planificador de
 * micro-tramas, y parte de la razon de que ese no pueda vivir aqui.
 *
 * Y con tope: un bucle sin tope esperando a un registro de hardware es la
 * forma mas comoda de colgar un driver. Si el chip no contesta, no contesta. */
static int esperar_bit(uint64_t off, uint32_t bit, int puesto, int ticks)
{
    for (int v = 0; v < 200000; v++)
        if (((leer(off) & bit) != 0) == puesto) return 1;

    for (int i = 0; i < ticks; i++) {
        sleep(1);
        if (((leer(off) & bit) != 0) == puesto) return 1;
    }
    return 0;
}

/* --- Despertar el nucleo ----------------------------------------------
 *
 * El firmware de la Pi deja el DWC2 encendido y a medio configurar. Heredar eso
 * es como se depura durante tres dias algo que funciona en un arranque y no en
 * el siguiente, asi que lo primero es ponerlo en un estado CONOCIDO.
 *
 * El orden importa mas de lo que parece, y aqui lo aprendi a base de que la
 * placa no viera lo que tiene soldado: la seleccion de PHY hay que escribirla
 * ANTES del reset, porque es el reset el que la hace efectiva. Yo la escribia
 * despues, y en QEMU funcionaba igual -su modelo no simula el PHY- asi que el
 * fallo solo aparecio en el hardware de verdad. */
static int nucleo_despertar(void)
{
    /* 1. Quitar las puertas de reloj y la pinza de alimentacion. Con el reloj
     *    cortado los registros contestan basura, y todo lo que venga despues
     *    seria un misterio. */
    escribir(PCGCCTL, 0);

    /* 2. Cerrar la salida de interrupciones mientras se configura. Aun no hay
     *    nadie escuchando, y una fuente abierta sin manejador es el sistema
     *    girando en el vector. */
    escribir(GAHBCFG, leer(GAHBCFG) & ~AHB_GLBLINTR);

    /* 3. Nada de VBUS externo ni pulsos en las lineas: son cosas de una placa
     *    con transceptor de fuera, y aqui el PHY esta dentro del chip. */
    escribir(GUSBCFG, leer(GUSBCFG) & ~(USB_EXT_VBUS | USB_TS_DLINE));

    /* 4. El PHY y el papel, AHORA, antes del reset.
     *
     * UTMI+ de alta velocidad: PHYSEL a cero es "no me pongas el serie de
     * velocidad completa", y sin eso la Ethernet -que es de alta velocidad- no
     * se veria nunca.
     *
     * Y el modo anfitrion a mano, aunque este chip lo deduzca del pin ID: si
     * el pin flotara, se quedaria esperando a que alguien le hable en vez de
     * hablar el. */
    uint32_t cfg = leer(GUSBCFG);
    cfg &= ~(USB_ULPI_SEL | USB_PHYSEL_FS);
    cfg &= ~(USB_SRPCAP | USB_HNPCAP);     /* nada de negociar el papel */
    cfg |=  USB_FORCEHOST;
    cfg &= ~USB_FORCEDEV;
    escribir(GUSBCFG, cfg);

    /* 5. Esperar a que el bus AHB este quieto ANTES de resetear. Resetear con
     *    una transferencia a medias deja el bus colgado, y con el medio chip. */
    if (!esperar_bit(GRSTCTL, RST_AHBIDLE, 1, 10)) return 0;

    /* 6. Y el reset, que es lo que hace efectiva la eleccion de PHY. Se pide
     *    poniendo el bit y termina cuando el propio chip lo quita. */
    escribir(GRSTCTL, RST_CSFTRST);
    if (!esperar_bit(GRSTCTL, RST_CSFTRST, 0, 20)) return 0;
    if (!esperar_bit(GRSTCTL, RST_AHBIDLE, 1, 10)) return 0;

    /* 7. Reafirmarlo. El reset no deberia borrar un registro de
     *    configuracion, pero escribirlo otra vez cuesta una instruccion y
     *    quita la duda. */
    escribir(GUSBCFG, cfg);

    /* Forzar el modo tarda: el chip tiene que mirar las lineas y decidir, y
     * hasta 25 ms lo que uno lea no es definitivo. Es de las esperas que no se
     * pueden cambiar por un bucle mirando un bit, porque no hay bit. */
    sleep(4);
    return 1;
}

/* --- Las FIFO, que no salen bien por defecto ---------------------------
 *
 * El nucleo tiene 4080 palabras de FIFO -lo dijo GHWCFG3- y hay que repartirlas
 * a mano entre recepcion, transmision no periodica y transmision periodica. Los
 * valores por defecto de este ejemplar no sirven, y los que se usan aqui son
 * los que el driver de Linux lleva escritos para esta placa concreta.
 *
 * El reparto se escribe como "profundidad y donde empieza", y las tres zonas
 * tienen que ir seguidas y sin solaparse: si dos se pisan, las transferencias
 * salen con datos de la otra y el sintoma no se parece en nada a la causa. */
#define FIFO_RX     774
#define FIFO_NPTX   256
#define FIFO_PTX    512

static void fifos_repartir(void)
{
    escribir(GRXFSIZ,   FIFO_RX);
    escribir(GNPTXFSIZ, (FIFO_NPTX << 16) | FIFO_RX);
    escribir(HPTXFSIZ,  (FIFO_PTX  << 16) | (FIFO_RX + FIFO_NPTX));

    /* Y vaciarlas, porque lo que hubiera dentro es de antes del reparto y
     * ahora esta en el sitio de otro. TXFNUM a 0x10 quiere decir "todas". */
    escribir(GRSTCTL, (0x10u << 6) | (1u << 5));    /* TXFFLSH */
    esperar_bit(GRSTCTL, (1u << 5), 0, 5);
    escribir(GRSTCTL, (1u << 4));                   /* RXFFLSH */
    esperar_bit(GRSTCTL, (1u << 4), 0, 5);
}

/* --- Una transferencia por un canal ------------------------------------
 *
 * Esto es todo lo que el DWC2 sabe hacer: le dices a quien, por que endpoint,
 * en que direccion, de que tipo, cuantos bytes y donde ponerlos, y el lo hace.
 * Una transferencia de USB de las de verdad -un control, por ejemplo- son
 * varias de estas seguidas.
 *
 * Se espera mirando, no por interrupcion. GINTMSK sigue a cero: meter las
 * interrupciones antes de que una transferencia funcione es depurar dos cosas
 * a la vez. */
static uint32_t canal_hacer(int canal, int entrada, int tipo, int mps,
                            int addr, int ep, int pid, uint64_t pa, int bytes,
                            int csplit)
{
    int paquetes = bytes ? (bytes + mps - 1) / mps : 1;

    /* Para una lectura, el tamanyo se programa como un numero ENTERO de
     * paquetes maximos, no como los bytes que se quieren. Es lo que hace Linux
     * en dwc2_hc_start_transfer -"always program an integral # of max packets
     * for IN transfers"- porque el nucleo escribe en memoria paquetes enteros y
     * decide que la transferencia acabo cuando recibe uno corto. Se piden 18
     * bytes de un dispositivo con paquetes de 64: se programa 64, el dispositivo
     * manda 18, y ese paquete corto es el final. */
    if (entrada) bytes = paquetes * mps;

    /* Los avisos viejos del canal, fuera. Un canal se reutiliza, y arrancar
     * con el ACK de la transferencia anterior puesto es creerse que esta ya
     * termino. */
    escribir(HCINTMSK(canal), 0);
    escribir(HCINT(canal), 0xFFFFFFFF);
    escribir(HAINTMSK, leer(HAINTMSK) | (1u << canal));

    /* Partida o no, y si lo es, en que mitad. Lo decide el contexto del
     * dispositivo, no quien llama: quien llama solo sabe que quiere leer un
     * descriptor. */
    if (split_activo)
        escribir(HCSPLT(canal), SPLT_ENA | SPLT_XACT_ALL | SPLT_HUB(split_hub) |
                                SPLT_PUERTO(split_puerto) |
                                (csplit ? SPLT_COMPLETE : 0));
    else
        escribir(HCSPLT(canal), 0);         /* cuelga del raiz, o es de alta */

    /* --- Las caracteristicas del canal, SIN habilitarlo todavia -----------
     *
     * Dos escrituras y no una, que es como lo hace la implementacion de
     * referencia y no como lo hacia yo. Poner las caracteristicas y el bit de
     * "arranca" en la MISMA escritura le pide al nucleo que empiece con unos
     * valores que estan llegando en ese mismo ciclo de bus. Separarlo es
     * gratis, y lo que se gana es que cuando el canal arranca todo lo demas
     * llevaba ya un rato en su sitio. */
    escribir(HCCHAR(canal), HCC_MC(1) | HCC_ADDR(addr) | HCC_TIPO(tipo) |
                            (entrada ? HCC_IN : 0) | HCC_EP(ep) |
                            HCC_MPS(mps) |
                            (dispositivo_baja ? HCC_LOWSPEED : 0));

    escribir(HCTSIZ(canal), HCT_PID(pid) | HCT_PAQUETES(paquetes) |
                            HCT_BYTES(bytes));
    escribir(HCDMA(canal), dma_bus ? BUS(pa) : (uint32_t)pa);

    /* Y la mascara del canal: SOLO "se ha detenido".
     *
     * Yo ponia 0x7FF, o sea todo, incluidos NAK y ACK. La referencia pone
     * unicamente ese bit, y tiene sentido: el canal se detiene al acabar, pase
     * lo que pase, y entonces el resto de HCINT dice POR QUE. Desenmascarar
     * NAK y ACK le pide al nucleo que reaccione a cosas que son normales a
     * mitad de una transferencia. */
    escribir(HCINTMSK(canal), HCI_CHHLTD);

    /* La paridad de la trama, que tampoco ponia. Con ella el nucleo sabe en
     * que mitad del ciclo colocar la transaccion. */
    uint32_t c = leer(HCCHAR(canal));
    if (!(leer(HFNUM) & 1)) c |= HCC_ODDFRM;

    /* Que todo lo escrito -el paquete de SETUP, el patron, los registros del
     * canal- este de verdad en su sitio ANTES de que el nucleo reciba la orden
     * de arrancar. Sin esto, el DMA lee lo de la vez anterior. */
    barrera();

    /* Y ahora arranca. */
    escribir(HCCHAR(canal), c | HCC_ENABLE);

    /* Y esperar a que el canal se DETENGA. Solo a eso.
     *
     * Aqui estaba el fallo de protocolo que corrompia el nucleo. Mi bucle
     * volvia en cuanto veia CUALQUIER bit -un XACTERR, por ejemplo- y un
     * XACTERR no es el final: es el nucleo diciendo "esta transaccion ha ido
     * mal" mientras sigue con el canal activo, reintentando las veces que diga
     * MC. Yo volvia, control_leer reintentaba, y canal_hacer reprogramaba
     * HCCHAR, HCTSIZ y HCDMA de un canal que TODAVIA ESTABA TRANSFIRIENDO.
     * Eso es comportamiento indefinido en el DWC2, y lo que la placa ensenyaba
     * despues -ChEna y ChDis a la vez, el DMA apagandose solo- es el aspecto
     * de un canal corrompido.
     *
     * El final de una transferencia es UNO: el bit de "detenido". Se detiene
     * al acabar bien y se detiene al acabar mal, y solo entonces el resto de
     * HCINT dice cual de las dos. Es lo que hacen CherryUSB y Linux, y es por
     * lo que la mascara del canal lleva ese bit y nada mas. */
    for (int v = 0; v < 2000000; v++) {
        uint32_t i = leer(HCINT(canal));
        if (i & HCI_CHHLTD) {
            /* Y la simetrica: que lo que el DMA dejo en memoria se lea DESPUES
             * de haber visto el bit de detenido, no especulado antes. */
            barrera();

            /* Lo que el chip dice que hizo, para poder compararlo con lo que
             * hay en memoria. HCTSIZ baja en lo recibido y HCDMA sube en lo
             * escrito; si los dos dicen 18 y en memoria hay 8, el problema es
             * de memoria, y si dicen 8, el problema es del hub. */
            ultimo_hctsiz = leer(HCTSIZ(canal));
            ultimo_hcdma  = leer(HCDMA(canal));
            return i;
        }
    }

    /* Nada en un par de millones de vueltas. Antes de rendirse, contar lo que
     * se ve, porque "HCINT a cero" no distingue dos cosas muy distintas: que el
     * canal nunca arrancara, o que arrancara y este esperando algo.
     *
     * Lo dice ChEna: si sigue puesto, el nucleo cogio la orden y esta a lo
     * suyo; si se bajo solo, la acabo y no lo conto. */
    uint32_t fin = leer(HCCHAR(canal));
    printf("  [usb] el canal %d no contesta: HCCHAR = 0x%08x (%s), "
           "HCTSIZ = 0x%08x\n",
           canal, (unsigned int)fin,
           (fin & HCC_ENABLE) ? "sigue habilitado" : "se deshabilito solo",
           (unsigned int)leer(HCTSIZ(canal)));
    printf("  [usb]   GINTSTS = 0x%08x, HAINT = 0x%08x, GAHBCFG = 0x%08x "
           "(DMA %s), HCDMA = 0x%08x\n",
           (unsigned int)leer(GINTSTS), (unsigned int)leer(HAINT),
           (unsigned int)leer(GAHBCFG),
           (leer(GAHBCFG) & AHB_DMAEN) ? "encendido" : "APAGADO",
           (unsigned int)leer(HCDMA(canal)));

    /* Detenerlo como manda el modo DMA: ChEna Y ChDis a la vez -Linux,
     * dwc2_hc_halt: "in DMA mode, always sets the Channel Enable and Channel
     * Disable bits"- y luego ESPERAR a que el nucleo confirme con el bit de
     * detenido. Antes ponia solo ChDis y volvia en el acto, dejando el canal
     * a medio detener para que la siguiente llamada lo reprogramara encima. */
    escribir(HCCHAR(canal), fin | HCC_ENABLE | HCC_DISABLE);
    for (int v = 0; v < 200000; v++)
        if (leer(HCINT(canal)) & HCI_CHHLTD) break;
    escribir(HCINT(canal), 0xFFFFFFFF);
    return 0;
}

/* Esperar n micro-tramas mirando el contador del propio nucleo. A alta
 * velocidad avanza cada 125 us, asi que 8 son una trama de velocidad completa:
 * el tiempo que el hub necesita para hacer por su cuenta la transaccion lenta.
 * Un sleep() aqui seria diez veces mas de lo que hace falta. */
static void esperar_microtramas(int n)
{
    uint32_t desde = leer(HFNUM) & 0x3FFF;
    for (int v = 0; v < 2000000; v++)
        if (((leer(HFNUM) - desde) & 0x3FFF) >= (uint32_t)n) return;
}

/* --- Una transferencia, partida o no ----------------------------------------
 *
 * Si el dispositivo es de alta velocidad -o cuelga del raiz- esto es una
 * llamada a canal_hacer y nada mas. Si no, es la maquina de estados de la
 * particion, que sale de leer a Linux, USPi y CherryUSB y es la misma en los
 * tres:
 *
 *   start split   -> ACK:  el hub la ha aceptado; a por el complete split
 *                 -> NAK:  el hub no tiene sitio; volver a empezar
 *   complete split-> NYET: el hub no ha terminado; esperar y repetir
 *                 -> NAK:  el DISPOSITIVO dijo que no; volver a empezar
 *                 -> XferCompl: hecho
 *
 * Y una regla que solo esta en Linux y sin la que nada de esto funciona: en
 * modo partido el nucleo mueve UN paquete por ciclo (dwc2_hc_start_transfer:
 * num_packets = 1, xfer_len = max_packet). Una lectura de 18 bytes con
 * paquetes de 8 son tres ciclos, con el PID alternando entre ellos y un
 * paquete corto marcando el final. Y el complete split de un OUT se programa
 * con CERO bytes, para que el nucleo no vuelva a pedirle datos a la FIFO. */
static int ultimo_recibido;   /* bytes que llegaron en la ultima lectura */

/* Cuantas veces insistir con un dispositivo partido que dice NAK. Para un
 * descriptor, muchas: NAK es "espera un poco". Para SONDEAR un teclado, una:
 * NAK es "no hay tecla", que es la respuesta normal el 99% de las veces, y hay
 * que volver enseguida para no tener el bus ocupado con un teclado callado. */
static int split_intentos = 40;

static uint32_t canal_transferir(int entrada, int tipo, int mps, int addr,
                                 int ep, int pid, uint64_t pa, int bytes)
{
    if (!split_activo) {
        uint32_t r = canal_hacer(0, entrada, tipo, mps, addr, ep, pid, pa, bytes, 0);
        int programado = entrada ? ((bytes + mps - 1) / mps) * mps : bytes;
        if (!bytes) programado = 0;
        ultimo_recibido = programado - (int)(ultimo_hctsiz & 0xFFFF);
        return r;
    }

    int hecho = 0;
    int quedan = bytes;
    ultimo_recibido = 0;

    /* --- Lo periodico tiene reloj, lo demas no ------------------------------
     *
     * Para control y bulk, el traductor del hub GUARDA el resultado hasta que
     * el anfitrion vuelva a por el: se puede esperar una trama entera entre
     * las dos mitades y sale bien, como salio toda la enumeracion.
     *
     * Para un endpoint de interrupcion, no. El traductor hace la transaccion
     * lenta en la trama siguiente y se queda el resultado SOLO durante esa
     * trama: si el complete split llega tarde, lo tira. Asi que aqui el
     * tiempo esta contado en micro-tramas, y la regla es la de USPi, que
     * lleva anyos leyendo teclados detras de este hub: el start split en una
     * micro-trama que no sea la 6, el primer complete split dos despues, y si
     * el hub dice NYET, otro una micro-trama mas tarde, hasta tres. Y luego
     * se da esa trama por perdida y se vuelve a empezar.
     *
     * Yo esperaba ocho micro-tramas -una trama entera- antes del primer
     * complete split. Llegaba cuando el resultado ya no existia, y el teclado
     * parecia mudo. */
    int periodico = (tipo == EP_INT || tipo == EP_ISO);

    /* Un paquete por ciclo, hasta que se acaben o llegue uno corto. Un OUT de
     * cero bytes -el estado- es un ciclo con cero. */
    do {
        int trozo = quedan < mps ? quedan : mps;
        uint32_t r = 0;
        int listo = 0;

        for (int vuelta = 0; vuelta < split_intentos && !listo; vuelta++) {
            /* Lo periodico arranca al principio de una trama. ODDFRM hace que
             * el canal salga en la micro-trama SIGUIENTE a la que se programa
             * (canal_hacer lo pone con la paridad de la siguiente), asi que se
             * espera a la 7 y el start split sale en la 0. Las dos mitades y
             * sus reintentos caben entonces en la misma trama, y nunca se
             * pisa la 6, que la norma reserva. */
            if (periodico)
                for (int v = 0; v < 400000 && (leer(HFNUM) & 7) != 7; v++) { }

            /* Start split: para un OUT lleva los datos; para un IN, el
             * tamanyo del paquete que se espera. */
            r = canal_hacer(0, entrada, tipo, mps, addr, ep, pid, pa + hecho,
                            entrada ? mps : trozo, 0);
            if (r & HCI_MALO) return r;
            if (r & HCI_NAK) { esperar_microtramas(8); continue; }
            if (!(r & HCI_ACK)) { esperar_microtramas(8); continue; }

            /* Complete split: repetir mientras el hub diga NYET.
             *
             * Periodico: el start salio en la micro-trama 0; esperar una y
             * programar pone el primer complete en la 2, y cada reintento,
             * programado nada mas volver, sale una mas tarde: 2, 3, 4. Tres y
             * se acabo; la trama se da por perdida. NAK aqui es el teclado
             * diciendo "no tengo nada", y no se insiste.
             *
             * Control y bulk: sin prisa, una trama entre intento e intento. */
            int intentos_c = periodico ? 3 : 40;
            for (int c = 0; c < intentos_c; c++) {
                if (periodico) { if (c == 0) esperar_microtramas(1); }
                else           esperar_microtramas(8);
                r = canal_hacer(0, entrada, tipo, mps, addr, ep, pid, pa + hecho,
                                entrada ? mps : 0, 1);
                if (r & HCI_MALO)      return r;
                if (r & HCI_XFERCOMPL) { listo = 1; break; }
                if (r & HCI_NAK)       break;          /* de nuevo desde el start */
                /* NYET o nada: otro intento */
            }
            if (!listo && periodico && (r & HCI_NAK)) break;   /* no hay dato */
        }
        if (!listo) return r ? r : 0;

        if (entrada) {
            int llego = mps - (int)(ultimo_hctsiz & 0xFFFF);
            ultimo_recibido += llego;
            hecho  += llego;
            quedan -= llego;
            if (llego < mps) break;                    /* paquete corto: fin */
        } else {
            hecho  += trozo;
            quedan -= trozo;
        }

        /* El siguiente paquete va con el otro PID. */
        pid = (pid == PID_DATA1) ? PID_DATA0 : PID_DATA1;
    } while (quedan > 0);

    return HCI_XFERCOMPL | HCI_CHHLTD;
}

/* Por que fallo, en palabras. Con estos nombres delante, un HCINT deja de ser
 * un numero: STALL es "el dispositivo dice que no entiende eso", XACTERR es
 * "no ha contestado o ha contestado mal", BBLERR es "ha hablado mas de lo que
 * le tocaba". Son diagnosticos distintos y llevan a sitios distintos. */
static void quejarse_canal(const char *que, uint32_t i)
{
    printf("  [usb] %s: HCINT = 0x%08x%s%s%s%s%s%s\n", que, (unsigned int)i,
           (i & HCI_STALL)      ? " STALL"      : "",
           (i & HCI_XACTERR)    ? " XACTERR"    : "",
           (i & HCI_BBLERR)     ? " BBLERR"     : "",
           (i & HCI_NAK)        ? " NAK"        : "",
           (i & HCI_AHBERR)     ? " AHBERR"     : "",
           (i & HCI_DATATGLERR) ? " DATATGLERR" : "");
}

/* --- Una transferencia de control, que son tres -------------------------
 *
 * SETUP, datos y estado. El SETUP dice que se pide, los datos van o vienen, y
 * el estado es un paquete vacio con el que el dispositivo confirma. Las tres
 * por el mismo canal y el mismo endpoint 0, que es el unico que existe antes
 * de saber nada del dispositivo.
 *
 * Los PID no son decorativos: la fase de datos de un control empieza SIEMPRE
 * en DATA1 y el estado tambien va en DATA1. Equivocarse ahi da un
 * DATATGLERR, que es el chip diciendo "esto no es el paquete que esperaba". */
/* El tramo de DMA, repartido: el paquete de SETUP al principio y los datos 64
 * bytes mas adelante. Dos zonas y no una porque el chip lee las dos en la
 * misma transferencia de control y solaparlas seria pisarse. */
static uint64_t dma_va, dma_pa;

#define OFF_SETUP   0
#define OFF_DATOS  64

#define PATRON  0xA5      /* con que se rellena para saber si el DMA llego */

static int control_leer(int addr, int mps, uint8_t tipo, uint8_t peticion,
                        uint16_t valor, uint16_t indice, int bytes)
{
    uint64_t off_setup = OFF_SETUP;
    volatile uint8_t *setup = (volatile uint8_t *)(dma_va + off_setup);
    volatile uint8_t *datos = (volatile uint8_t *)(dma_va + OFF_DATOS);

    /* Los ocho bytes de siempre, en el orden de siempre y en little-endian,
     * que es el del USB entero. */
    setup[0] = tipo;
    setup[1] = peticion;
    setup[2] = (uint8_t)(valor & 0xFF);
    setup[3] = (uint8_t)(valor >> 8);
    setup[4] = (uint8_t)(indice & 0xFF);
    setup[5] = (uint8_t)(indice >> 8);
    setup[6] = (uint8_t)(bytes & 0xFF);
    setup[7] = (uint8_t)(bytes >> 8);

    /* El patron, para poder distinguir "no ha contestado" de "ha contestado en
     * otro sitio". Ver el comentario de BUS(). */
    for (int i = 0; i < bytes; i++) datos[i] = PATRON;

    uint32_t r;

    r = canal_transferir(0, EP_CONTROL, mps, addr, 0, PID_SETUP,
                         dma_pa + off_setup, 8);
    if (!(r & HCI_XFERCOMPL)) { quejarse_canal("el SETUP no paso", r); return -1; }

    r = canal_transferir(1, EP_CONTROL, mps, addr, 0, PID_DATA1,
                         dma_pa + OFF_DATOS, bytes);
    if (!(r & HCI_XFERCOMPL)) { quejarse_canal("los datos no llegaron", r); return -1; }

    /* El testigo: lo que el chip dice que recibio, contra lo que se pidio. En
     * este ejemplar el campo de tamanyo son 16 bits (max_transfer_size 65535
     * en Linux), asi que se enmascara a eso.
     *
     * Ya no se imprime por defecto. Fue lo que destapo la L2 de la VideoCore y
     * se queda a mano -'detallado'- para el siguiente dispositivo que no
     * conteste; pero con la conversacion funcionando, son dos lineas por cada
     * peticion y la regla de esta casa es que lo normal no se anuncia. */
    if (detallado) {
        printf("  [usb]   lectura de %d (SETUP en +%lu): "
               "HCINT 0x%03x, HCTSIZ 0x%08x -> recibidos %d, HCDMA avanzo %ld\n",
               bytes, (unsigned long)off_setup, (unsigned int)r,
               (unsigned int)ultimo_hctsiz, ultimo_recibido,
               (long)(ultimo_hcdma - (dma_bus ? BUS(dma_pa + OFF_DATOS)
                                              : (uint32_t)(dma_pa + OFF_DATOS))));
        printf("  [usb]   SETUP tal como esta en memoria:");
        for (int i = 0; i < 8; i++) printf(" %02x", (unsigned)setup[i]);
        printf("\n");
    }

    r = canal_transferir(0, EP_CONTROL, mps, addr, 0, PID_DATA1,
                         dma_pa + OFF_DATOS, 0);
    if (!(r & HCI_XFERCOMPL)) { quejarse_canal("el estado no paso", r); return -1; }

    /* ¿Ha escrito alguien aqui? Si sigue todo con el patron, el DMA fue a otra
     * parte: el chip dijo que la transferencia acabo bien, y acabo bien... en
     * una direccion que no es esta. */
    int tocado = 0;
    for (int i = 0; i < bytes; i++) if (datos[i] != PATRON) { tocado = 1; break; }
    if (!tocado) {
        printf("  [usb] la transferencia dice que fue bien y el buffer sigue "
               "intacto: el DMA no escribe donde creo\n");
        return -1;
    }

    return 0;
}

/* --- Una transferencia de control sin datos ----------------------------
 *
 * SET_ADDRESS, SET_CONFIGURATION, SET_PORT_FEATURE: el SETUP lo dice todo y no
 * hay fase de datos. Lo que si hay es la de estado, y aqui va AL REVES que en
 * una lectura: es un IN de cero bytes. Es el dispositivo confirmando "hecho"
 * con un paquete vacio, y va en DATA1 como todo estado. */
static int control_escribir(int addr, int mps, uint8_t tipo, uint8_t peticion,
                            uint16_t valor, uint16_t indice)
{
    volatile uint8_t *setup = (volatile uint8_t *)(dma_va + OFF_SETUP);

    setup[0] = tipo;
    setup[1] = peticion;
    setup[2] = (uint8_t)(valor & 0xFF);
    setup[3] = (uint8_t)(valor >> 8);
    setup[4] = (uint8_t)(indice & 0xFF);
    setup[5] = (uint8_t)(indice >> 8);
    setup[6] = 0;
    setup[7] = 0;

    uint32_t r;

    r = canal_transferir(0, EP_CONTROL, mps, addr, 0, PID_SETUP,
                         dma_pa + OFF_SETUP, 8);
    if (!(r & HCI_XFERCOMPL)) { quejarse_canal("el SETUP no paso", r); return -1; }

    r = canal_transferir(1, EP_CONTROL, mps, addr, 0, PID_DATA1,
                         dma_pa + OFF_DATOS, 0);
    if (!(r & HCI_XFERCOMPL)) { quejarse_canal("el estado no paso", r); return -1; }

    return 0;
}

/* Un control OUT CON datos: SETUP, los bytes (DATA1), y el estado como un IN
 * vacio. Es lo que necesita una tarjeta de red para que se le escriba un
 * registro: la orden dice cual, y los cuatro bytes van detras. */
static int control_escribir_datos(int addr, int mps, uint8_t tipo, uint8_t peticion,
                                  uint16_t valor, uint16_t indice, int bytes)
{
    volatile uint8_t *setup = (volatile uint8_t *)(dma_va + OFF_SETUP);

    setup[0] = tipo;
    setup[1] = peticion;
    setup[2] = (uint8_t)(valor & 0xFF);
    setup[3] = (uint8_t)(valor >> 8);
    setup[4] = (uint8_t)(indice & 0xFF);
    setup[5] = (uint8_t)(indice >> 8);
    setup[6] = (uint8_t)(bytes & 0xFF);
    setup[7] = (uint8_t)(bytes >> 8);

    uint32_t r;

    r = canal_transferir(0, EP_CONTROL, mps, addr, 0, PID_SETUP,
                         dma_pa + OFF_SETUP, 8);
    if (!(r & HCI_XFERCOMPL)) { quejarse_canal("el SETUP no paso", r); return -1; }

    r = canal_transferir(0, EP_CONTROL, mps, addr, 0, PID_DATA1,
                         dma_pa + OFF_DATOS, bytes);
    if (!(r & HCI_XFERCOMPL)) { quejarse_canal("los datos no pasaron", r); return -1; }

    r = canal_transferir(1, EP_CONTROL, mps, addr, 0, PID_DATA1,
                         dma_pa + OFF_DATOS, 0);
    if (!(r & HCI_XFERCOMPL)) { quejarse_canal("el estado no paso", r); return -1; }

    return 0;
}

/* --- "Quien eres": el descriptor de dispositivo, en dos veces -------------
 *
 * Ocho bytes con paquetes de ocho, porque es lo unico que se sabe seguro de un
 * desconocido; en el byte 7 viene su tamanyo de paquete, y con ese se pide el
 * resto. Devuelve el tamanyo de paquete, o -1. */
static int num_configs = 1;          /* del ultimo descriptor de dispositivo leido */

static int presentarse(int addr, unsigned *vendedor, unsigned *producto,
                       unsigned *clase)
{
    volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_DATOS);

    if (control_leer(addr, 8, 0x80, 6, 0x0100, 0, 8) < 0) return -1;

    int mps0 = d[7];
    if (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64) {
        printf("  [usb] tamanyo de paquete %d: no es legal\n", mps0);
        return -1;
    }

    if (control_leer(addr, mps0, 0x80, 6, 0x0100, 0, 18) < 0) return -1;
    num_configs = d[17] ? d[17] : 1;

    *vendedor = (unsigned)(d[8]  | (d[9]  << 8));
    *producto = (unsigned)(d[10] | (d[11] << 8));
    *clase    = d[4];

    printf("  [usb] descriptor:");
    for (int i = 0; i < 18; i++) printf(" %02x", (unsigned)d[i]);
    printf("\n");
    printf("  [usb] direccion %d: es %04x:%04x, clase %u, USB %x.%02x, "
           "paquete maximo %d\n", addr, *vendedor, *producto, *clase,
           (unsigned)d[3], (unsigned)d[2], mps0);
    return mps0;
}

/* --- Lo que se le pide a un hub -------------------------------------------
 * Son peticiones de CLASE (bit 5 del tipo) y las de puerto van "al otro"
 * (recipient 3), con el numero de puerto en wIndex. Los numeros son los de la
 * norma y no tienen mas misterio que estar escritos en una tabla. */
#define HUB_GET_DESCRIPTOR   0xA0   /* clase, dispositivo, IN  */
#define HUB_GET_PORT_STATUS  0xA3   /* clase, puerto,      IN  */
#define HUB_SET_PORT_FEATURE 0x23   /* clase, puerto,      OUT */
#define HUB_CLR_PORT_FEATURE 0x23

#define PORT_RESET            4
#define PORT_POWER            8
#define C_PORT_CONNECTION    16
#define C_PORT_RESET         20

/* wPortStatus */
#define PS_CONNECTION   (1u << 0)
#define PS_ENABLE       (1u << 1)
#define PS_RESET        (1u << 4)
#define PS_POWER        (1u << 8)
#define PS_LOW_SPEED    (1u << 9)
#define PS_HIGH_SPEED   (1u << 10)

static int hub_estado_puerto(int addr, int mps, int puerto,
                             unsigned *estado, unsigned *cambio);

/* Resetear un puerto del hub y esperar a que salga habilitado. Deja en
 * *estado lo que dijo el hub, que es donde esta la velocidad. */
static int hub_resetear_puerto(int addr, int mps, int puerto, unsigned *estado)
{
    unsigned cam = 0;
    int listo = 0;

    control_escribir(addr, mps, HUB_CLR_PORT_FEATURE, 1, C_PORT_CONNECTION, (uint16_t)puerto);
    control_escribir(addr, mps, HUB_SET_PORT_FEATURE, 3, PORT_RESET, (uint16_t)puerto);

    for (int v = 0; v < 50 && !listo; v++) {
        sleep(1);
        if (hub_estado_puerto(addr, mps, puerto, estado, &cam) < 0) return -1;
        if (!(*estado & PS_RESET) && (*estado & PS_ENABLE)) listo = 1;
    }
    control_escribir(addr, mps, HUB_CLR_PORT_FEATURE, 1, C_PORT_RESET, (uint16_t)puerto);
    sleep(2);                          /* recuperacion tras el reset */
    return listo ? 0 : -1;
}

static int hub_estado_puerto(int addr, int mps, int puerto,
                             unsigned *estado, unsigned *cambio)
{
    volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_DATOS);
    if (control_leer(addr, mps, HUB_GET_PORT_STATUS, 0, 0, (uint16_t)puerto, 4) < 0)
        return -1;
    *estado = (unsigned)(d[0] | (d[1] << 8));
    *cambio = (unsigned)(d[2] | (d[3] << 8));
    return 0;
}

/* --- Almacenamiento masivo: un disco al otro lado de un cable ---------------
 *
 * Un pendrive no es un dispositivo USB "de discos": es un dispositivo SCSI
 * -el mismo idioma de los discos de los servidores de los anyos 90- metido
 * en un sobre USB. El sobre se llama Bulk-Only Transport, BOT, y es de una
 * simplicidad que se agradece:
 *
 *   1. un paquete de 31 bytes por el endpoint bulk OUT: el CBW, "Command
 *      Block Wrapper", que lleva dentro la orden SCSI (INQUIRY, READ...) y
 *      dice cuantos bytes van a ir o venir despues;
 *   2. los datos, si los hay, por el bulk IN o el bulk OUT;
 *   3. un paquete de 13 bytes por el bulk IN: el CSW, "Command Status
 *      Wrapper", que dice si la orden salio bien.
 *
 * Y nada mas. No hay interrupciones, ni registros, ni estado: cada orden es
 * un dialogo entero y el siguiente empieza de cero. Es lo que hace que un
 * driver de pendrive quepa en doscientas lineas, y por lo que todos los
 * pendrives del mundo funcionan con el mismo driver.
 *
 * Las ordenes SCSI que hacen falta para un disco son cuatro: INQUIRY (quien
 * eres), TEST UNIT READY (estas listo), READ CAPACITY (cuanto mides) y
 * READ/WRITE(10) (dame/toma estos sectores). Los numeros van en big-endian,
 * que era lo natural cuando SCSI se escribio. */
struct disco {
    int hay, addr, mps0, iface;
    int ep_in, ep_out, mps_in, mps_out;
    int tog_in, tog_out;                 /* el DATA0/1 de cada endpoint bulk */
    int split, puerto, baja;
    uint32_t sectores, tam_sector;
    char vendedor[9], producto[17];
};
static struct disco disco;

/* Sitios en el tramo de DMA: el CBW y el CSW aparte de los datos, para que
 * una lectura no pise la orden que la pidio. */
#define OFF_CBW      1024
#define OFF_CSW      1088
#define OFF_SECTOR   8192
#define SECTORES_MAX   16                /* 8 KB por orden, de sobra */

static uint32_t etiqueta_cbw = 1;

/* Una transferencia bulk con el disco, llevando la cuenta del toggle.
 *
 * El DATA0/DATA1 de un endpoint bulk persiste entre transferencias: el
 * siguiente paquete lleva el PID contrario al ultimo. Se cuenta por paquetes
 * y no se pregunta al chip, que en el camino partido no lo dice. Un STALL es
 * el disco diciendo "esa orden no": se limpia el endpoint con CLEAR_FEATURE
 * (ENDPOINT_HALT) y el toggle vuelve a DATA0, que es lo que manda la norma.
 * Devuelve los bytes movidos o -1. */
static int disco_bulk(int entrada, uint64_t pa, int bytes)
{
    int  ep  = entrada ? disco.ep_in   : disco.ep_out;
    int  mps = entrada ? disco.mps_in  : disco.mps_out;
    int *tog = entrada ? &disco.tog_in : &disco.tog_out;

    split_activo = disco.split; split_hub = DIR_HUB; split_puerto = disco.puerto;
    dispositivo_baja = disco.baja;
    uint32_t r = canal_transferir(entrada, EP_BULK, mps, disco.addr, ep,
                                  *tog ? PID_DATA1 : PID_DATA0, pa, bytes);
    split_activo = 0;

    if (!(r & HCI_XFERCOMPL)) {
        if (r & HCI_STALL) {
            control_escribir(disco.addr, disco.mps0, 0x02, 1, 0,
                             (uint16_t)(ep | (entrada ? 0x80 : 0)));
            *tog = 0;
        }
        return -1;
    }

    int hechos = entrada ? ultimo_recibido : bytes;
    int paquetes = hechos ? (hechos + mps - 1) / mps : 1;
    *tog ^= (paquetes & 1);
    return hechos;
}

/* Una orden SCSI entera: CBW, datos, CSW. Devuelve los bytes de datos que
 * se movieron, -1 si el transporte fallo, -2 si el disco dijo que no. */
static int disco_orden(const uint8_t *cb, int cblen, int entrada, uint64_t pa, int bytes)
{
    volatile uint8_t *cbw = (volatile uint8_t *)(dma_va + OFF_CBW);
    volatile uint8_t *csw = (volatile uint8_t *)(dma_va + OFF_CSW);
    uint32_t tag = etiqueta_cbw++;

    for (int i = 0; i < 31; i++) cbw[i] = 0;
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    cbw[4] = (uint8_t)tag;   cbw[5] = (uint8_t)(tag >> 8);
    cbw[6] = (uint8_t)(tag >> 16); cbw[7] = (uint8_t)(tag >> 24);
    cbw[8] = (uint8_t)bytes; cbw[9] = (uint8_t)(bytes >> 8);
    cbw[10] = (uint8_t)(bytes >> 16); cbw[11] = (uint8_t)(bytes >> 24);
    cbw[12] = entrada ? 0x80 : 0;
    cbw[13] = 0;                                    /* LUN 0 */
    cbw[14] = (uint8_t)cblen;
    for (int i = 0; i < cblen && i < 16; i++) cbw[15 + i] = cb[i];

    if (disco_bulk(0, dma_pa + OFF_CBW, 31) < 0) return -1;

    int hechos = 0;
    if (bytes) {
        hechos = disco_bulk(entrada, pa, bytes);
        if (hechos < 0) hechos = 0;      /* STALL en los datos: el CSW dira */
    }

    int n = disco_bulk(1, dma_pa + OFF_CSW, 13);
    if (n < 0) n = disco_bulk(1, dma_pa + OFF_CSW, 13);   /* tras limpiar el STALL */
    if (n != 13 || csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S')
        return -1;
    uint32_t tag2 = csw[4] | (csw[5] << 8) | ((uint32_t)csw[6] << 16) | ((uint32_t)csw[7] << 24);
    if (tag2 != tag) return -1;
    if (csw[12] != 0) return -2;
    return hechos;
}

static void copiar_recortado(char *dst, const volatile uint8_t *src, int n)
{
    int fin = n;
    while (fin > 0 && (src[fin - 1] == ' ' || src[fin - 1] == 0)) fin--;
    for (int i = 0; i < fin; i++) dst[i] = (char)src[i];
    dst[fin] = 0;
}

/* Quien eres (INQUIRY), estas listo (TEST UNIT READY, con REQUEST SENSE si
 * dice que no: un pendrive recien enchufado suele contestar "acabo de
 * arrancar" a la primera) y cuanto mides (READ CAPACITY). */
static int disco_arrancar(void)
{
    volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_SECTOR);

    uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (disco_orden(inquiry, 6, 1, dma_pa + OFF_SECTOR, 36) < 36) {
        printf("  [usb] el disco no contesta al INQUIRY\n");
        return -1;
    }
    copiar_recortado(disco.vendedor, d + 8, 8);
    copiar_recortado(disco.producto, d + 16, 16);
    printf("  [usb] disco: \"%s %s\", tipo SCSI %d%s\n", disco.vendedor, disco.producto,
           d[0] & 0x1F, (d[1] & 0x80) ? ", extraible" : "");

    uint8_t listo[6] = { 0x00, 0, 0, 0, 0, 0 };
    uint8_t sense[6] = { 0x03, 0, 0, 0, 18, 0 };
    int intento;
    for (intento = 0; intento < 20; intento++) {
        if (disco_orden(listo, 6, 0, 0, 0) == 0) break;
        if (disco_orden(sense, 6, 1, dma_pa + OFF_SECTOR, 18) >= 18 && detallado)
            printf("  [usb] sense: clave %d asc %02x ascq %02x\n", d[2] & 0xF, d[12], d[13]);
        sleep(10);
    }
    if (intento == 20) { printf("  [usb] el disco no se pone listo\n"); return -1; }

    uint8_t capacidad[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (disco_orden(capacidad, 10, 1, dma_pa + OFF_SECTOR, 8) < 8) {
        printf("  [usb] el disco no dice cuanto mide\n");
        return -1;
    }
    uint32_t ultimo = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | (d[2] << 8) | d[3];
    disco.tam_sector = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | (d[6] << 8) | d[7];
    disco.sectores = ultimo + 1;
    printf("  [usb] %u sectores de %u bytes: %u MB\n", disco.sectores, disco.tam_sector,
           (unsigned)(((uint64_t)disco.sectores * disco.tam_sector) >> 20));
    if (disco.tam_sector != 512) {
        printf("  [usb] sectores que no son de 512 bytes: no se leerlos\n");
        return -1;
    }
    return 0;
}

/* Leer o escribir n sectores seguidos (n <= SECTORES_MAX) en el tramo de DMA
 * de OFF_SECTOR. 0 si va bien. */
static int disco_leer(uint32_t lba, int n)
{
    uint8_t cb[10] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                       (uint8_t)(lba >> 8), (uint8_t)lba, 0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
    return disco_orden(cb, 10, 1, dma_pa + OFF_SECTOR, n * 512) == n * 512 ? 0 : -1;
}

static int disco_escribir(uint32_t lba, int n)
{
    uint8_t cb[10] = { 0x2A, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                       (uint8_t)(lba >> 8), (uint8_t)lba, 0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
    return disco_orden(cb, 10, 0, dma_pa + OFF_SECTOR, n * 512) == n * 512 ? 0 : -1;
}

/* El sector 0, que es donde un disco dice como esta repartido: o una tabla
 * de particiones (MBR, la firma 0xAA55 al final y cuatro entradas de 16
 * bytes desde el 446) o directamente un volumen FAT sin tabla, que es como
 * vienen muchos pendrives de fabrica ("superfloppy"). */
static void disco_presentar_sector0(void)
{
    volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_SECTOR);

    if (disco_leer(0, 1) < 0) { printf("  [usb] no puedo leer el sector 0\n"); return; }

    if (d[510] != 0x55 || d[511] != 0xAA) {
        printf("  [usb] sector 0 sin firma 0xAA55: %02x %02x %02x ...\n", d[0], d[1], d[2]);
        return;
    }

    /* Un BPB empieza por un salto y dice 512 bytes por sector en el 11. Si es
     * asi, el sector 0 es ya el volumen: no hay tabla. */
    if ((d[0] == 0xEB || d[0] == 0xE9) && d[11] == 0 && d[12] == 2) {
        printf("  [usb] sector 0: un volumen FAT directamente, sin tabla de particiones\n");
        return;
    }

    printf("  [usb] sector 0: tabla de particiones\n");
    for (int i = 0; i < 4; i++) {
        const volatile uint8_t *e = d + 446 + i * 16;
        if (!e[4]) continue;
        uint32_t lba = e[8] | (e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t tam = e[12] | (e[13] << 8) | ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        printf("  [usb]   particion %d: tipo 0x%02x, empieza en %u, %u sectores (%u MB)\n",
               i + 1, e[4], lba, tam, tam >> 11);
    }
}

/* Servir un sector al servidor de ficheros. Un mensaje, un sector: se lee o
 * se escribe por el DMA y se contesta al puerto que diga la peticion. */
static void disco_atender(const struct message *pet)
{
    const struct blk_request *b = (const struct blk_request *)pet->data;
    volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_SECTOR);
    static struct message resp;
    int ok = 0;

    if (disco.hay && b->lba < disco.sectores) {
        if (pet->type == BMSG_LEER) {
            ok = (disco_leer((uint32_t)b->lba, 1) == 0);
            if (ok) for (int i = 0; i < 512; i++) resp.data[i] = (char)d[i];
        } else {
            for (int i = 0; i < 512; i++) d[i] = b->datos[i];
            ok = (disco_escribir((uint32_t)b->lba, 1) == 0);
        }
    }

    resp.type = ok ? BMSG_OK : BMSG_ERROR;
    resp.len  = (ok && pet->type == BMSG_LEER) ? 512 : 0;
    msg_send(b->port, &resp);
}

/* --- La tarjeta de red: tramas por un cable USB ------------------------------
 *
 * Una tarjeta de red USB es un dispositivo que hace dos cosas: acepta tramas
 * Ethernet por un endpoint bulk OUT y las entrega por uno bulk IN. Todo lo
 * demas -direcciones IP, puertos, la hora- va DENTRO de las tramas y no es
 * asunto suyo. Este paso llega hasta ahi: mandar una trama y ver que la red
 * contesta.
 *
 * Hay dos tarjetas porque hay dos maquinas. En la Pi, el LAN9514 de SMSC:
 * una tarjeta de verdad, con un chip Ethernet y un PHY, que se configura por
 * REGISTROS (peticiones de fabricante por el endpoint 0: "escribe este valor
 * en esta direccion") y que envuelve cada trama con una cabecera propia. En
 * QEMU, CDC-ECM: la clase estandar de "Ethernet por USB", sin registros ni
 * cabeceras, que solo pide que se active la interfaz de datos. El mismo
 * driver los lleva a los dos, y a partir de "manda esta trama" no se
 * distinguen. */
#define NIC_NINGUNA  0
#define NIC_LAN9514  1
#define NIC_ECM      2

struct nic {
    int hay, tipo, addr, mps0;
    int ep_in, ep_out, mps_in, mps_out, tog_out;
    int iface_datos, idx_mac;            /* ECM: la interfaz de datos y la MAC */
    int enlace;                          /* hay cable y se ha negociado       */
    uint8_t mac[6];
};
static struct nic nic;

#define OFF_RX    16384                  /* 2 KB para recibir                 */
#define OFF_TX    20480                  /* 2 KB para enviar                  */
#define RX_BYTES   2048                  /* una trama de 1522 cabe de sobra   */
#define CANAL_RX      1                  /* el segundo canal del DWC2         */

/* --- Los registros del LAN9514, por el endpoint 0 ------------------------ */
#define LAN_ID_REV      0x00
#define LAN_INT_STS     0x08
#define LAN_TX_CFG      0x10
#define LAN_HW_CFG      0x14
#define LAN_PM_CTRL     0x20
#define LAN_LED_GPIO    0x24
#define LAN_AFC_CFG     0x2C
#define LAN_BURST_CAP   0x38
#define LAN_BULK_IN_DLY 0x6C
#define LAN_MAC_CR      0x100
#define LAN_ADDRH       0x104
#define LAN_ADDRL       0x108
#define LAN_HASHH       0x10C
#define LAN_HASHL       0x110
#define LAN_MII_ADDR    0x114
#define LAN_MII_DATA    0x118
#define LAN_FLOW        0x11C
#define LAN_VLAN1       0x120
#define LAN_COE_CR      0x130

#define HW_CFG_LRST     0x00000008
#define HW_CFG_RXDOFF   0x00000600
#define PM_CTL_PHY_RST  0x00000010
#define TX_CFG_ON       0x00000004
#define MAC_CR_FDPX     0x00100000
#define MAC_CR_RCVOWN   0x00800000
#define MAC_CR_MCPAS    0x00080000
#define MAC_CR_PRMS     0x00040000
#define MAC_CR_HPFILT   0x00002000
#define MAC_CR_TXEN     0x00000008
#define MAC_CR_RXEN     0x00000004
#define LED_SPD_LNK_FDX 0x01110000
#define AFC_CFG_DEFECTO 0x00F830A1
#define MII_BUSY        0x01
#define MII_WRITE       0x02
#define PHY_ID          1
#define TX_CMD_A_FIRST  0x00002000
#define TX_CMD_A_LAST   0x00001000
#define RX_STS_ES       0x00008000

static int lan_leer(uint32_t reg, uint32_t *v)
{
    if (control_leer(nic.addr, nic.mps0, 0xC0, 0xA1, 0, (uint16_t)reg, 4) < 0) return -1;
    volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_DATOS);
    *v = d[0] | (d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    return 0;
}

static int lan_escribir(uint32_t reg, uint32_t v)
{
    volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_DATOS);
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
    return control_escribir_datos(nic.addr, nic.mps0, 0x40, 0xA0, 0, (uint16_t)reg, 4);
}

/* El PHY -el chip que habla con el cable- no esta en el bus USB: se le llega
 * a traves de dos registros del LAN9514, direccion y dato, con un bit de
 * "ocupado" que hay que esperar. Es MDIO, el mismo bus serie de dos hilos
 * de cualquier tarjeta Ethernet, solo que aqui cada acceso son dos o tres
 * peticiones USB. */
static int mii_espera(void)
{
    for (int i = 0; i < 100; i++) {
        uint32_t v;
        if (lan_leer(LAN_MII_ADDR, &v) < 0) return -1;
        if (!(v & MII_BUSY)) return 0;
    }
    return -1;
}

static int mii_leer(int reg, uint32_t *v)
{
    if (mii_espera() < 0) return -1;
    if (lan_escribir(LAN_MII_ADDR, (PHY_ID << 11) | (reg << 6) | MII_BUSY) < 0) return -1;
    if (mii_espera() < 0) return -1;
    if (lan_leer(LAN_MII_DATA, v) < 0) return -1;
    *v &= 0xFFFF;
    return 0;
}

static int mii_escribir(int reg, uint32_t v)
{
    if (mii_espera() < 0) return -1;
    if (lan_escribir(LAN_MII_DATA, v) < 0) return -1;
    if (lan_escribir(LAN_MII_ADDR, (PHY_ID << 11) | (reg << 6) | MII_WRITE | MII_BUSY) < 0) return -1;
    return mii_espera();
}

/* Arrancar el LAN9514: el orden es el de smsc95xx_reset de Linux, que es la
 * implementacion de referencia y la que lleva anyos en esta misma placa.
 *
 *   1. reset "lite" del chip y reset del PHY;
 *   2. la MAC en sus dos registros (la placa no tiene EEPROM: viene de la
 *      GPU, por el buzon);
 *   3. una trama por transferencia -sin modo turbo- y sin desplazamiento
 *      de datos: la trama va justo detras de su palabra de estado;
 *   4. LEDs, control de flujo, sin descarga de sumas, solo unicast y
 *      broadcast;
 *   5. el PHY: reset, anunciar todo, negociar, y esperar el enlace;
 *   6. y por fin, TX y RX encendidos.
 *
 * Una diferencia a proposito con Linux: no se pone HW_CFG_BIR. Con ese bit
 * la tarjeta contesta un paquete vacio cuando no tiene tramas; sin el,
 * contesta NAK, y un NAK es justo lo que hace que el canal de recepcion se
 * quede esperando solo, sin que nadie tenga que preguntar. */
static int lan_arrancar(void)
{
    uint32_t v;

    if (lan_escribir(LAN_HW_CFG, HW_CFG_LRST) < 0) return -1;
    for (int i = 0; i < 100; i++) {
        sleep(1);
        if (lan_leer(LAN_HW_CFG, &v) < 0) return -1;
        if (!(v & HW_CFG_LRST)) break;
    }
    if (v & HW_CFG_LRST) { printf("  [usb] el LAN9514 no sale del reset\n"); return -1; }

    if (lan_leer(LAN_PM_CTRL, &v) < 0) return -1;
    if (lan_escribir(LAN_PM_CTRL, v | PM_CTL_PHY_RST) < 0) return -1;
    for (int i = 0; i < 100; i++) {
        sleep(1);
        if (lan_leer(LAN_PM_CTRL, &v) < 0) return -1;
        if (!(v & PM_CTL_PHY_RST)) break;
    }

    uint64_t m = mac();
    if (!m) { printf("  [usb] la GPU no me da la MAC de la placa\n"); return -1; }
    for (int i = 0; i < 6; i++) nic.mac[i] = (uint8_t)(m >> (8 * i));
    lan_escribir(LAN_ADDRL, (uint32_t)(m & 0xFFFFFFFF));
    lan_escribir(LAN_ADDRH, (uint32_t)((m >> 32) & 0xFFFF));

    if (lan_leer(LAN_HW_CFG, &v) < 0) return -1;
    lan_escribir(LAN_HW_CFG, v & ~HW_CFG_RXDOFF);      /* sin BIR, sin turbo */
    lan_escribir(LAN_BURST_CAP, 0);
    lan_escribir(LAN_BULK_IN_DLY, 0x2000);
    lan_escribir(LAN_INT_STS, 0xFFFFFFFF);

    if (lan_leer(LAN_ID_REV, &v) == 0)
        printf("  [usb] LAN9514: ID_REV = 0x%08x (chip %04x, revision %04x)\n",
               (unsigned)v, (unsigned)(v >> 16), (unsigned)(v & 0xFFFF));

    if (lan_leer(LAN_LED_GPIO, &v) == 0) lan_escribir(LAN_LED_GPIO, v | LED_SPD_LNK_FDX);
    lan_escribir(LAN_FLOW, 0);
    lan_escribir(LAN_AFC_CFG, AFC_CFG_DEFECTO);
    lan_escribir(LAN_VLAN1, 0x8100);
    lan_escribir(LAN_COE_CR, 0);
    lan_escribir(LAN_HASHH, 0);
    lan_escribir(LAN_HASHL, 0);

    uint32_t mac_cr;
    if (lan_leer(LAN_MAC_CR, &mac_cr) < 0) return -1;
    mac_cr &= ~(MAC_CR_PRMS | MAC_CR_MCPAS | MAC_CR_HPFILT);

    /* El PHY: reset, anunciar 10/100 en ambos duplex con pausa, y negociar. */
    mii_escribir(0, 0x8000);
    for (int i = 0; i < 50; i++) { sleep(1); if (mii_leer(0, &v) == 0 && !(v & 0x8000)) break; }
    mii_escribir(4, 0x0DE1);
    mii_escribir(0, 0x1200);

    /* Esperar el enlace, hasta 3 s. BMSR guarda el "se cayo" hasta que se lee:
     * se lee dos veces para ver el estado de ahora. */
    nic.enlace = 0;
    for (int i = 0; i < 300; i++) {
        sleep(1);
        if (mii_leer(1, &v) < 0) break;
        if (mii_leer(1, &v) < 0) break;
        if (v & 0x0004) { nic.enlace = 1; break; }
    }
    if (nic.enlace) {
        uint32_t lpa = 0;
        mii_leer(5, &lpa);
        int full = (lpa & 0x0140) != 0;          /* 100 o 10 en full duplex */
        int cien = (lpa & 0x0180) != 0;
        if (full) mac_cr = (mac_cr | MAC_CR_FDPX) & ~MAC_CR_RCVOWN;
        else      mac_cr = (mac_cr & ~MAC_CR_FDPX) | MAC_CR_RCVOWN;
        printf("  [usb] enlace: %s Mbit/s, %s duplex\n", cien ? "100" : "10", full ? "full" : "half");
    } else {
        printf("  [usb] sin enlace: hay cable?\n");
    }

    lan_escribir(LAN_TX_CFG, TX_CFG_ON);
    lan_escribir(LAN_MAC_CR, mac_cr | MAC_CR_TXEN | MAC_CR_RXEN);
    return 0;
}

/* CDC-ECM: activar la interfaz de datos (su alternativa 0 no tiene endpoints;
 * la 1 si) y leer la MAC, que la clase guarda como una CADENA de doce
 * hexadecimales en un descriptor de texto. */
static int ecm_arrancar(void)
{
    if (control_escribir(nic.addr, nic.mps0, 0x01, 11, 1, (uint16_t)nic.iface_datos) < 0) {
        printf("  [usb] la tarjeta no activa su interfaz de datos\n");
        return -1;
    }
    if (nic.idx_mac &&
        control_leer(nic.addr, nic.mps0, 0x80, 6, (uint16_t)(0x0300 | nic.idx_mac), 0x0409, 26) >= 0) {
        volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_DATOS);
        for (int i = 0; i < 12; i++) {
            int c = d[2 + 2 * i];
            int h = (c >= '0' && c <= '9') ? c - '0' : (c | 0x20) - 'a' + 10;
            nic.mac[i / 2] = (uint8_t)((nic.mac[i / 2] << 4) | (h & 0xF));
        }
    }
    nic.enlace = 1;
    return 0;
}

/* --- Recibir: un canal que se queda esperando --------------------------------
 *
 * Una tarjeta de red no avisa de que tiene una trama; hay que preguntarle
 * con un IN. Pero preguntar cada 10 ms con el canal 0 -el de todo lo demas-
 * tiene un problema que no tiene el teclado: cuando no hay trama la tarjeta
 * contesta NAK, y para un bulk IN el DWC2 no se detiene con el NAK, sino que
 * lo reintenta el solo hasta que haya datos. El canal 0 se quedaria colgado
 * esperando una trama, con el teclado y el disco detras.
 *
 * El DWC2 tiene ocho canales. La recepcion va por el 1: se programa un IN de
 * 2 KB, se deja habilitado, y se vuelve a lo demas. Mientras no hay tramas el
 * nucleo repite el IN por su cuenta y el canal sigue "activo"; cuando llega
 * una, el canal se detiene con XFERCOMPL y ahi esta. Mirar si se ha detenido
 * es leer un registro, y se hace a cada alarma. Es lo que en Linux hace la
 * URB de recepcion siempre pendiente, y aqui sale sin interrupciones. */
static int rx_armado, rx_tog;
static unsigned tramas_recibidas, tramas_bytes;

static void rx_armar(void)
{
    int paquetes = RX_BYTES / nic.mps_in;

    escribir(HCINTMSK(CANAL_RX), 0);
    escribir(HCINT(CANAL_RX), 0xFFFFFFFF);
    escribir(HAINTMSK, leer(HAINTMSK) | (1u << CANAL_RX));
    escribir(HCSPLT(CANAL_RX), 0);                    /* de alta, o raiz completa */
    escribir(HCCHAR(CANAL_RX), HCC_MC(1) | HCC_ADDR(nic.addr) | HCC_TIPO(EP_BULK) |
                               HCC_IN | HCC_EP(nic.ep_in) | HCC_MPS(nic.mps_in));
    escribir(HCTSIZ(CANAL_RX), HCT_PID(rx_tog ? PID_DATA1 : PID_DATA0) |
                               HCT_PAQUETES(paquetes) | HCT_BYTES(RX_BYTES));
    escribir(HCDMA(CANAL_RX), dma_bus ? BUS(dma_pa + OFF_RX) : (uint32_t)(dma_pa + OFF_RX));
    escribir(HCINTMSK(CANAL_RX), HCI_CHHLTD);
    barrera();
    escribir(HCCHAR(CANAL_RX), leer(HCCHAR(CANAL_RX)) | HCC_ENABLE);
    rx_armado = 1;
}

/* Bytes que han llegado al canal 1, 0 si sigue esperando, -1 si fue mal. */
static int rx_mirar(void)
{
    if (!rx_armado) return 0;
    uint32_t i = leer(HCINT(CANAL_RX));
    if (!(i & HCI_CHHLTD)) return 0;

    barrera();
    uint32_t t = leer(HCTSIZ(CANAL_RX));
    escribir(HCINT(CANAL_RX), 0xFFFFFFFF);
    rx_armado = 0;

    if (!(i & HCI_XFERCOMPL)) {
        if (detallado) quejarse_canal("recepcion", i);
        if (i & HCI_STALL) {
            control_escribir(nic.addr, nic.mps0, 0x02, 1, 0, (uint16_t)(nic.ep_in | 0x80));
            rx_tog = 0;
        }
        return -1;
    }
    int n = RX_BYTES - (int)(t & 0x7FFFF);
    int paquetes = n ? (n + nic.mps_in - 1) / nic.mps_in : 1;
    rx_tog ^= (paquetes & 1);
    return n;
}

/* Mandar una trama. El LAN9514 quiere delante dos palabras -"primer y ultimo
 * trozo, tantos bytes" y "tantos bytes"-; ECM, nada. Y si el total es un
 * multiplo del paquete maximo, un paquete vacio detras: es como el otro lado
 * sabe que la transferencia ha terminado. */
static int nic_enviar(const uint8_t *f, int n)
{
    volatile uint8_t *t = (volatile uint8_t *)(dma_va + OFF_TX);
    int off = 0;

    if (n > 1514 || !nic.hay) return -1;

    if (nic.tipo == NIC_LAN9514) {
        uint32_t a = (uint32_t)n | TX_CMD_A_FIRST | TX_CMD_A_LAST, b = (uint32_t)n;
        t[0] = (uint8_t)a; t[1] = (uint8_t)(a >> 8); t[2] = (uint8_t)(a >> 16); t[3] = (uint8_t)(a >> 24);
        t[4] = (uint8_t)b; t[5] = (uint8_t)(b >> 8); t[6] = (uint8_t)(b >> 16); t[7] = (uint8_t)(b >> 24);
        off = 8;
    }
    for (int i = 0; i < n; i++) t[off + i] = f[i];
    int total = off + n;

    uint32_t r = canal_transferir(0, EP_BULK, nic.mps_out, nic.addr, nic.ep_out,
                                  nic.tog_out ? PID_DATA1 : PID_DATA0, dma_pa + OFF_TX, total);
    if (!(r & HCI_XFERCOMPL)) { quejarse_canal("no pude mandar la trama", r); return -1; }
    nic.tog_out ^= (((total + nic.mps_out - 1) / nic.mps_out) & 1);

    if (total % nic.mps_out == 0) {
        r = canal_transferir(0, EP_BULK, nic.mps_out, nic.addr, nic.ep_out,
                             nic.tog_out ? PID_DATA1 : PID_DATA0, dma_pa + OFF_TX, 0);
        if (!(r & HCI_XFERCOMPL)) return -1;
        nic.tog_out ^= 1;
    }
    return 0;
}

/* --- Una trama que valga como prueba: DHCP DISCOVER ---------------------------
 *
 * Para saber que la red contesta hace falta preguntarle algo a lo que
 * conteste sin conocernos. DHCP es exactamente eso: "soy la MAC tal, no
 * tengo direccion, alguien me da una?", a todos (broadcast), desde 0.0.0.0.
 * Cualquier red con un router responde con una OFERTA. Es la primera trama
 * de la pila que vendra en el paso siguiente; aqui solo se construye a mano,
 * byte a byte, para ver que el cable funciona en los dos sentidos. */
static uint16_t suma_ip(const uint8_t *p, int n)
{
    uint32_t s = 0;
    for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (n & 1) s += (uint32_t)(p[n - 1] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static int dhcp_descubrir(uint8_t *f)
{
    int n = 0;
    for (int i = 0; i < 6; i++) f[n++] = 0xFF;                 /* a todos */
    for (int i = 0; i < 6; i++) f[n++] = nic.mac[i];
    f[n++] = 0x08; f[n++] = 0x00;                                /* IPv4 */

    int ip = n;
    uint8_t cab[20] = { 0x45, 0, 0, 0, 0x12, 0x34, 0, 0, 64, 17, 0, 0,
                        0, 0, 0, 0, 255, 255, 255, 255 };
    for (int i = 0; i < 20; i++) f[n++] = cab[i];

    int udp = n;
    f[n++] = 0; f[n++] = 68; f[n++] = 0; f[n++] = 67; f[n++] = 0; f[n++] = 0; f[n++] = 0; f[n++] = 0;

    int bootp = n;
    f[n++] = 1; f[n++] = 1; f[n++] = 6; f[n++] = 0;              /* peticion, Ethernet */
    f[n++] = 'T'; f[n++] = 'i'; f[n++] = 'n'; f[n++] = 'y';      /* xid */
    f[n++] = 0; f[n++] = 0; f[n++] = 0x80; f[n++] = 0;           /* secs, flags: contestame a todos */
    for (int i = 0; i < 16; i++) f[n++] = 0;                     /* ciaddr yiaddr siaddr giaddr */
    for (int i = 0; i < 6; i++) f[n++] = nic.mac[i];
    for (int i = 0; i < 10 + 64 + 128; i++) f[n++] = 0;
    f[n++] = 99; f[n++] = 130; f[n++] = 83; f[n++] = 99;         /* la galleta magica */
    f[n++] = 53; f[n++] = 1; f[n++] = 1;                          /* DISCOVER */
    f[n++] = 55; f[n++] = 4; f[n++] = 1; f[n++] = 3; f[n++] = 6; f[n++] = 42;  /* mascara, router, DNS, NTP */
    f[n++] = 255;
    while (n - bootp < 300) f[n++] = 0;                          /* BOOTP minimo */

    int ludp = n - udp, lip = n - ip;
    f[udp + 4] = (uint8_t)(ludp >> 8); f[udp + 5] = (uint8_t)ludp;
    f[ip + 2]  = (uint8_t)(lip >> 8);  f[ip + 3]  = (uint8_t)lip;
    uint16_t c = suma_ip(f + ip, 20);
    f[ip + 10] = (uint8_t)(c >> 8); f[ip + 11] = (uint8_t)c;
    return n;
}

/* Lo que llega, por ahora, se ensenya: las primeras tramas enteras en su
 * cabecera, y de las que se entienden -ARP, una oferta DHCP- lo que dicen. */
static void trama_llego(const uint8_t *f, int n)
{
    tramas_recibidas++;
    tramas_bytes += (unsigned)n;
    if (n < 14) return;

    unsigned tipo = (f[12] << 8) | f[13];
    if (tramas_recibidas <= 6)
        printf("  [usb] trama %u, %d bytes: de %02x:%02x:%02x:%02x:%02x:%02x para %02x:%02x:%02x:%02x:%02x:%02x, tipo 0x%04x\n",
               tramas_recibidas, n, f[6], f[7], f[8], f[9], f[10], f[11],
               f[0], f[1], f[2], f[3], f[4], f[5], tipo);

    if (tipo == 0x0806 && n >= 42 && f[21] == 1 && tramas_recibidas <= 6)
        printf("  [usb]   ARP: quien tiene %d.%d.%d.%d? pregunta %d.%d.%d.%d\n",
               f[38], f[39], f[40], f[41], f[28], f[29], f[30], f[31]);

    if (tipo == 0x0800 && n >= 34 + 8 + 240 && f[23] == 17) {
        int ihl = (f[14] & 0xF) * 4, udp = 14 + ihl, b = udp + 8;
        unsigned dst = (f[udp + 2] << 8) | f[udp + 3];
        if (dst == 68 && f[b] == 2) {
            const uint8_t *o = f + b + 240;
            int msg = 0; const uint8_t *srv = 0;
            while (o + 1 < f + n && o[0] != 255) {
                if (o[0] == 53) msg = o[2];
                if (o[0] == 54) srv = o + 2;
                o += (o[0] == 0) ? 1 : 2 + o[1];
            }
            printf("  [usb]   DHCP: %s de %d.%d.%d.%d, me %s %d.%d.%d.%d\n",
                   msg == 2 ? "OFERTA" : msg == 5 ? "ACK" : "mensaje",
                   srv ? srv[0] : f[26], srv ? srv[1] : f[27], srv ? srv[2] : f[28], srv ? srv[3] : f[29],
                   msg == 2 ? "ofrece" : "da",
                   f[b + 16], f[b + 17], f[b + 18], f[b + 19]);
        }
    }
}

/* A cada alarma: si el canal de recepcion se ha detenido, hay trama; se
 * saca de su envoltorio y se vuelve a armar. */
static void red_sondear(void)
{
    if (!nic.hay) return;
    int n = rx_mirar();
    if (n > 0) {
        const uint8_t *rx = (const uint8_t *)(dma_va + OFF_RX);
        if (nic.tipo == NIC_LAN9514) {
            if (n >= 4) {
                uint32_t h = rx[0] | (rx[1] << 8) | ((uint32_t)rx[2] << 16) | ((uint32_t)rx[3] << 24);
                int tam = (int)((h >> 16) & 0x3FFF);
                if (!(h & RX_STS_ES) && tam >= 18 && tam <= n - 4)
                    trama_llego(rx + 4, tam - 4);       /* sin la palabra ni el CRC */
                else if (detallado)
                    printf("  [usb] trama con error: estado 0x%08x, %d bytes\n", (unsigned)h, n);
            }
        } else {
            trama_llego(rx, n);
        }
    }
    if (!rx_armado) rx_armar();
}

static int nic_arrancar(void)
{
    int r = (nic.tipo == NIC_LAN9514) ? lan_arrancar() : ecm_arrancar();
    if (r < 0) return -1;

    printf("  [usb] tarjeta de red %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           nic.tipo == NIC_LAN9514 ? "LAN9514" : "CDC-ECM",
           nic.mac[0], nic.mac[1], nic.mac[2], nic.mac[3], nic.mac[4], nic.mac[5]);

    rx_tog = 0; nic.tog_out = 0;
    rx_armar();

    static uint8_t trama[600];
    int n = dhcp_descubrir(trama);
    if (nic_enviar(trama, n) == 0)
        printf("  [usb] DHCP DISCOVER mandado (%d bytes): a ver quien contesta\n", n);
    return 0;
}

/* --- HID: el idioma de los teclados -------------------------------------------
 *
 * Un dispositivo HID describe con un "report descriptor" el formato de lo que
 * manda, y un driver de verdad lo interpreta. Pero los teclados y ratones
 * tienen ademas el PROTOCOLO BOOT: un formato fijo que existe para que una
 * BIOS pueda leerlos sin interpretar nada. Es lo que se usa aqui.
 *
 *   teclado, 8 bytes: modificadores, reservado, y hasta 6 teclas pulsadas
 *   raton,   3 bytes: botones, dx, dy
 *
 * Los codigos de tecla no son ASCII: son "usages" del HID, y la 'a' es el 4.
 * La tabla de abajo traduce lo justo para ver que llega. */
#define HID_SET_IDLE      0x0A
#define HID_SET_PROTOCOL  0x0B
#define HID_PROTO_BOOT    0

struct hid_ep {
    int addr, ep, mps, intervalo, iface;
    int split, puerto, baja;         /* como se le habla */
    int toggle;                      /* DATA0/DATA1 del endpoint, que persiste */
    int hay;
};


static struct hid_ep teclado, raton;

/* Lo que contesta el endpoint al sondearlo. "No se ve nada" no es un
 * diagnostico; "dos mil NAK y ningun NYET" si lo es. */
static unsigned sondeo_datos, sondeo_nak, sondeo_nyet, sondeo_nada, sondeo_error;

/* Leer la configuracion entera y apuntar donde esta lo que se entiende: un
 * teclado, un raton, un disco. */
static int descubrir(int addr, int mps, unsigned vendedor, unsigned producto)
{
    volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_DATOS);
    int config = 0;

    /* Un dispositivo puede traer VARIAS configuraciones, y solo una esta
     * activa. La tarjeta de red de QEMU trae dos: RNDIS -el protocolo de
     * Microsoft- la primera, y CDC-ECM -el estandar- la segunda. Se leen en
     * orden y se elige la primera en la que haya algo que se entienda. */
    for (int ci = 0; ci < num_configs && ci < 4; ci++) {
    if (control_leer(addr, mps, 0x80, 6, (uint16_t)(0x0200 | ci), 0, 9) < 0) return -1;
    int total = d[2] | (d[3] << 8);
    if (total > 255) total = 255;

    /* El descriptor entero: interfaces y endpoints seguidos, cada uno con su
     * longitud en el byte 0 y su tipo en el 1. Se recorre; no se supone. */
    if (control_leer(addr, mps, 0x80, 6, (uint16_t)(0x0200 | ci), 0, total) < 0) return -1;

    config = d[5];
    int iface = -1, clase = 0, sub = 0, proto = 0, alt = 0;
    if (num_configs > 1) printf("  [usb]   configuracion %d de %d:\n", ci + 1, num_configs);

    for (int i = 0; i + 1 < total; i += d[i] ? d[i] : 1) {
        int tipo = d[i + 1];

        if (tipo == 4) {                            /* interfaz */
            iface = d[i + 2]; alt = d[i + 3]; clase = d[i + 5]; sub = d[i + 6]; proto = d[i + 7];
            printf("  [usb]   interfaz %d: clase %d.%d protocolo %d%s\n",
                   iface, clase, sub, proto,
                   clase == 3 ? (proto == 1 ? " (teclado boot)" :
                                 proto == 2 ? " (raton boot)" : " (HID)") :
                   clase == 8 ? (sub == 6 && proto == 0x50 ? " (disco SCSI, bulk-only)"
                                                           : " (almacenamiento)") :
                   clase == 2 && sub == 6 ? " (Ethernet CDC-ECM, control)" :
                   clase == 10 ? (alt ? " (datos, activa)" : " (datos, apagada)") :
                   clase == 255 && vendedor == 0x0424 ? " (LAN9514)" : "");
            if (clase == 2 && sub == 6 && !nic.hay) {
                nic.tipo = NIC_ECM; nic.addr = addr; nic.mps0 = mps;
            }
        }
        /* ECM guarda el indice de la cadena con la MAC en un descriptor
         * propio de la clase: tipo 0x24, subtipo 0x0F. */
        if (tipo == 0x24 && d[i + 2] == 0x0F && nic.tipo == NIC_ECM && nic.addr == addr)
            nic.idx_mac = d[i + 3];

        /* Los endpoints bulk de una tarjeta de red: los de la interfaz de
         * datos activa (ECM) o los de la unica interfaz del LAN9514. */
        int es_red = (nic.tipo == NIC_ECM && nic.addr == addr && clase == 10 && alt == 1) ||
                     (clase == 255 && vendedor == 0x0424 && producto == 0xec00);
        if (tipo == 5 && es_red && (d[i + 3] & 3) == 2) {
            int mps_ep = d[i + 4] | (d[i + 5] << 8);
            printf("  [usb]     endpoint 0x%02x bulk, %d bytes\n", d[i + 2], mps_ep);
            if (nic.tipo != NIC_ECM) { nic.tipo = NIC_LAN9514; nic.addr = addr; nic.mps0 = mps; }
            nic.iface_datos = iface;
            if (d[i + 2] & 0x80) { nic.ep_in  = d[i + 2] & 0xF; nic.mps_in  = mps_ep; }
            else                 { nic.ep_out = d[i + 2] & 0xF; nic.mps_out = mps_ep; }
            if (nic.ep_in && nic.ep_out) nic.hay = 1;
        }
        /* Un disco: clase 8, subclase 6 (ordenes SCSI transparentes) y
         * protocolo 0x50 (bulk-only). Sus dos endpoints bulk, uno por sentido. */
        if (tipo == 5 && clase == 8 && sub == 6 && proto == 0x50 && (d[i + 3] & 3) == 2) {
            int mps_ep = d[i + 4] | (d[i + 5] << 8);
            printf("  [usb]     endpoint 0x%02x bulk, %d bytes\n", d[i + 2], mps_ep);
            if (!disco.hay) {
                disco.addr = addr; disco.mps0 = mps; disco.iface = iface;
                disco.split = split_activo; disco.puerto = split_puerto;
                disco.baja = dispositivo_baja;
            }
            if (d[i + 2] & 0x80) { disco.ep_in  = d[i + 2] & 0xF; disco.mps_in  = mps_ep; }
            else                 { disco.ep_out = d[i + 2] & 0xF; disco.mps_out = mps_ep; }
            if (disco.ep_in && disco.ep_out && disco.addr == addr) disco.hay = 1;
        }
        if (tipo == 5 && clase == 3 && (d[i + 2] & 0x80)) {   /* endpoint IN */
            struct hid_ep *e = (proto == 1) ? &teclado : (proto == 2) ? &raton : 0;
            printf("  [usb]     endpoint 0x%02x, %d bytes, cada %d ms\n",
                   d[i + 2], d[i + 4] | (d[i + 5] << 8), d[i + 6]);
            if (e && !e->hay) {
                e->hay = 1; e->addr = addr; e->iface = iface;
                e->ep = d[i + 2] & 0xF;
                e->mps = d[i + 4] | (d[i + 5] << 8);
                e->intervalo = d[i + 6];
                e->split = split_activo; e->puerto = split_puerto;
                e->baja = dispositivo_baja; e->toggle = 0;
            }
        }
    }

    /* Algo entendido en esta configuracion? Entonces es la que se pone. */
    if ((teclado.hay && teclado.addr == addr) || (raton.hay && raton.addr == addr) ||
        (disco.hay && disco.addr == addr) || (nic.hay && nic.addr == addr))
        break;
    if (nic.tipo == NIC_ECM && nic.addr == addr && !nic.hay) nic.tipo = NIC_NINGUNA;
    }   /* configuraciones */

    if (control_escribir(addr, mps, 0x00, 9, (uint16_t)config, 0) < 0) return -1;

    /* Protocolo boot e "idle" a cero -avisa solo cuando cambie algo- en las
     * dos interfaces. Son peticiones de clase a la INTERFAZ (recipient 1). Si
     * el dispositivo no las acepta no pasa nada: casi todos ya nacen en boot. */
    struct hid_ep *es[2] = { &teclado, &raton };
    for (int k = 0; k < 2; k++) {
        if (!es[k]->hay || es[k]->addr != addr) continue;
        control_escribir(addr, mps, 0x21, HID_SET_PROTOCOL, HID_PROTO_BOOT, (uint16_t)es[k]->iface);
        control_escribir(addr, mps, 0x21, HID_SET_IDLE, 0, (uint16_t)es[k]->iface);
    }

    /* Y si lo que hay es un disco, presentarlo: quien es, cuanto mide y
     * como esta repartido. */
    if (disco.hay && disco.addr == addr) {
        disco.tog_in = disco.tog_out = 0;
        if (disco_arrancar() < 0) disco.hay = 0;
        else disco_presentar_sector0();
    }

    /* Y si es una tarjeta de red, arrancarla y mandar la primera trama. */
    if (nic.hay && nic.addr == addr && nic_arrancar() < 0) nic.hay = 0;
    return 0;
}

/* Una lectura del endpoint de interrupcion: un solo ciclo, y NAK es "nada".
 * Devuelve los bytes leidos, 0 si no habia nada, -1 si fue mal. */
static int hid_sondear(struct hid_ep *e, uint8_t *out)
{
    split_activo = e->split; split_hub = DIR_HUB; split_puerto = e->puerto;
    dispositivo_baja = e->baja;
    split_intentos = 1;

    uint32_t r = canal_transferir(1, EP_INT, e->mps, e->addr, e->ep,
                                  e->toggle ? PID_DATA1 : PID_DATA0,
                                  dma_pa + OFF_DATOS, e->mps);

    split_intentos = 40;
    split_activo = 0;

    if (r & HCI_XFERCOMPL) {
        e->toggle ^= 1;
        volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_DATOS);
        int n = ultimo_recibido;
        for (int i = 0; i < n && i < 64; i++) out[i] = d[i];
        sondeo_datos++;
        return n;
    }
    if (r & HCI_NAK)  { sondeo_nak++;  return 0; }
    if (r & HCI_NYET) { sondeo_nyet++; return 0; }
    if (!r)           { sondeo_nada++; return 0; }
    sondeo_error++;
    return -1;
}

/* --- Del "usage" a un byte para la consola ---------------------------------
 *
 * El informe boot trae posiciones de tecla, no letras: el 4 es la tecla
 * donde un teclado americano tiene la 'a'. Traducir es cosa del anfitrion, y
 * por eso el mismo teclado escribe distinto en cada sistema. Esta tabla es la
 * distribucion americana, que es la que la norma usa para nombrar las
 * teclas; la espanyola cambia una docena de posiciones y se anyade cuando
 * haga falta.
 *
 * Lo que sale de aqui es lo mismo que mandaria un terminal por la UART: el
 * Enter es '\n', borrar es 127, Escape es 27, y Ctrl con una letra es esa
 * letra menos 64. Asi la disciplina de linea del kernel no distingue de
 * donde vino la tecla, que es la idea. */
static const char tabla_sin[] =
    "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\x7f\t -=[]\\#;'`,./";
static const char tabla_con[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\x7f\t _+{}|~:\"~<>?";

static char hid_tecla(int usage, int shift, int ctrl, int mayusculas)
{
    if (usage < 4 || usage > 56) return 0;          /* fuera de la tabla */
    int letra = (usage <= 29);
    if (letra && mayusculas) shift = !shift;         /* bloq mayus solo afecta a letras */
    char c = (shift ? tabla_con : tabla_sin)[usage - 4];
    if (ctrl && letra) c = (char)((c & 0x1F));       /* Ctrl-A = 1 ... Ctrl-Z = 26 */
    return c;
}

/* Un informe del teclado boot convertido en bytes para la consola.
 *
 * El informe dice que teclas ESTAN pulsadas, no cuales se acaban de pulsar:
 * comparar con el anterior es lo que convierte "la a sigue abajo" en nada y
 * "la a acaba de bajar" en una 'a'. Y como el teclado no repite, lo hace el
 * anfitrion: media segundo abajo y la ultima tecla se repite treinta veces
 * por segundo, que es lo que uno espera al dejar el dedo puesto. */
static uint8_t tecla_antes[8];
static int     tecla_quieta;            /* sondeos con el informe igual */
static int     tecla_ultima;            /* la ultima que bajo, para repetir */

static int teclado_traducir(const uint8_t *inf, char *out)
{
    int n = 0;
    int shift = (inf[0] & 0x22) != 0, ctrl = (inf[0] & 0x11) != 0;
    static int mayusculas;

    int igual = 1;
    for (int k = 0; k < 8; k++) if (inf[k] != tecla_antes[k]) igual = 0;

    if (igual) {
        /* Repeticion: a partir de medio segundo, cada tres sondeos. */
        if (tecla_ultima && ++tecla_quieta >= 50 && (tecla_quieta % 3) == 0) {
            char c = hid_tecla(tecla_ultima, shift, ctrl, mayusculas);
            if (c) out[n++] = c;
        }
        return n;
    }
    tecla_quieta = 0;

    for (int k = 2; k < 8; k++) {
        int u = inf[k];
        if (!u) continue;
        int ya = 0;
        for (int j = 2; j < 8; j++) if (tecla_antes[j] == u) ya = 1;
        if (ya) continue;                              /* sigue pulsada */

        if (u == 57) { mayusculas = !mayusculas; continue; }   /* bloq mayus */
        char c = hid_tecla(u, shift, ctrl, mayusculas);
        if (c) { out[n++] = c; tecla_ultima = u; }
    }

    /* Si la que se repetia ya no esta, se acabo la repeticion. */
    int sigue = 0;
    for (int j = 2; j < 8; j++) if (inf[j] == tecla_ultima) sigue = 1;
    if (!sigue) tecla_ultima = 0;

    for (int k = 0; k < 8; k++) tecla_antes[k] = inf[k];
    return n;
}

/* Entregar a la consola lo que se ha tecleado. Ctrl-C y Ctrl-Z no son bytes,
 * son ordenes, y las da el kernel: a quien le toca es cosa suya. El resto va
 * a la disciplina de linea por la misma puerta que las teclas de la UART. */
static unsigned teclas_dadas;
static int      rechazo_avisado;

static void empujar(const char *b, int m)
{
    int64_t r = console_push(b, m);
    if (r >= 0) { teclas_dadas += (unsigned)m; return; }

    /* Un -EPERM callado seria un teclado que "no funciona" sin pista alguna.
     * Se dice una vez y basta. */
    if (!rechazo_avisado++)
        printf("  [usb] la consola no me acepta teclas: %ld (%s)\n", (long)r, strerror(errno));
}

static void teclado_entregar(const char *b, int n)
{
    char plano[8];
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (b[i] == 3)       { if (m) { empujar(plano, m); m = 0; } console_int();  continue; }
        if (b[i] == 26)      { if (m) { empujar(plano, m); m = 0; } console_stop(); continue; }
        plano[m++] = b[i];
    }
    if (m) empujar(plano, m);
}

/* --- Lo demas de la configuracion ------------------------------------- */
/* --- La configuracion, en el orden de una implementacion que funciona ----
 *
 * Aqui llevaba siete arranques proponiendo un registro por vez, sacado de
 * memoria. El orden de abajo no es mio: es el de CherryUSB, que es una pila de
 * USB portable con un puerto de DWC2 limpio y probado en varios chips.
 *
 * Comparar mi version con la suya destapo cinco diferencias que no se me
 * habian ocurrido, y el orden era una de ellas. Vale la pena anotar por que
 * cada paso va donde va:
 *
 *   1. HCFG antes de todo, incluido quitar FSLSS -"solo velocidades completa y
 *      baja"-, que yo no tocaba nunca.
 *   2. Las FIFO y su vaciado ANTES de encender el DMA. Al reves, el DMA queda
 *      apuntando a un reparto que se va a mover debajo.
 *   3. El DMA con lectura-modificacion-escritura, no reescribiendo el registro
 *      entero: hay bits ahi que el nucleo pone por su cuenta.
 *   4. Y la interrupcion global AL FINAL, cuando ya esta todo puesto y el
 *      puerto alimentado. Antes de eso no hay nada que interrumpir.
 *
 * La leccion de metodo es la que me llevo: cuando algo no funciona y se
 * empiezan a proponer bits de memoria, lo que hay que hacer es buscar una
 * implementacion que funcione y comparar. Siete arranques mas tarde. */
static void modo_anfitrion(void)
{
    /* 1. Que NO se limite a velocidades lentas. Este bit lo tenia sin mirar
     *    desde el principio, y es literalmente "no hables alta velocidad". */
    escribir(HCFG, leer(HCFG) & ~HCFG_FSLSS);

    /* 2. El reparto de las FIFO y su vaciado, antes del DMA. */
    fifos_repartir();

    /* 3. Las rafagas y el DMA, sin pisar el resto del registro.
     *
     * Y el valor de las rafagas NO es el que uno elegiria leyendo el databook.
     * Yo puse INCR16 -HBSTLEN = 7-, que es una eleccion razonable y en esta
     * placa se traduce en que cada rafaga de DMA se queda en DOS palabras. Se
     * veia sin saber que se veia: todo lo que media 8 bytes o menos funcionaba
     * -el SETUP, la primera lectura- y una lectura de 18 devolvia exactamente
     * 8, con los otros diez intactos.
     *
     * Linux, para la bcm2835 y solo para ella, escribe 0x10 en crudo en
     * GAHBCFG (dwc2_set_bcm_params: "p->ahbcfg = 0x10"), sin usar las macros
     * del campo. Es un bit que en la codificacion generica del DWC2 no tiene
     * nombre, y en este SoC es lo que hace que las rafagas salgan enteras. No
     * se por que; se que es lo que tiene la gente que lo tiene funcionando, y
     * a estas alturas del paso eso vale mas que mi razonamiento. */
    uint32_t ahb = leer(GAHBCFG);
    ahb &= ~AHB_HBSTLEN(0xF);
    ahb |=  0x10 | AHB_DMAEN;
    escribir(GAHBCFG, ahb);

    /* Y los avisos viejos borrados. Sin pedir interrupciones: se pregunta
     * mirando, y con GINTMSK a cero el chip no levanta la linea. Es la unica
     * desviacion deliberada de la referencia -ella si las pide- y el motivo es
     * que este driver todavia no atiende su puerto de mensajes: una
     * interrupcion que nadie recoge acabaria en una tormenta. */
    escribir(GINTMSK, 0);
    escribir(GINTSTS, 0xFFFFFFFF);
}

/* --- El puerto raiz ---------------------------------------------------
 *
 * Uno solo, y de el cuelga TODO: lo que hay enchufado ahi en la Pi 3B es el
 * LAN9514, o sea el hub de los cuatro conectores de fuera Y la tarjeta de red.
 * Este puerto es la unica puerta que hay. */
static void puerto_encender(void)
{
    uint32_t p = leer(HPRT0);
    if (!(p & HPRT_PWR)) hprt_escribir(p | HPRT_PWR);

    /* 200 ms, no 20. Es lo que espera la implementacion de referencia, y da de
     * sobra para la subida de VBUS y para el antirrebote del controlador. */
    sleep(20);

    /* Y AHORA la interrupcion global, con todo lo demas ya puesto. */
    escribir(GAHBCFG, leer(GAHBCFG) | AHB_GLBLINTR);
}

/* Y esperar a que el controlador registre la conexion.
 *
 * Esto no estaba, y era el otro motivo de que la placa no viera su hub. El USB
 * manda ANTIRREBOTAR una conexion al menos 100 ms antes de darla por buena
 * -TATTDB en la norma- porque un conector que se acaba de enchufar hace
 * contacto varias veces mientras entra. El controlador hace ese antirrebote
 * por su cuenta, y hasta que termina PRTCONNSTS sigue a cero.
 *
 * Yo miraba una vez, 20 ms despues de dar corriente. En QEMU salia bien porque
 * ahi no hay rebote que antirrebotar: la conexion es instantanea. Es la
 * segunda vez en este paso que el emulador dice si a algo que el cobre dice
 * no. */
static int puerto_esperar_conexion(void)
{
    return esperar_bit(HPRT0, HPRT_CONNSTS, 1, 100);   /* hasta 1 segundo */
}

/* El estado de las lineas, que es el diagnostico de verdad cuando "no hay nada
 * conectado". Dice lo que el PHY ve en el cobre, y eso no miente:
 *
 *   00  las dos bajas: no hay nada, o estamos en reset
 *   01  D+ alta: hay un dispositivo de velocidad completa o alta
 *   10  D- alta: hay uno de velocidad baja
 *
 * Con "no hay nada conectado" y un 01 aqui, el problema no es el cable: es que
 * el controlador no ha llegado a enterarse. Eso fue exactamente lo que paso. */
static const char *lineas(uint32_t hprt)
{
    switch ((hprt >> 10) & 3) {
    case 0:  return "las dos bajas (nada, o en reset)";
    case 1:  return "D+ alta (hay algo, completa o alta)";
    case 2:  return "D- alta (hay algo, baja)";
    default: return "las dos altas (eso no deberia pasar)";
    }
}

/* Resetear el puerto es lo que hace que el dispositivo del otro lado se
 * presente. El USB manda mantenerlo 10 ms como minimo; aqui se mantiene 60,
 * porque los hubs de verdad tardan y no hay ninguna prisa por arrancar una
 * sola vez. */
static void puerto_reset(void)
{
    hprt_escribir(leer(HPRT0) | HPRT_RST);
    sleep(6);
    hprt_escribir(leer(HPRT0) & ~HPRT_RST);
    sleep(3);                          /* y tiempo para recuperarse */
}

/* --- El reloj del PHY, que NO es el del enlace ---------------------------
 *
 * Aqui hubo un arreglo equivocado, y merece quedar contado porque el error es
 * de los que se repiten: en el paso 62b puse HCFG.FSLSPCLKSEL a 48 MHz cuando
 * el puerto enumeraba a velocidad completa, razonando que "el reloj del bus
 * tiene que ser el del enlace". Lo saque de una implementacion para STM32, y
 * en un STM32 es correcto: aquel chip lleva un PHY DEDICADO de velocidad
 * completa que corre a 48 MHz.
 *
 * Este no. Este lleva un PHY UTMI+ de alta velocidad, y ese PHY corre a
 * 30/60 MHz SIEMPRE, hable a la velocidad que hable: el nucleo divide por
 * dentro. Linux lo dice en dwc2_init_fs_ls_pclk_sel: "High speed PHY running
 * at full speed or high speed -> 30/60 MHz". FSLSPCLKSEL = 48 MHz es solo para
 * quien tiene un PHY de velocidad completa con su propio reloj de 48.
 *
 * Con el reloj mal, el nucleo pone los bits en el cable a una velocidad que no
 * es la del cable. El dispositivo recibe basura, no contesta, y eso es un
 * XACTERR: exactamente lo que dio la placa en cuanto MC dejo de valer cero.
 *
 * Lo que si depende del enlace es HFIR, el numero de relojes por trama: a
 * 60 MHz (PHY de 8 bits, que es lo que dice GUSBCFG.PHYIF), una trama de
 * velocidad completa dura 1 ms = 60000 relojes, y una micro-trama de alta
 * velocidad 125 us = 7500. Es la formula de dwc2_calc_frame_interval. */
static void reloj_del_enlace(uint32_t hprt)
{
    escribir(HCFG, leer(HCFG) & ~3u);           /* 30/60 MHz, siempre aqui */

    int phy16 = (leer(GUSBCFG) & USB_PHYIF16) != 0;
    uint32_t mhz = phy16 ? 30 : 60;

    if (HPRT_SPD(hprt) == 0) escribir(HFIR, 125 * mhz);     /* alta      */
    else                     escribir(HFIR, 1000 * mhz);    /* completa/baja */
}

static const char *velocidad(uint32_t hprt)
{
    switch (HPRT_SPD(hprt)) {
    case 0:  return "alta (480 Mbit/s)";
    case 1:  return "completa (12 Mbit/s)";
    case 2:  return "baja (1,5 Mbit/s)";
    default: return "?";
    }
}

/* 64 KB de DMA. No hacen falta todavia; el numero saldra de cuantos canales
 * tenga el controlador por cuantos bytes quepan en una transferencia, y eso
 * es del paso siguiente. */
#define PAGINAS_DMA  16

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    uint64_t base = mmio_base();
    if (!base) {
        printf("  [usb] no tengo MMIO: init tiene que arrancarme con DEV_USB\n");
        return 1;
    }
    reg = (volatile uint32_t *)base;

    printf("\n  [usb] soy el pid %lu; MMIO del DWC2 en 0x%lx\n",
           (unsigned long)getpid(), (unsigned long)base);

    /* --- 1. ¿Hay alguien ahi? --- */
    uint32_t id = leer(GSNPSID);
    if ((id >> 16) != 0x4F54) {
        /* En QEMU el modelo de la raspi3b puede no traer este controlador, y
         * entonces sale cero o basura. En la Pi de verdad tiene que salir. */
        printf("  [usb] GSNPSID = 0x%08x  <- no parece un DWC2\n",
               (unsigned int)id);
    } else {
        printf("  [usb] GSNPSID = 0x%08x  -> Synopsys DWC2, version %x.%03x\n",
               (unsigned int)id, (unsigned int)((id >> 12) & 0xF),
               (unsigned int)(id & 0xFFF));
    }

    /* GHWCFG2 dice como esta construido este ejemplar concreto, y dos campos
     * deciden como habra que escribir el driver:
     *
     *   bits [4:3]   arquitectura: 0 = por registros (la CPU mueve cada
     *                byte), 1 = DMA externo, 2 = DMA interno. Con DMA
     *                interno el controlador lee y escribe la RAM el solo, y
     *                por eso hace falta lo que acabamos de anyadir.
     *   bits [13:10] canales de anfitrion MENOS UNO. Son las transferencias
     *                que puede tener en vuelo a la vez, y en la Pi son 8.
     *                Ocho canales para todo lo que cuelgue del hub es la
     *                razon de que este controlador necesite un planificador
     *                por software. */
    uint32_t hw2 = leer(GHWCFG2);
    unsigned arq    = (hw2 >> 3) & 0x3;
    unsigned canales = ((hw2 >> 14) & 0xF) + 1;

    static const char *arqs[] = { "por registros", "DMA externo",
                                  "DMA interno", "?" };

    printf("  [usb] GHWCFG2 = 0x%08x  -> %s, %u canales de anfitrion\n",
           (unsigned int)hw2, arqs[arq], canales);
    printf("  [usb] GHWCFG1/3/4 = 0x%08x 0x%08x 0x%08x\n",
           (unsigned int)leer(GHWCFG1), (unsigned int)leer(GHWCFG3),
           (unsigned int)leer(GHWCFG4));

    /* --- 2. Memoria para DMA --- */
    uint64_t pa = 0;
    int64_t  va = dma_alloc(PAGINAS_DMA, &pa);
    dma_va = (uint64_t)va;
    dma_pa = pa;
    if (va < 0) {
        printf("  [usb] sin memoria para DMA: %s\n", strerror(errno));
        return 1;
    }

    (void)0;

    /* Que la fisica este alineada a pagina no es cosmetico: un controlador
     * de DMA con una direccion desalineada escribe donde no debe, y muchos
     * ni siquiera avisan. */
    if (pa & 0xFFF) {
        printf("  [usb] MAL: la fisica no esta alineada a pagina\n");
        return 1;
    }

    /* Y que las paginas sean de verdad escribibles y distintas: se marca
     * cada una con su numero y se releen todas al final. Si dos paginas
     * cayeran en la misma fisica -o alguna no estuviera mapeada- esto lo
     * caza, y es exactamente el fallo que en un DMA de verdad se
     * manifestaria como datos de otro. */
    volatile unsigned char *buf = (volatile unsigned char *)(uint64_t)va;

    for (int p = 0; p < PAGINAS_DMA; p++) {
        buf[p * 4096]         = (unsigned char)(0xA0 + p);
        buf[p * 4096 + 4095]  = (unsigned char)(0x50 + p);
    }
    for (int p = 0; p < PAGINAS_DMA; p++) {
        if (buf[p * 4096] != (unsigned char)(0xA0 + p) ||
            buf[p * 4096 + 4095] != (unsigned char)(0x50 + p)) {
            printf("  [usb] MAL: la pagina %d no conserva lo que se escribio\n", p);
            return 1;
        }
    }
    /* Y en silencio si sale bien, que es la regla de esta casa: lo normal no
      * se anuncia. Arrancando en cada arranque, un driver que recita sus
      * autopruebas llena la consola de ruido que nadie lee. */

    /* Uno por proceso: el segundo intento tiene que fallar, y decir por que. */
    uint64_t otra = 0;
    if (dma_alloc(4, &otra) >= 0)
        printf("  [usb] MAL: me ha dado un segundo tramo\n");
    else if (errno != EBUSY)
        printf("  [usb] MAL: el segundo tramo falla con %s, no con EBUSY\n",
               strerror(errno));
    /* y si dice EBUSY, que es lo que debe, no se dice nada */

    /* --- 3. La interrupcion --- */
    int64_t puerto = port_create(-1);
    if (puerto < 0) {
        printf("  [usb] no he podido abrir un puerto\n");
        return 1;
    }

    if (irq_register(IRQ_USB, (int)puerto) < 0) {
        printf("  [usb] MAL: el kernel no me deja reclamar la IRQ %d\n", IRQ_USB);
        return 1;
    }
    printf("  [usb] IRQ %d reclamada; DMA en PA 0x%lx\n",
           IRQ_USB, (unsigned long)pa);

    /* Y las dos cosas que la tabla NO tiene que dejar hacer.
     *
     * La primera es una que ya tiene duenyo: la de la UART se la quedo el
     * conserver al arrancar, y dos drivers atendiendo la misma fuente es que
     * uno de los dos no se entera nunca.
     *
     * La segunda es una que no esta en la lista. La lista sigue escrita a
     * mano -eso no ha cambiado en este paso- y es la frontera: si un proceso
     * pudiera reclamar la del temporizador, podria parar el planificador. */
    if (irq_register(IRQ_UART, (int)puerto) >= 0)
        printf("  [usb] MAL: me ha dejado quitarle la UART al conserver\n");

    if (irq_register(29, (int)puerto) >= 0)
        printf("  [usb] MAL: me ha dado una IRQ que no esta en la lista\n");

    /* --- 4. Encenderlo de verdad, que no es lo mismo que tocar registros --
     *
     * Lo PRIMERO, antes de escribir un solo bit del controlador. En la Pi el
     * USB lo enciende la GPU, y eso incluye arrancar su PHY: el bloque
     * analogico que pone los unos y ceros en el cable.
     *
     * Y esto no se puede deducir leyendo registros, que es el error que me
     * costo cinco arranques. Con el dominio a medias el AHB va, o sea que todo
     * el banco de registros se lee y se escribe bien y hasta el DMA funciona
     * -se veia: HCDMA avanzaba ocho bytes-, pero el PHY solo da para lo minimo:
     * ver si hay una resistencia en D+ y hablar a velocidad completa. Los
     * paquetes no salen y no hay ningun bit que lo diga.
     *
     * Yo lo descarte con un argumento malo: "los registros se leen bien, asi
     * que esta encendido". Leer registros solo prueba que hay reloj de bus. */
    printf("  [usb] la GPU contesta al encendido: 0x%08x\n",
           (unsigned int)dev_power(PWR_USB));

    /* --- 5. Y ahora si: configurar el controlador --- */
    if (!nucleo_despertar()) {
        printf("  [usb] el nucleo no responde al reset (GRSTCTL = 0x%08x)\n",
               (unsigned int)leer(GRSTCTL));
        return 1;
    }

    modo_anfitrion();
    puerto_encender();

    uint32_t p = leer(HPRT0);
    /* GUSBCFG leido de VUELTA, que es un punto ciego que llevaba tres
     * iteraciones teniendo: lo escribo y nunca compruebo que se sostenga. Si
     * PHYSEL siguiera puesto -el PHY serie de velocidad completa- el puerto
     * enumeraria a velocidad completa sin remedio, que es exactamente lo que
     * lleva pasando. */
    uint32_t ucfg = leer(GUSBCFG);
    printf("  [usb] nucleo en modo anfitrion; HPRT0 = 0x%08x\n",
           (unsigned int)p);
    printf("  [usb] GUSBCFG = 0x%08x: PHY %s, %s, modo %s\n",
           (unsigned int)ucfg,
           (ucfg & USB_PHYSEL_FS) ? "SERIE de velocidad completa" : "UTMI+/ULPI",
           (ucfg & USB_ULPI_SEL)  ? "ULPI" : "UTMI+",
           (ucfg & USB_FORCEHOST) ? "anfitrion forzado" : "segun el pin ID");

    if (!puerto_esperar_conexion()) {
        /* En la Pi aqui hay un LAN9514 soldado, asi que esto no deberia pasar;
         * en QEMU si, porque la raspi3b no trae nada enchufado a menos que se
         * le diga. Y si pasa en la placa, el estado de las lineas dice si el
         * problema esta en el cobre o en nosotros. */
        p = leer(HPRT0);
        printf("  [usb] no hay nada conectado (HPRT0 = 0x%08x)\n",
               (unsigned int)p);
        printf("  [usb] lineas: %s\n", lineas(p));
        for (;;) sleep(1000);
    }

    p = leer(HPRT0);
    printf("  [usb] conectado: %s, lineas %s\n", velocidad(p), lineas(p));

    puerto_reset();
    p = leer(HPRT0);

    printf("  [usb] tras el reset: HPRT0 = 0x%08x, %s, puerto %s\n",
           (unsigned int)p, velocidad(p),
           (p & HPRT_ENA) ? "habilitado" : "SIN habilitar");

    if (!(p & HPRT_ENA)) {
        printf("  [usb] el puerto no quedo habilitado; no hay con quien hablar\n");
        for (;;) sleep(1000);
    }

    printf("  [usb] micro-trama %u: el bus esta vivo\n",
           (unsigned int)(leer(HFNUM) & 0x3FFF));

    /* Y reconocer los avisos de cambio, que es lo que faltaba: si no, quedan
     * puestos para siempre y el driver no puede enterarse del siguiente. */
    hprt_reconocer(HPRT_CONNDET | HPRT_ENCHNG);

    /* El reloj del bus, AHORA que se sabe la velocidad. Antes del reset no se
     * podia saber. */
    reloj_del_enlace(p);
    printf("  [usb] reloj del enlace puesto para %s (HCFG = 0x%08x)\n",
           velocidad(p), (unsigned int)leer(HCFG));

    /* --- 6. Y ahora a hablar: quien eres ----------------------------------- */
    unsigned vendedor = 0, producto = 0, clase = 0;
    int mps0 = -1;

    for (int intento = 1; intento <= 4 && mps0 < 0; intento++) {
        mps0 = presentarse(0, &vendedor, &producto, &clase);
        if (mps0 < 0) { printf("  [usb] intento %d fallido; espero y repito\n", intento); sleep(5); }
    }
    if (mps0 < 0) {
        printf("  [usb] el dispositivo no contesta a un GET_DESCRIPTOR\n");
        for (;;) sleep(1000);
    }

    if (vendedor == 0x0424) printf("  [usb] 0x0424 es SMSC: esto es el hub con la Ethernet dentro\n");
    if (clase != 9) {
        printf("  [usb] no es un hub (clase %u): no se que hacer con el todavia\n", clase);
        for (;;) sleep(1000);
    }

    /* --- 7. Darle una direccion --------------------------------------------
     *
     * Hasta aqui se le ha hablado a la direccion 0, que es la que tiene todo
     * dispositivo recien reseteado. Sirve mientras solo hay uno; en cuanto el
     * hub encienda sus puertos, lo que cuelgue de ellos tambien saldra del
     * reset en la 0, y dos en la misma direccion es que ninguno oye.
     *
     * SET_ADDRESS es la unica peticion que se contesta desde la direccion
     * VIEJA: el dispositivo cambia de nombre despues de decir "hecho". Y la
     * norma le da 2 ms para acostumbrarse antes de que nadie le hable por el
     * nuevo. */
    if (control_escribir(0, mps0, 0x00, 5, DIR_HUB, 0) < 0) {
        printf("  [usb] no acepta SET_ADDRESS\n");
        for (;;) sleep(1000);
    }
    sleep(1);
    printf("  [usb] el hub ya es la direccion %d\n", DIR_HUB);

    /* --- 8. Configurarlo ---------------------------------------------------
     *
     * Un dispositivo sin configurar es un descriptor y nada mas: no tiene
     * endpoints activos ni hace su trabajo. La configuracion se elige por su
     * numero, que esta en el byte 5 de su descriptor de configuracion, y casi
     * siempre es 1. Se lee en vez de suponerlo. */
    volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_DATOS);
    if (control_leer(DIR_HUB, mps0, 0x80, 6, 0x0200, 0, 9) < 0) {
        printf("  [usb] no me da su descriptor de configuracion\n");
        for (;;) sleep(1000);
    }
    int config = d[5];
    if (control_escribir(DIR_HUB, mps0, 0x00, 9, (uint16_t)config, 0) < 0) {
        printf("  [usb] no acepta SET_CONFIGURATION(%d)\n", config);
        for (;;) sleep(1000);
    }
    printf("  [usb] configuracion %d puesta: %u bytes de descriptores, %u interfaz%s\n",
           config, (unsigned)(d[2] | (d[3] << 8)), (unsigned)d[4],
           d[4] == 1 ? "" : "es");

    /* --- 9. El descriptor de hub, que es de clase --------------------------
     * Cuantos puertos, y cuanto tardan en tener corriente buena despues de
     * encenderlos: bPwrOn2PwrGood, en unidades de 2 ms. */
    if (control_leer(DIR_HUB, mps0, HUB_GET_DESCRIPTOR, 6, 0x2900, 0, 9) < 0) {
        printf("  [usb] no me da su descriptor de hub\n");
        for (;;) sleep(1000);
    }
    int puertos   = d[2];
    int pwr_good  = d[5] * 2;             /* ms */
    printf("  [usb] hub de %d puertos, caracteristicas 0x%04x, "
           "corriente buena a los %d ms\n",
           puertos, (unsigned)(d[3] | (d[4] << 8)), pwr_good);
    if (puertos > 8) puertos = 8;

    /* --- 10. Encender los puertos y ver que hay ----------------------------
     * Y esperar lo que el hub dijo, mas el antirrebote de siempre: lo que se
     * enchufa hace contacto varias veces mientras entra. */
    for (int pt = 1; pt <= puertos; pt++)
        control_escribir(DIR_HUB, mps0, HUB_SET_PORT_FEATURE, 3, PORT_POWER, (uint16_t)pt);
    sleep((pwr_good + 100) / 10 + 1);

    int con_algo[8];
    int n_con_algo = 0;
    for (int pt = 1; pt <= puertos; pt++) {
        unsigned est = 0, cam = 0;
        if (hub_estado_puerto(DIR_HUB, mps0, pt, &est, &cam) < 0) {
            printf("  [usb] puerto %d: no contesta al estado\n", pt);
            continue;
        }
        printf("  [usb] puerto %d: 0x%04x/0x%04x %s%s%s\n", pt, est, cam,
               (est & PS_POWER)      ? "alimentado" : "SIN alimentar",
               (est & PS_CONNECTION) ? ", HAY ALGO"  : ", vacio",
               (est & PS_CONNECTION)
                 ? ((est & PS_HIGH_SPEED) ? " (alta)" :
                    (est & PS_LOW_SPEED)  ? " (baja)" : " (completa)") : "");
        if (est & PS_CONNECTION) con_algo[n_con_algo++] = pt;
    }

    if (!n_con_algo) {
        printf("  [usb] ningun puerto tiene nada; en la Pi 3B eso no puede ser\n");
        for (;;) sleep(1000);
    }

    /* --- 11. Cada puerto con algo: resetear, ver la velocidad, preguntar ---
     *
     * Y darle una direccion a cada uno segun aparece, porque lo que sale del
     * reset esta en la 0 y el siguiente tambien va a salir en la 0. La 1 es
     * del hub; a partir de la 2, por orden de puerto.
     *
     * La velocidad decide COMO se le habla. De alta: directamente, como al
     * hub. De completa o baja: partido, a traves del hub, que es el que sabe
     * hablar lento. Y si es de baja, ademas hay que decirselo al canal. */
    int siguiente_dir = 2;
    int raiz_alta = HPRT_SPD(leer(HPRT0)) == 0;

    for (int i = 0; i < n_con_algo; i++) {
        int pt = con_algo[i];
        unsigned est = 0;

        if (hub_resetear_puerto(DIR_HUB, mps0, pt, &est) < 0) {
            printf("  [usb] el puerto %d no sale del reset (0x%04x)\n", pt, est);
            continue;
        }

        int alta = (est & PS_HIGH_SPEED) != 0;
        int baja = (est & PS_LOW_SPEED)  != 0;

        split_activo     = raiz_alta && !alta;
        split_hub        = DIR_HUB;
        split_puerto     = pt;
        dispositivo_baja = baja;

        printf("  [usb] puerto %d reseteado, velocidad %s%s; le pregunto quien es\n",
               pt, alta ? "alta" : baja ? "baja" : "completa",
               split_activo ? " (a traves del hub, partido)" : "");

        unsigned v2 = 0, p2 = 0, c2 = 0;
        int mps2 = -1;
        for (int intento = 1; intento <= 4 && mps2 < 0; intento++) {
            mps2 = presentarse(0, &v2, &p2, &c2);
            if (mps2 < 0) sleep(5);
        }
        if (mps2 < 0) {
            printf("  [usb] lo que hay en el puerto %d no contesta\n", pt);
            split_activo = 0;
            continue;
        }

        if (v2 == 0x0424 && p2 == 0xec00)
            printf("  [usb] 0424:ec00 es la Ethernet del LAN9514. Ahi esta la red.\n");
        if (v2 == 0x046d)
            printf("  [usb] 0x046d es Logitech: el receptor del teclado y el raton\n");

        if (control_escribir(0, mps2, 0x00, 5, (uint16_t)siguiente_dir, 0) < 0) {
            printf("  [usb] no acepta SET_ADDRESS(%d)\n", siguiente_dir);
            split_activo = 0;
            continue;
        }
        sleep(1);
        printf("  [usb] el del puerto %d ya es la direccion %d\n", pt, siguiente_dir);

        /* Mirar dentro de lo que sea -menos un hub- y dejar listo lo que se
         * entienda: teclado, raton, disco, tarjeta de red. */
        if (c2 != 9 && descubrir(siguiente_dir, mps2, v2, p2) == 0 &&
            ((teclado.hay && teclado.addr == siguiente_dir) || (raton.hay && raton.addr == siguiente_dir)))
            printf("  [usb] HID configurado: %s%s%s\n",
                   teclado.hay ? "teclado" : "", (teclado.hay && raton.hay) ? " y " : "",
                   raton.hay ? "raton" : "");

        siguiente_dir++;
        split_activo = 0;
    }

    /* --- 12. Un bucle de mensajes, no un bucle de sondeo --------------------
     *
     * Hasta el paso 67 esto era "sondear el teclado, dormir un tick, repetir".
     * Valia mientras el driver solo tuviera un cliente: el teclado. Ahora
     * tiene dos, y el segundo -el servidor de ficheros pidiendo sectores- no
     * puede esperar a que el driver se despierte de un sleep.
     *
     * La salida es la misma que con las interrupciones en el paso 58: que el
     * tiempo tambien sea un mensaje. El kernel manda CMSG_ALARMA a este
     * puerto cada tick, y el driver hace UNA cosa: esperar en msg_recv. Si lo
     * que llega es la alarma, sondea el teclado; si es una peticion de
     * sector, la sirve y contesta; si es la respuesta del servidor de
     * ficheros, la cuenta. Una peticion del fs despierta al driver en el
     * acto, y el teclado sigue mirandose cada 10 ms. Y el canal 0 del DWC2,
     * que es uno solo, nunca lo usan dos cosas a la vez, porque el bucle
     * atiende un mensaje entero antes de mirar el siguiente. */
    if (teclado.hay || nic.hay) {
        if (alarma((uint64_t)puerto, 1) < 0)
            printf("  [usb] el kernel no me da el reloj: sin teclado ni red\n");
        else if (teclado.hay)
            printf("  [usb] teclado USB listo: lo que teclees va a la consola\n");
    }

    /* Y si hay un disco, decirselo al servidor de ficheros: que lo monte en
     * /mnt y que pida los sectores por este puerto. No se espera la
     * respuesta aqui -para montar tiene que leer sectores, y los sectores
     * los sirve este bucle-: llegara como un mensaje mas. */
    if (disco.hay) {
        struct message m;
        struct fs_disco *fd = (struct fs_disco *)m.data;
        m.type = FS_DISCO; m.len = sizeof(*fd);
        fd->port = (unsigned long)puerto;
        fd->bloques = (unsigned long)puerto;
        fd->sectores = disco.sectores;
        int i = 0;
        for (const char *c = disco.vendedor; *c && i < 30; c++) fd->nombre[i++] = *c;
        fd->nombre[i++] = ' ';
        for (const char *c = disco.producto; *c && i < 31; c++) fd->nombre[i++] = *c;
        fd->nombre[i] = 0;
        if (msg_send(PORT_FILES, &m) < 0)
            printf("  [usb] no hay servidor de ficheros a quien ofrecerle el disco\n");
    }

    if (!teclado.hay && !disco.hay && !nic.hay)
        printf("  [usb] ni teclado, ni disco, ni red; me quedo esperando\n");

    uint8_t inf[64];
    unsigned vueltas = 0;
    for (;;) {
        struct message m;
        if (msg_recv((uint64_t)puerto, &m) < 0) break;

        if (m.type == CMSG_IRQ) { irq_ack(IRQ_USB); continue; }

        if (m.type == FS_OK)    { printf("  [usb] el disco esta montado en /mnt\n"); continue; }
        if (m.type == FS_ERROR) { printf("  [usb] el servidor de ficheros no ha podido montar el disco\n"); continue; }

        if (m.type == BMSG_LEER || m.type == BMSG_ESCRIBIR) {
            disco_atender(&m);
            continue;
        }

        if (m.type != CMSG_ALARMA) continue;

        /* La red primero: mirar si el canal 1 ha recibido, y volver a armarlo. */
        red_sondear();
        if (!teclado.hay) continue;

        /* Un teclado USB no avisa: se le pregunta. Cada bInterval milisegundos
         * el anfitrion le manda un IN a su endpoint de interrupcion, y el
         * teclado contesta NAK -"nada"- o un informe de 8 bytes. Aqui se
         * pregunta a cada alarma, que es un tick, y se traduce lo que llega. */
        if (detallado && ++vueltas % 200 == 0)
            printf("  [usb] sondeos: %u con datos, %u NAK, %u NYET, %u sin respuesta, %u error; %u teclas entregadas\n",
                   sondeo_datos, sondeo_nak, sondeo_nyet, sondeo_nada, sondeo_error, teclas_dadas);

        int n = hid_sondear(&teclado, inf);
        if (detallado && n >= 8) {
            printf("  [usb] informe de %d bytes:", n);
            for (int i = 0; i < 8; i++) printf(" %02x", inf[i]);
            printf("\n");
        }
        if (n >= 8 || (n == 0 && tecla_ultima)) {
            /* Con datos, o sin ellos pero con una tecla abajo: en el segundo
             * caso el informe no ha cambiado y lo que toca es repetir. */
            char teclas[8];
            int t = teclado_traducir(n >= 8 ? inf : tecla_antes, teclas);
            if (t) teclado_entregar(teclas, t);
        } else if (n < 0) {
            printf("  [usb] el teclado ha fallado al sondearlo\n");
            sleep(50);
        }
    }
    return 0;
}
