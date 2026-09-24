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
void uart_push(const char *buf, uint64_t n);
extern uint64_t uart_perdidos;       /* teclas tiradas por falta de sitio */                /* lo llama irq.c cuando salta la IRQ   */
int  uart_read(char *out);          /* saca un byte del buffer: 1=hay, 0=no */

/* Hasta n bytes de lo legible, durmiendo si no hay nada. 0 = se acabo la
 * entrada (Ctrl-D), -1 = lo interrumpio una senyal. */
int64_t uart_leer(char *dst, uint64_t n);

/* El modo del terminal: T_ECO, T_CANONICO. Devuelve el que habia; con -1
 * solo consulta. Ver la disciplina de linea en uart.c. */
int  uart_modo(int nuevo);

/* Tirar la entrada pendiente: la linea a medias y lo ya entregado. Lo
 * llaman Ctrl-C y Ctrl-Z. */
void uart_descartar_entrada(void);

/* --- Ceder la UART, o dejar de ser dos drivers -------------------------
 *
 * Cuando un proceso reclama la UART, el kernel deja de escribir en el
 * hardware y su texto se va a un anillo que ese proceso vacia. Es lo que
 * convierte dos escritores en uno, y sin eso los dos se pisan en la FIFO de
 * transmision y se pierden caracteres. Ver uart.c.
 *
 * Las dos las llama irq.c en el mismo sitio donde cambia de manos el
 * teclado: es un solo dispositivo y se entrega entero. */
void     uart_ceder(void);
void     uart_recuperar(void);
void     uart_panico_toma_el_mando(void);   /* lo llama panic() */

/* Texto de un proceso. A diferencia del del kernel NO se puede perder, asi
 * que si el anillo se llena esta espera a que lo vacien: control de flujo.
 * Devuelve los bytes aceptados. */
int64_t  uart_escribir_texto(const char *s, uint64_t n);

uint64_t uart_klog_hay(void);               /* bytes esperando */
uint64_t uart_klog_perdidos(void);          /* los que no cupieron */
uint64_t uart_klog_saca(char *dst, uint64_t n);
