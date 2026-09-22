/* uart.h - Consola serie (PL011 / UART0) */
#pragma once
#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
char uart_getc(void);               /* bloqueante, por polling */
void uart_puts(const char *s);
void uart_hex8(uint8_t v);
void uart_hex32(uint32_t v);
void uart_hex64(uint64_t v);
void uart_dec(uint64_t v);

/* --- Recepcion por interrupcion --------------------------------------- */
void uart_enable_rx_irq(void);      /* pide a la PL011 que avise al recibir */
void uart_irq(void);                /* lo llama irq.c cuando salta la IRQ   */
int  uart_read(char *out);          /* saca un byte del buffer: 1=hay, 0=no */
char uart_getc_blocking(void);      /* duerme el hilo hasta que llegue algo */
