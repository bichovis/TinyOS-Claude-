/* mbox.c - El buzon de la VideoCore
 *
 * En la Raspberry Pi la CPU ARM es el perifero y la GPU es quien manda:
 * ella arranca la maquina, lee la SD y decide cuanta RAM nos deja. La unica
 * forma de preguntarle cosas es este buzon.
 *
 * El protocolo ("property interface", canal 8) es: se prepara un buffer
 * alineado a 16 bytes con una lista de etiquetas, se escribe su direccion
 * en el registro de escritura y se espera a que la GPU conteste rellenando
 * el mismo buffer.
 *
 * IMPORTANTE: esto solo se puede usar con las caches apagadas. La GPU lee
 * el buffer directamente de la RAM y no ve la cache de la CPU; con la MMU
 * encendida habria que reservarlo como memoria no cacheable.
 */
#include <stdint.h>
#include "mbox.h"
#include "mmio.h"
#include "timer.h"

#define MBOX_BASE     (PERIPHERAL_BASE + 0xB880)
#define MBOX_READ     (MBOX_BASE + 0x00)
#define MBOX_STATUS   (MBOX_BASE + 0x18)
#define MBOX_WRITE    (MBOX_BASE + 0x20)

#define MBOX_FULL     0x80000000u
#define MBOX_EMPTY    0x40000000u
#define MBOX_CH_PROP  8              /* canal de "property tags"          */

#define TAG_GET_ARM_MEMORY  0x00010005u
#define TAG_END             0x00000000u
#define CODE_REQUEST        0x00000000u
#define CODE_RESP_OK        0x80000000u

/* El buffer tiene que estar alineado a 16 bytes: los 4 bits bajos de la
 * direccion se usan para meter el numero de canal. */
static volatile uint32_t buf[16] __attribute__((aligned(16)));

/* La GPU contesta en microsegundos, pero si algo va mal (un emulador que no
 * implementa el buzon, un firmware al que no le gusta la peticion) no
 * podemos quedarnos esperando para siempre: eso seria colgar el arranque
 * entero. Nos damos 100 ms y nos rendimos.                              */
#define MBOX_TIMEOUT_MS  100

static int mbox_call(uint32_t channel)
{
    uint32_t addr = (uint32_t)(uint64_t)buf;
    uint32_t msg  = (addr & ~0xFu) | (channel & 0xFu);
    uint64_t limit = timer_now() + (uint64_t)timer_hz() * MBOX_TIMEOUT_MS / 1000;

    /* Que los datos del buffer esten de verdad en RAM ANTES de tocar el
     * timbre. Las caches estan apagadas, pero eso no basta: el procesador
     * puede reordenar una escritura a memoria normal respecto de una
     * escritura a un periferico. 'dsb sy' espera a que se hayan completado
     * todas las anteriores.                                              */
    __asm__ volatile("dsb sy" ::: "memory");

    while (mmio_read(MBOX_STATUS) & MBOX_FULL) {
        if (timer_now() > limit)
            return 0;
    }
    mmio_write(MBOX_WRITE, msg);

    for (;;) {
        while (mmio_read(MBOX_STATUS) & MBOX_EMPTY) {
            if (timer_now() > limit)
                return 0;
        }
        /* El buzon es compartido: puede llegar respuesta de otro canal. */
        if (mmio_read(MBOX_READ) == msg) {
            /* Y simetricamente: no leer el buffer hasta estar seguros de
             * que lo que escribio la GPU ya es visible.                  */
            __asm__ volatile("dsb sy" ::: "memory");
            return buf[1] == CODE_RESP_OK;
        }
        if (timer_now() > limit)
            return 0;
    }
}

int mbox_arm_memory(uint64_t *base, uint64_t *size)
{
    buf[0] = 8 * 4;                  /* tamano total del mensaje          */
    buf[1] = CODE_REQUEST;
    buf[2] = TAG_GET_ARM_MEMORY;
    buf[3] = 8;                      /* espacio para la respuesta         */
    buf[4] = 0;                      /* tamano de la peticion (ninguna)   */
    buf[5] = 0;                      /* <- la GPU escribe aqui la base    */
    buf[6] = 0;                      /* <- y aqui el tamano               */
    buf[7] = TAG_END;

    if (!mbox_call(MBOX_CH_PROP))
        return 0;

    *base = buf[5];
    *size = buf[6];
    return (*size != 0);
}
