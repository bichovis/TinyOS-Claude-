/* timer.h - Temporizador generico de ARM (per-core) */
#pragma once
#include <stdint.h>

void     timer_init(uint32_t hz);   /* arranca el tick periodico           */
void     timer_irq(void);           /* lo llama irq.c al saltar la IRQ     */
uint64_t timer_ticks(void);         /* ticks desde el arranque             */
uint64_t timer_uptime_ms(void);     /* milisegundos desde el arranque      */
uint32_t timer_hz(void);            /* frecuencia del contador, en Hz      */
void     delay_ms(uint32_t ms);     /* espera activa precisa               */
uint64_t timer_now(void);           /* CNTPCT_EL0 en crudo, para medir     */
uint64_t timer_us(uint64_t cycles); /* convierte cuentas a microsegundos   */
