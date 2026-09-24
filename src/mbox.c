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
 * Y hay dos detalles que no perdonan, porque la GPU no es la CPU:
 *
 *   - La direccion que se le pasa es FISICA. La nuestra es virtual (el
 *     kernel vive en TTBR1), asi que hay que convertirla.
 *   - La GPU lee y escribe la RAM directamente, sin pasar por nuestra
 *     cache. Hay que bajarle el buffer a la RAM antes de avisarla, y tirar
 *     lo que tengamos cacheado de el despues, o leeriamos nuestra propia
 *     peticion en vez de su respuesta.
 */
#include <stdint.h>
#include "mbox.h"
#include "mmio.h"
#include "timer.h"
#include "mm.h"

#define MBOX_BASE     (PERIPHERAL_BASE + 0xB880)
#define MBOX_READ     (MBOX_BASE + 0x00)
#define MBOX_STATUS   (MBOX_BASE + 0x18)
#define MBOX_WRITE    (MBOX_BASE + 0x20)

#define MBOX_FULL     0x80000000u
#define MBOX_EMPTY    0x40000000u
#define MBOX_CH_PROP  8              /* canal de "property tags"          */

#define TAG_GET_ARM_MEMORY  0x00010005u
#define TAG_GET_CLOCK_RATE  0x00030002u
#define TAG_SET_POWER_STATE 0x00028001u
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

void dcache_clean_range(const void *addr, uint64_t bytes);       /* cache.S */
void dcache_invalidate_range(const void *addr, uint64_t bytes);  /* cache.S */

static int mbox_call(uint32_t channel)
{
    uint32_t addr = (uint32_t)virt_to_phys((const void *)buf);
    uint32_t msg  = (addr & ~0xFu) | (channel & 0xFu);
    uint64_t limit = timer_now() + (uint64_t)timer_hz() * MBOX_TIMEOUT_MS / 1000;

    /* Bajar el buffer a la RAM: la GPU no ve nuestra cache. El 'dsb sy' que
     * lleva dentro dcache_clean_range sirve ademas para lo otro que hace
     * falta aqui: que la escritura al buffer no se reordene por delante de
     * la escritura al registro del periferico. */
    dcache_clean_range((const void *)buf, sizeof(buf));

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
            /* Y simetricamente: tirar lo que tengamos cacheado del buffer,
             * porque lo que vale es lo que la GPU acaba de dejar en RAM. */
            dcache_invalidate_range((const void *)buf, sizeof(buf));
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

/* Cuanto va de rapido un reloj de la placa. Los identificadores los fija la
 * GPU: 1 = EMMC, 2 = UART, 3 = ARM, 4 = core.
 *
 * Esto hace falta porque el divisor de un periferico se calcula a partir de
 * su reloj base, y ese reloj depende de la configuracion de la placa. Darlo
 * por supuesto es lo que hace que un driver funcione en el emulador y no en
 * el hardware: el divisor sale de un numero inventado.
 */
/* --- Encender un dispositivo --------------------------------------------
 *
 * Y esto es mas que un interruptor de corriente. Lo que la VideoCore hace al
 * "encender" un periferico incluye poner en marcha su reloj y, en el caso del
 * USB, su PHY: el bloque analogico que hace la senyalizacion en el cable.
 *
 * De ahi que esto no se pueda deducir leyendo registros. Con el dominio a
 * medias, el AHB funciona -o sea que todos los registros del controlador se
 * leen y se escriben perfectamente, y hasta el DMA anda- pero el PHY solo da
 * para lo mas basico: ver si hay una resistencia en D+ y hablar a velocidad
 * completa. Los paquetes no salen y nada dice por que.
 *
 * Me costo cinco arranques descartar esto con un argumento malo: "los
 * registros se leen bien, asi que esta encendido". Leer registros solo prueba
 * que hay reloj de bus.
 *
 * El bit 1 del estado es "y espera a que este listo", que es justo lo que hace
 * falta: sin el, la llamada vuelve antes de que el PHY haya arrancado. */
#define PWR_ON        (1u << 0)
#define PWR_ESPERAR   (1u << 1)

uint32_t mbox_power_on(uint32_t device_id)
{
    buf[0] = 8 * 4;
    buf[1] = CODE_REQUEST;
    buf[2] = TAG_SET_POWER_STATE;
    buf[3] = 8;                      /* espacio para la respuesta         */
    buf[4] = 8;                      /* tamano de la peticion             */
    buf[5] = device_id;
    buf[6] = PWR_ON | PWR_ESPERAR;
    buf[7] = TAG_END;

    if (!mbox_call(MBOX_CH_PROP)) return 0xFFFFFFFFu;   /* ni contesto */

    /* Se devuelve el estado TAL CUAL, sin juzgarlo.
     *
     * Aqui tenia un "return (estado & ON) && !(estado & bit1)", dando por hecho
     * que el bit 1 de la respuesta significa "no existe ese dispositivo". La
     * placa contesto algo que ese juicio convirtio en "no esta encendido",
     * cuando a la vez el registro GUSBCFG del controlador CAMBIABA de valor,
     * o sea que la llamada estaba haciendo algo.
     *
     * Un booleano se equivoca en silencio; el numero se puede mirar. Cuando no
     * se esta seguro de como se interpreta una respuesta, lo que hay que
     * devolver es la respuesta. */
    return buf[6];
}

uint32_t mbox_clock_rate(uint32_t clock_id)
{
    buf[0] = 8 * 4;
    buf[1] = CODE_REQUEST;
    buf[2] = TAG_GET_CLOCK_RATE;
    buf[3] = 8;                      /* espacio para la respuesta         */
    buf[4] = 4;                      /* tamano de la peticion             */
    buf[5] = clock_id;
    buf[6] = 0;                      /* <- la GPU escribe aqui los Hz     */
    buf[7] = TAG_END;

    if (!mbox_call(MBOX_CH_PROP))
        return 0;

    return buf[6];
}
