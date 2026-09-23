/* stdio.h - Salida con formato
 *
 * printf no es magia: es un bucle sobre una cadena que, al encontrar un
 * '%', saca el siguiente argumento de la pila y lo convierte a texto. Lo
 * unico que no se puede escribir en C normal es "saca el siguiente
 * argumento", y para eso estan los va_ de <stdarg.h>, que si da el
 * compilador aunque no haya libc.
 */
#pragma once
#include <stdarg.h>
#include <stddef.h>

/* El atributo no es decoracion: hace que GCC compruebe que los % cuadran
 * con los argumentos. "%d" con un puntero pasa a ser un error de
 * compilacion en vez de un numero sin sentido en pantalla. */
int printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int snprintf(char *buf, size_t cap, const char *fmt, ...)
                                  __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
                                  __attribute__((format(printf, 3, 0)));

int putchar(int c);
int puts(const char *s);          /* anyade '\n', como manda el estandar */
int getchar(void);                /* -1 al final de la entrada */
