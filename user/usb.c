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

/* Y este chip quiere la FISICA, no la de bus. Lo dijo el experimento, y de
 * paso me corrigio:
 *
 * Con la de bus, HCDMA volvia avanzado ocho bytes y yo lo lei como "el nucleo
 * leyo los ocho bytes del SETUP". Solo probaba que movio su puntero.
 * 0xC015C000 esta muy por encima de los 996 MB de RAM de esta placa, asi que
 * leyo de ninguna parte, avanzo igual y se atasco sin decir nada.
 *
 * Con la fisica a secas, la transferencia SALIO -llego a dar un XACTERR, que es
 * un error de verdad en el cable-, y eso es lo que distingue "no hizo nada" de
 * "lo hizo y fue mal".
 *
 * La leccion: un contador que avanza no prueba que lo que se leyo fuera lo
 * correcto. Prueba que alguien conto. */
static int dma_bus = 0;

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
                            int addr, int ep, int pid, uint64_t pa, int bytes)
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
    escribir(HCSPLT(canal), 0);         /* sin particion: cuelga del raiz */

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
                            HCC_MPS(mps));

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
    volatile uint8_t *setup = (volatile uint8_t *)(dma_va + OFF_SETUP);
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

    r = canal_hacer(0, 0, EP_CONTROL, mps, addr, 0, PID_SETUP,
                    dma_pa + OFF_SETUP, 8);
    if (!(r & HCI_XFERCOMPL)) { quejarse_canal("el SETUP no paso", r); return -1; }

    r = canal_hacer(0, 1, EP_CONTROL, mps, addr, 0, PID_DATA1,
                    dma_pa + OFF_DATOS, bytes);
    if (!(r & HCI_XFERCOMPL)) { quejarse_canal("los datos no llegaron", r); return -1; }

    r = canal_hacer(0, 0, EP_CONTROL, mps, addr, 0, PID_DATA1,
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

    /* 3. Las rafagas y el DMA, sin pisar el resto del registro. */
    uint32_t ahb = leer(GAHBCFG);
    ahb &= ~AHB_HBSTLEN(0xF);
    ahb |=  AHB_HBSTLEN(7) | AHB_DMAEN;
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

    /* --- 6. Y ahora a hablar ------------------------------------------
     *
     * El primer GET_DESCRIPTOR pide OCHO bytes, y no es timidez: todavia no se
     * sabe cual es el tamanyo maximo de paquete de este dispositivo, y ese dato
     * esta DENTRO del descriptor, en el byte 7. Ocho es el minimo que la norma
     * obliga a soportar a todo el mundo, asi que con ocho se puede leer cuanto
     * se puede leer. El descriptor te dice como leer el descriptor. */
    volatile uint8_t *d = (volatile uint8_t *)(dma_va + OFF_DATOS);

    /* Y si no contesta, se prueba con la otra forma de direccion ANTES de
     * rendirse. Las dos posibilidades son "el chip quiere la direccion de bus"
     * y "el chip quiere la fisica", y averiguarlo probando cuesta un segundo;
     * averiguarlo a base de arrancar la placa cuesta un arranque por intento. */
    /* Y con reintentos, que no es pereza: un XACTERR en la primera peticion a
     * un dispositivo que acaba de salir de un reset es normal. El canal ya
     * reintenta por su cuenta las veces que diga MC, pero un dispositivo que
     * todavia se esta despertando puede fallar las tres. Cualquier pila de USB
     * de verdad reintenta la enumeracion. */
    int ok = 0;
    for (int intento = 1; intento <= 4 && !ok; intento++) {
        if (control_leer(0, 8, 0x80, 6, 0x0100, 0, 8) == 0) { ok = 1; break; }
        printf("  [usb] intento %d fallido; espero y repito\n", intento);
        sleep(5);
    }

    if (!ok) {
        printf("  [usb] el dispositivo no contesta a un GET_DESCRIPTOR\n");
        printf("  [usb] estado del puerto: HPRT0 = 0x%08x, trama %u\n",
               (unsigned int)leer(HPRT0),
               (unsigned int)(leer(HFNUM) & 0x3FFF));
        for (;;) sleep(1000);
    }

    int mps0 = d[7];
    printf("  [usb] contesta: descriptor de %u bytes, USB %x.%02x, "
           "paquete maximo %d\n",
           (unsigned int)d[0], (unsigned int)d[3], (unsigned int)d[2], mps0);

    if (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64) {
        printf("  [usb] ese tamanyo de paquete no es legal; me paro aqui\n");
        for (;;) sleep(1000);
    }

    /* Y ahora el descriptor entero, con el tamanyo de paquete que acaba de
     * decir. Son 18 bytes y los interesantes estan del 8 al 11. */
    if (control_leer(0, mps0, 0x80, 6, 0x0100, 0, 18) < 0) {
        printf("  [usb] el segundo GET_DESCRIPTOR fallo\n");
        for (;;) sleep(1000);
    }

    unsigned vendedor  = (unsigned)(d[8]  | (d[9]  << 8));
    unsigned producto  = (unsigned)(d[10] | (d[11] << 8));

    printf("  [usb] descriptor:");
    for (int i = 0; i < 18; i++) printf(" %02x", (unsigned)d[i]);
    printf("\n");

    printf("  [usb] es %04x:%04x, clase %u, %u configuracion%s\n",
           vendedor, producto, (unsigned)d[4], (unsigned)d[17],
           d[17] == 1 ? "" : "es");

    if (vendedor == 0x0424)
        printf("  [usb] 0x0424 es SMSC: esto es el hub con la Ethernet dentro\n");
    if (d[4] == 9)
        printf("  [usb] clase 9 es HUB, que es lo que tiene que ser\n");

    /* Y aqui se para. Lo siguiente es ponerle una direccion con SET_ADDRESS,
     * leerle el descriptor de hub y encender sus puertos, que es donde
     * aparecera la Ethernet. */
    for (;;) sleep(1000);
}
