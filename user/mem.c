/* user/mem.c - Ver el monton de un proceso por dentro
 *
 * Ensenya la diferencia entre las dos capas, que es facil de confundir:
 *
 *   malloc/free  reparten memoria DENTRO de lo que el proceso ya tiene
 *   sbrk         es lo unico que le pide mas al kernel
 *
 * Por eso free() no devuelve nada al sistema: suelta el trozo en la lista
 * del proceso, para que el siguiente malloc lo reaproveche. El tope no
 * baja. Y esta bien que no baje: devolverlo para volver a pedirlo en dos
 * lineas serian dos llamadas al sistema tiradas.
 */
#include "syscall.h"

#define N  64

static void *trozos[N];

static void num(const char *antes, uint64_t v, const char *despues)
{
    char b[24];
    uint64_t n = udec(b, v);
    b[n] = 0;
    kprint(antes); kprint(b); kprint(despues);
}

static void tope(const char *que)
{
    num("  ", (uint64_t)sbrk(0), "");
    kprint("   ");
    kprint(que);
    kprint("\n");
}

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    (void)argc; (void)argv;

    kprint("\n  tope del monton (sbrk)   que acaba de pasar\n");
    tope("al empezar: el monton esta vacio");

    /* 1. Muchos bloques pequenyos. El tope sube a saltos, no bloque a
     *    bloque: malloc pide al kernel de 16 KB en 16 KB. */
    for (int i = 0; i < N; i++) trozos[i] = malloc(1024);
    tope("despues de 64 bloques de 1 KB");

    /* 2. Soltarlos todos. El tope NO baja: la memoria vuelve a la lista
     *    del proceso, no al kernel. */
    for (int i = 0; i < N; i++) free(trozos[i]);
    tope("despues de soltarlos: no baja, y es lo correcto");

    /* 3. Y ahora uno grande. Si el asignador ha fundido bien los 64 huecos
     *    de antes, cabe sin pedirle nada al kernel. */
    void *grande = malloc(48 * 1024);
    tope(grande ? "despues de pedir 48 KB de golpe" : "no ha habido 48 KB");

    if (grande) {
        unsigned char *b = grande;
        for (uint64_t i = 0; i < 48 * 1024; i++) b[i] = (unsigned char)i;
        for (uint64_t i = 0; i < 48 * 1024; i++)
            if (b[i] != (unsigned char)i) {
                kprint("  los 48 KB no son mios: ALGO VA MAL\n");
                exit(1);
            }
        kprint("  los 48 KB se escriben y se releen bien\n");
        free(grande);
    }

    /* 4. Y devolverle al kernel lo que ya no hace falta. */
    void *antes = sbrk(0);
    sbrk(-16 * 1024);
    if (sbrk(0) < antes) kprint("  sbrk negativo: el tope baja y las paginas vuelven\n");

    exit(0);
}
