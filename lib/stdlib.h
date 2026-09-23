/* stdlib.h - El monton, la salida y conversiones */
#pragma once
#include <stddef.h>

void *malloc(size_t n);
void  free(void *p);

/* Reservar n elementos de tam bytes, a cero. La multiplicacion se
 * comprueba: calloc(2, SIZE_MAX) no puede devolver un bloque de dos
 * bytes, que es lo que pasaria si se desbordara en silencio. */
void *calloc(size_t n, size_t tam);

/* Cambiar de tamanyo conservando el contenido. Puede mover el bloque, y
 * por eso devuelve la direccion nueva: usar la vieja despues es de los
 * errores mas viejos que hay. */
void *realloc(void *p, size_t n);

/* Ordenar. Es quicksort con la mediana de tres, que es lo que hace todo
 * el mundo y por una razon: el quicksort de libro se vuelve cuadratico
 * justo con lo que mas aparece en la vida real, que son datos ya
 * ordenados. */
void  qsort(void *base, size_t n, size_t tam,
            int (*comparar)(const void *, const void *));

void *bsearch(const void *clave, const void *base, size_t n, size_t tam,
              int (*comparar)(const void *, const void *));

/* De texto a numero, en la base que sea. 'fin' recibe donde se paro, que
 * es lo que permite leer varios numeros seguidos de una cadena. */
long          strtol(const char *s, char **fin, int base);
unsigned long strtoul(const char *s, char **fin, int base);

/* noreturn no es cosmetica: sin ella GCC cree que exit() puede volver, y
 * se queja de que un main que termina con exit() "llega al final de una
 * funcion que no es void". */
void  exit(int codigo) __attribute__((noreturn));

/* Salir SIN vaciar los buffers. Es lo que hay debajo de exit(), y lo que
 * usa el hijo de un fork() cuando no quiere reimprimir lo que el padre
 * dejo a medias en su cubo. */
void  _exit(int codigo) __attribute__((noreturn));

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
