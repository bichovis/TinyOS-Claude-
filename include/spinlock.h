/* spinlock.h - El cerrojo mas simple que funciona entre nucleos
 *
 * irq_save() NO es un cerrojo, y hasta ahora no habia forma de notarlo.
 * Tapar las interrupciones calla al nucleo propio; a los otros tres no les
 * dice absolutamente nada. Mientras uno se cree a solas en su seccion
 * critica, los demas entran por la puerta de al lado.
 *
 * Un cerrojo de verdad necesita una operacion que el hardware garantice
 * atomica. En ARM no existe un "test and set": existe un PAR de
 * instrucciones, y entre las dos el procesador vigila si alguien ha tocado
 * esa direccion.
 *
 *   ldaxr   carga y RESERVA la direccion
 *   stlxr   escribe solo si la reserva sigue viva, y dice si lo consiguio
 *   stlr    suelta
 *
 * Las dos letras del medio son lo que hace que el cerrojo proteja datos y
 * no solo a si mismo:
 *   la 'a' de ldaxr es acquire: nada de lo que venga despues puede
 *          adelantarse a esta lectura, asi que no se lee la seccion
 *          critica antes de tenerla.
 *   la 'l' de stlr es release: todo lo escrito dentro ya es visible
 *          cuando otro nucleo ve el cerrojo abierto.
 * Sin ellas, el procesador reordenaria los accesos y el cerrojo cerraria
 * una puerta con la pared abierta.
 */
#pragma once
#include <stdint.h>

struct spinlock {
    volatile uint32_t locked;
    const char       *name;        /* solo para depurar */
};

#define SPINLOCK(n)   { 0, (n) }

void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

/* Un cerrojo que tambien se coge desde un manejador de interrupciones hay
 * que cogerlo con las IRQ tapadas. Si no, a este mismo nucleo le puede
 * entrar una IRQ teniendolo, el manejador intentara cogerlo, y se quedara
 * dando vueltas esperando a alguien que no va a soltarlo nunca: el mismo.
 * Un interbloqueo de un solo nucleo consigo mismo.
 *
 * Por eso irq_save() no desaparece con la llegada de los cerrojos: pasa a
 * ser la mitad de uno. Y el orden importa: primero tapar, despues cerrar. */
uint64_t spin_lock_irqsave(struct spinlock *l);
void     spin_unlock_irqrestore(struct spinlock *l, uint64_t flags);
