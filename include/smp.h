/* smp.h - Arranque de los nucleos 1, 2 y 3 */
#pragma once
#include <stdint.h>

#define CORES  4

/* En que nucleo estamos. MPIDR_EL1 es de solo lectura y lo pone el
 * hardware: es la unica cosa distinta que ve cada nucleo del mismo codigo. */
static inline uint64_t this_core(void)
{
    uint64_t m;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(m));
    return m & 0xFF;
}

/* Los despierta y espera a que den senyales de vida. Devuelve cuantos han
 * contestado (0..3). No se cuelga si alguno no lo hace. */
int  smp_start_secondaries(void);

/* Que dice cada nucleo de si mismo. */
void smp_dump(void);

/* Lo llama boot.S en cada nucleo secundario. No vuelve. */
void secondary_main(uint64_t core);

/* Pone a los CUATRO nucleos a incrementar el mismo contador, 'iters' veces
 * cada uno. Devuelve lo que vale al final: con cerrojo tiene que ser
 * exactamente 4*iters, y sin el, menos. Es la demostracion de que
 * irq_save() no bastaba. */
uint64_t smp_hammer(uint64_t iters, int con_cerrojo);
