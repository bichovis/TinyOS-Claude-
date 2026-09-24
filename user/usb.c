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
#define HCFG       0x400    /* configuracion de anfitrion                    */
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

static volatile uint32_t *reg;

static uint32_t leer(uint64_t off) { return reg[off / 4]; }
static void escribir(uint64_t off, uint32_t v) { reg[off / 4] = v; }

/* Escribir HPRT0 sin pisarse los pies. Ver el comentario de arriba. */
static void hprt_escribir(uint32_t v) { escribir(HPRT0, v & ~HPRT_W1C); }

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
 * El firmware de la Pi deja el DWC2 encendido y a medio configurar, asi que lo
 * primero es ponerlo en un estado CONOCIDO, y conocido quiere decir reset.
 * Heredar la configuracion de otro es como se depura durante tres dias algo
 * que funcionaba en un arranque y no en el siguiente.
 *
 * El orden no es negociable y cada paso tiene su motivo. */
static int nucleo_despertar(void)
{
    /* 1. Quitar las puertas de reloj y la pinza de alimentacion. Con el reloj
     *    cortado los registros contestan basura, y todo lo que venga despues
     *    seria un misterio. */
    escribir(PCGCCTL, 0);

    /* 2. Cerrar la salida de interrupciones mientras se configura. Aun no hay
     *    nadie escuchando, y una interrupcion sin manejador con la fuente
     *    abierta es el sistema girando en el vector. */
    escribir(GAHBCFG, leer(GAHBCFG) & ~AHB_GLBLINTR);

    /* 3. Nada de VBUS externo ni pulsos en las lineas: son cosas de una placa
     *    con transceptor de fuera, y aqui el PHY esta dentro del chip. */
    escribir(GUSBCFG, leer(GUSBCFG) & ~(USB_EXT_VBUS | USB_TS_DLINE));

    /* 4. Esperar a que el bus este quieto ANTES de resetear. Resetear con una
     *    transferencia AHB a medias deja el bus colgado, y con el medio chip. */
    if (!esperar_bit(GRSTCTL, RST_AHBIDLE, 1, 10)) return 0;

    /* 5. Y el reset. Se pide poniendo el bit, y se sabe que acabo cuando el
     *    propio chip lo quita: no hay que quitarlo a mano. */
    escribir(GRSTCTL, RST_CSFTRST);
    if (!esperar_bit(GRSTCTL, RST_CSFTRST, 0, 20)) return 0;
    if (!esperar_bit(GRSTCTL, RST_AHBIDLE, 1, 10)) return 0;

    return 1;
}

/* --- Modo anfitrion ---------------------------------------------------
 *
 * Este controlador es OTG: puede ser anfitrion o dispositivo, y decide segun
 * lo que vea en el pin ID. En la Pi siempre es anfitrion, pero decirselo a
 * mano quita una variable: si el pin flotara, el chip se quedaria esperando a
 * que alguien le hable en vez de hablar el. */
static void modo_anfitrion(void)
{
    uint32_t cfg = leer(GUSBCFG);

    /* El PHY: UTMI+ y de alta velocidad, que es el que este chip lleva dentro.
     * PHYSEL a cero es "no me pongas el serie de velocidad completa", y sin eso
     * la Ethernet -que es de alta velocidad- no se veria nunca. */
    cfg &= ~(USB_ULPI_SEL | USB_PHYSEL_FS);
    cfg &= ~(USB_SRPCAP | USB_HNPCAP);     /* nada de negociar el papel */
    cfg |=  USB_FORCEHOST;
    cfg &= ~USB_FORCEDEV;
    escribir(GUSBCFG, cfg);

    /* Forzar el modo tarda: el chip tiene que mirar las lineas y decidir, y
     * hasta 25 ms lo que uno lea no es definitivo. Es de las esperas que no se
     * pueden cambiar por un bucle mirando un bit, porque no hay bit. */
    sleep(3);

    /* DMA interno y rafagas de 16 palabras: es lo que GHWCFG2 dijo que este
     * ejemplar sabe hacer. Y la interrupcion global, otra vez abierta. */
    escribir(GAHBCFG, AHB_DMAEN | AHB_HBSTLEN(7) | AHB_GLBLINTR);

    /* Sin pedir ninguna interrupcion todavia: de momento se pregunta mirando.
     * Y los avisos viejos borrados -los W1C se limpian escribiendo unos-,
     * porque arrancar con avisos de antes del reset es leer noticias de ayer. */
    escribir(GINTMSK, 0);
    escribir(GINTSTS, 0xFFFFFFFF);

    /* El reloj de las lineas lentas: con 0 se le dice "30/60 MHz", que es lo
     * que toca cuando el PHY es UTMI+ de alta velocidad. */
    escribir(HCFG, leer(HCFG) & ~3u);
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
    sleep(2);                          /* que la alimentacion suba */
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

    /* --- 4. Y ahora si: encender el controlador --- */
    if (!nucleo_despertar()) {
        printf("  [usb] el nucleo no responde al reset (GRSTCTL = 0x%08x)\n",
               (unsigned int)leer(GRSTCTL));
        return 1;
    }

    modo_anfitrion();
    puerto_encender();

    uint32_t p = leer(HPRT0);
    printf("  [usb] nucleo en modo anfitrion; HPRT0 = 0x%08x\n",
           (unsigned int)p);

    if (!(p & HPRT_CONNSTS)) {
        /* En la Pi aqui hay un LAN9514 soldado, asi que esto no deberia pasar.
         * En QEMU si: la raspi3b trae el controlador pero no trae nada
         * enchufado a menos que se le diga. */
        printf("  [usb] no hay nada conectado al puerto raiz\n");
        for (;;) sleep(1000);
    }

    printf("  [usb] algo conectado, velocidad %s\n", velocidad(p));

    puerto_reset();
    p = leer(HPRT0);

    printf("  [usb] tras el reset: HPRT0 = 0x%08x, %s, puerto %s\n",
           (unsigned int)p, velocidad(p),
           (p & HPRT_ENA) ? "habilitado" : "SIN habilitar");

    if (p & HPRT_ENA)
        printf("  [usb] micro-trama %u: el bus esta vivo\n",
               (unsigned int)(leer(HFNUM) & 0x3FFF));

    /* Y aqui se para. Lo siguiente es hablar con el: un canal, una
     * transferencia de control y un GET_DESCRIPTOR, que es donde el hub dira
     * quien es. */
    for (;;) sleep(1000);
}
