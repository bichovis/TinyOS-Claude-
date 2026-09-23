/* stdlib.h - El monton, la salida y conversiones */
#pragma once
#include <stddef.h>

void *malloc(size_t n);
void  free(void *p);

/* noreturn no es cosmetica: sin ella GCC cree que exit() puede volver, y
 * se queja de que un main que termina con exit() "llega al final de una
 * funcion que no es void". */
void  exit(int codigo) __attribute__((noreturn));

int   atoi(const char *s);
long  atol(const char *s);
int   abs(int v);
