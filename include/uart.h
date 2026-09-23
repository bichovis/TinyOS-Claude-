/* uart.h - Consola serie (PL011 / UART0) */
#pragma once
#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
char uart_getc(void);               /* bloqueante, por polling */
void uart_puts(const char *s);

/* --- Lineas enteras ----------------------------------------------------
 * El cerrojo de la UART hace indivisible cada LLAMADA, y una linea suelen
 * ser varias: un uart_puts seguido de un uart_dec se puede partir por la
 * mitad si otro nucleo escribe en medio. Este par hace indivisible el
 * grupo entero.
 *
 * Es reentrante a proposito: las llamadas de dentro vuelven a pedir el
 * cerrojo y no se bloquean contra su propio grupo.
 *
 * Y es un spinlock, no el mutex de consola, porque tiene que poder usarse
 * desde sitios donde no se puede dormir: dentro de un manejador de
 * interrupcion, o teniendo cogido sched_lock -que es lo que hace
 * sched_dump()-. Un mutex ahi seria un interbloqueo. */
uint64_t uart_begin(void);
void     uart_end(uint64_t flags);
void uart_hex8(uint8_t v);
void uart_hex32(uint32_t v);
void uart_hex64(uint64_t v);
void uart_dec(uint64_t v);

/* --- Recepcion por interrupcion --------------------------------------- */
void uart_enable_rx_irq(void);      /* pide a la PL011 que avise al recibir */
void uart_irq(void);
void uart_push(const char *buf, uint64_t n);                /* lo llama irq.c cuando salta la IRQ   */
int  uart_read(char *out);          /* saca un byte del buffer: 1=hay, 0=no */
int  uart_getc_blocking(void);   /* -1 si lo interrumpe una senyal */      /* duerme el hilo hasta que llegue algo */
