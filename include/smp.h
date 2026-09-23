/* smp.h - Arranque de los nucleos 1, 2 y 3 */
#pragma once
#include <stdint.h>

#define CORES  4

/* Los despierta y espera a que den senyales de vida. Devuelve cuantos han
 * contestado (0..3). No se cuelga si alguno no lo hace. */
int  smp_start_secondaries(void);

/* Que dice cada nucleo de si mismo. */
void smp_dump(void);

/* Lo llama boot.S en cada nucleo secundario. No vuelve. */
void secondary_main(uint64_t core);
