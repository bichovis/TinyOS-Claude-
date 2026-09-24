/* mbox.h - Buzon de mensajes con la GPU (VideoCore IV) */
#pragma once
#include <stdint.h>

/* Pregunta a la GPU cuanta RAM le ha dejado a la CPU.
 * Devuelve 1 si la respuesta es valida. */
int mbox_arm_memory(uint64_t *base, uint64_t *size);

/* Frecuencia de un reloj de la placa, en Hz. 0 si la GPU no contesta.
 * Identificadores: 1 = EMMC, 2 = UART, 3 = ARM, 4 = core. */
uint32_t mbox_clock_rate(uint32_t clock_id);

/* Pedirle a la GPU que encienda un dispositivo y espere a que este listo.
 * Devuelve 1 si quedo encendido. Para el USB no es solo corriente: incluye
 * arrancar su PHY, y sin eso los registros funcionan y los paquetes no salen. */
uint32_t mbox_power_on(uint32_t device_id);   /* el estado, sin juzgarlo */
