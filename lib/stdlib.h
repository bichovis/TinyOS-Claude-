/* stdlib.h - El monton, la salida y conversiones */
#pragma once
#include <stddef.h>

void *malloc(size_t n);
void  free(void *p);

/* noreturn no es cosmetica: sin ella GCC cree que exit() puede volver, y
 * se queja de que un main que termina con exit() "llega al final de una
 * funcion que no es void". */
void  exit(int codigo) __attribute__((noreturn));

/* --- El entorno -------------------------------------------------------
 *
 * Un array de cadenas "NOMBRE=valor" terminado en cero, igual que argv.
 * La diferencia con argv no esta en la forma: esta en que argv lo pone
 * quien te arranca UNA vez, y el entorno se HEREDA hacia abajo sin que
 * nadie vuelva a escribirlo.
 *
 * Lo rellena crt0 antes de main, con lo que le dio el kernel. */
extern char **environ;

const char *getenv(const char *nombre);

/* Cambiar el entorno del proceso que llama. Eso NO cambia el de nadie
 * mas: ni el del padre, ni el de los hermanos. Un hijo se lleva una
 * copia, y a partir de ahi son dos cosas distintas.
 *
 * De ahi que "export" tenga que ser una orden interna del shell, por el
 * mismo motivo que "cd". */
int setenv(const char *nombre, const char *valor);

int   atoi(const char *s);
long  atol(const char *s);
int   abs(int v);
