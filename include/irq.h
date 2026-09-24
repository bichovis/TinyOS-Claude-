/* irq.h - Controladores de interrupcion del BCM2837 */
#pragma once
#include <stdint.h>

void     irq_init(void);        /* lo llama el nucleo 0: los dos controladores */
void     irq_init_core(void);   /* y esto, CADA nucleo: lo suyo propio         */
void     irq_send_resched(uint64_t core);  /* "mirate el turno" a otro nucleo */

/* Pedir que una interrupcion se convierta en mensajes a un puerto. */
int      irq_register(uint64_t irq, int puerto);
int      irq_ack(uint64_t irq);
void     irq_release_port(int puerto);

/* ¿Es ese proceso el duenyo de esa interrupcion? Lo usa la llamada que saca
 * el texto del kernel: solo el duenyo de la UART puede leerlo. */
int      irq_es_duenyo(uint64_t irq, uint64_t pid);
extern uint64_t irq_avisos_perdidos;

/* Avisar al duenyo de la consola de que el kernel tiene texto que sacar.
 * Lo llama el tick, y tambien quien acaba de meter teclas: el eco no puede
 * esperar diez milisegundos. Pide sched_lock, asi que NUNCA con el cerrojo
 * de la UART cogido. */
void     klog_avisar(void);
void     klog_entregado(void);   /* el anillo se ha quedado vacio: proximo texto, aviso nuevo */
void     irq_handle(void);      /* lo llama el vector IRQ desde vectors.S */
uint64_t irq_count(void);              /* atendidas entre los cuatro nucleos */
uint64_t irq_count_core(uint64_t core); /* y las de uno solo                 */

/* Habilitar / deshabilitar las IRQ en la propia CPU (registro DAIF).
 * irq_save/irq_restore anidan bien: sirven para secciones criticas. */
void     irq_enable(void);
void     irq_disable(void);
uint64_t irq_save(void);
void     irq_restore(uint64_t flags);
