/* uart.h - Consola serie (PL011 / UART0) */
#pragma once
#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
char uart_getc(void);
void uart_puts(const char *s);
void uart_hex32(uint32_t v);
void uart_hex64(uint64_t v);
void uart_hex8(uint8_t v);
