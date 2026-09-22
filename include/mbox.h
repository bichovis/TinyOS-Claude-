/* mbox.h - Buzon de mensajes con la GPU (VideoCore IV) */
#pragma once
#include <stdint.h>

/* Pregunta a la GPU cuanta RAM le ha dejado a la CPU.
 * Devuelve 1 si la respuesta es valida. */
int mbox_arm_memory(uint64_t *base, uint64_t *size);
