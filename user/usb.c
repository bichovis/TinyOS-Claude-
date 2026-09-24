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

/* --- Registros globales del DWC2 -------------------------------------
 * Solo los de identidad. Los de control llegaran cuando haya algo que
 * controlar. */
#define GSNPSID    0x040    /* "quien soy": 0x4F54xxxx, o sea "OT" + version */
#define GHWCFG1    0x044
#define GHWCFG2    0x048    /* arquitectura y numero de canales              */
#define GHWCFG3    0x04C
#define GHWCFG4    0x050

static volatile uint32_t *reg;

static uint32_t leer(uint64_t off) { return reg[off / 4]; }

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

    printf("  [usb] DMA: %d paginas en VA 0x%lx -> PA 0x%lx\n",
           PAGINAS_DMA, (unsigned long)va, (unsigned long)pa);

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
    printf("  [usb] las %d paginas se escriben y se releen bien\n", PAGINAS_DMA);

    /* Uno por proceso: el segundo intento tiene que fallar, y decir por que. */
    uint64_t otra = 0;
    if (dma_alloc(4, &otra) >= 0)
        printf("  [usb] MAL: me ha dado un segundo tramo\n");
    else if (errno != EBUSY)
        printf("  [usb] MAL: el segundo tramo falla con %s, no con EBUSY\n",
               strerror(errno));
    else
        printf("  [usb] el segundo tramo dice EBUSY, como debe\n");

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
    printf("  [usb] IRQ %d reclamada en el puerto %ld\n",
           IRQ_USB, (long)puerto);

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
    else
        printf("  [usb] la IRQ de la UART ya tiene duenyo: no me la da\n");

    if (irq_register(29, (int)puerto) >= 0)
        printf("  [usb] MAL: me ha dado una IRQ que no esta en la lista\n");
    else
        printf("  [usb] una IRQ fuera de la lista: no me la da\n");

    /* Y aqui se queda, sin tocar un solo registro de control. Encender el
     * controlador sin saber apagarlo es como se cuelga una placa. */
    printf("  [usb] cimientos comprobados; el DWC2 sigue apagado\n\n");

    for (;;) sleep(1000);
}
