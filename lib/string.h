/* string.h - Memoria y cadenas
 *
 * La mitad de estas funciones no estan aqui solo por comodidad: el
 * COMPILADOR las llama por su cuenta. Una asignacion de structs, un array
 * inicializado a ceros o un bucle de copia que GCC reconoce se convierten
 * en llamadas a memcpy o a memset sin que aparezcan en el codigo fuente.
 * Sin ellas, el programa compila y no enlaza -o peor, enlaza y no funciona-.
 */
#pragma once
#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
