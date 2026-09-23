/* irq.h - Controladores de interrupcion del BCM2837 */
#pragma once
#include <stdint.h>

void     irq_init(void);        /* lo llama el nucleo 0: los dos controladores */
void     irq_init_core(void);   /* y esto, CADA nucleo: lo suyo propio         */
void     irq_handle(void);      /* lo llama el vector IRQ desde vectors.S */
uint64_t irq_count(void);              /* atendidas entre los cuatro nucleos */
uint64_t irq_count_core(uint64_t core); /* y las de uno solo                 */

/* Habilitar / deshabilitar las IRQ en la propia CPU (registro DAIF).
 * irq_save/irq_restore anidan bien: sirven para secciones criticas. */
void     irq_enable(void);
void     irq_disable(void);
uint64_t irq_save(void);
void     irq_restore(uint64_t flags);
