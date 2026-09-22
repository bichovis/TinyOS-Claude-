/* exception.h - Contexto guardado al entrar en una excepcion */
#pragma once
#include <stdint.h>

/* IMPORTANTE: este struct describe byte a byte lo que apila kernel_entry
 * en vectors.S. Si tocas uno, tienes que tocar el otro. */
struct trap_frame {
    uint64_t x[30];   /* offset   0 : x0 .. x29                        */
    uint64_t lr;      /* offset 240 : x30, direccion de retorno         */
    uint64_t elr;     /* offset 248 : ELR_EL1,  PC interrumpido         */
    uint64_t spsr;    /* offset 256 : SPSR_EL1, estado interrumpido     */
    uint64_t esr;     /* offset 264 : ESR_EL1,  causa de la excepcion   */
};                    /* total: 272 bytes                               */

_Static_assert(sizeof(struct trap_frame) == 272, "trap_frame != kernel_entry");

void exception_init(void);
void exception_dispatch(struct trap_frame *f, uint64_t index);
void panic(const char *msg);
