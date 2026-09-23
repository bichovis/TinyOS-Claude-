/* string.c - Memoria y cadenas
 *
 * UNA PRECAUCION, y conviene contarla bien porque es facil exagerarla.
 *
 * GCC tiene una optimizacion -ftree-loop-distribute-patterns que reconoce
 * un bucle que copia byte a byte y lo sustituye por una llamada a memcpy,
 * porque la de la libc suele estar en ensamblador y ser mas rapida.
 * Aplicada AQUI convertiria el cuerpo de memcpy en una llamada a memcpy:
 * recursion infinita, pila desbordada, y un programa que revienta en un
 * sitio sin ninguna relacion con lo que estaba haciendo. El codigo fuente
 * es obviamente correcto y el desensamblado no se parece en nada.
 *
 * Aqui NO pasa, y merece la pena saber por que: -ffreestanding le dice a
 * GCC que no de por hecha la biblioteca estandar, y eso ya desactiva la
 * transformacion. Se comprueba desensamblando sin el flag -memcpy, memset
 * y strcpy salen como bucles, sin un solo bl-.
 *
 * El fichero se compila igualmente con -fno-tree-loop-distribute-patterns.
 * No porque haga falta hoy, sino porque no queremos que la correccion de
 * memcpy dependa de un efecto secundario de otro flag: el dia que alguien
 * quite -ffreestanding, esto seguira funcionando.
 */
#include "string.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

/* La diferencia con memcpy es que los dos trozos pueden solaparse, y
 * entonces el sentido en que se copia importa: hacia adelante se pisaria
 * la fuente antes de haberla leido. */
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;

    if (d == s || n == 0) return dst;

    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src)
{
    size_t i = 0;
    while ((dst[i] = src[i])) i++;
    return dst;
}

/* Ojo con esta: si no cabe, NO pone el cero final. Es como esta definida
 * desde 1979 y ha costado muchos disgustos, pero cambiarla aqui seria
 * peor: el que la use esperara la de siempre. */
char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : 0;
}

char *strrchr(const char *s, int c)
{
    const char *ultimo = 0;
    for (; *s; s++) if (*s == (char)c) ultimo = s;
    if (c == 0) return (char *)s;
    return (char *)ultimo;
}
