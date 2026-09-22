/* irq.h - Controladores de interrupcion del BCM2837 */
#pragma once
#include <stdint.h>

void     irq_init(void);        /* configura ambos controladores        */
void     irq_handle(void);      /* lo llama el vector IRQ desde vectors.S */
uint64_t irq_count(void);       /* interrupciones atendidas en total    */

/* Habilitar / deshabilitar las IRQ en la propia CPU (registro DAIF).
 * irq_save/irq_restore anidan bien: sirven para secciones criticas. */
void     irq_enable(void);
void     irq_disable(void);
uint64_t irq_save(void);
void     irq_restore(uint64_t flags);
