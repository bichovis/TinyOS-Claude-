/* lib/malloc.c - El monton de un proceso
 *
 * Es el MISMO algoritmo que src/kheap.c: lista de trozos libres ordenada
 * por direccion, cabecera delante de cada uno, partir al reservar y fundir
 * al liberar. Merece la pena verlos juntos, porque lo unico que cambia
 * entre un asignador de kernel y uno de usuario es de donde sale la
 * memoria cuando se acaba:
 *
 *   el del kernel  se la pide al gestor de paginas   (pmm_alloc_contig)
 *   este           se la pide al kernel              (sbrk)
 *
 * Todo lo demas -el partir, el fundir, la fragmentacion que aparece si no
 * fundes- es identico, porque el problema es el mismo.
 */
#include "stdlib.h"
#include "syscall.h"

#define ALINEA      16
#define CRECE       (16 * 1024)      /* lo que se pide de una vez */

struct bloque {
    uint64_t       tam;              /* bytes TOTALES, cabecera incluida */
    struct bloque *sig;
};

#define CABECERA   (sizeof(struct bloque))
#define MINIMO     (CABECERA + ALINEA)

static struct bloque *libres;

static void insertar(struct bloque *b)
{
    struct bloque *prev = 0, *cur = libres;

    while (cur && cur < b) { prev = cur; cur = cur->sig; }

    b->sig = cur;
    if (prev) prev->sig = b;
    else      libres    = b;

    if (cur && (char *)b + b->tam == (char *)cur) {
        b->tam += cur->tam;
        b->sig  = cur->sig;
    }
    if (prev && (char *)prev + prev->tam == (char *)b) {
        prev->tam += b->tam;
        prev->sig  = b->sig;
    }
}

void *malloc(size_t n)
{
    if (!n) return 0;

    uint64_t necesita = (n + CABECERA + ALINEA - 1) & ~(uint64_t)(ALINEA - 1);
    if (necesita < MINIMO) necesita = MINIMO;

    for (int intento = 0; intento < 2; intento++) {
        struct bloque *prev = 0;

        for (struct bloque *b = libres; b; prev = b, b = b->sig) {
            if (b->tam < necesita) continue;

            if (b->tam - necesita >= MINIMO) {
                struct bloque *resto = (struct bloque *)((char *)b + necesita);
                resto->tam = b->tam - necesita;
                resto->sig = b->sig;
                b->tam     = necesita;
                if (prev) prev->sig = resto;
                else      libres    = resto;
            } else {
                if (prev) prev->sig = b->sig;
                else      libres    = b->sig;
            }
            return (char *)b + CABECERA;
        }

        if (intento) break;              /* ya lo intentamos una vez */

        /* No hay hueco: al kernel. */
        uint64_t pide = necesita > CRECE ? necesita : CRECE;
        void *nuevo = sbrk((int64_t)pide);
        if (!nuevo) return 0;

        struct bloque *b = nuevo;
        b->tam = pide;
        insertar(b);
    }
    return 0;
}

/* Cuanto sitio UTIL tiene un bloque ya reservado. La cabecera guarda el
 * tamanyo total, asi que es una resta. */
static uint64_t sitio_de(void *p)
{
    struct bloque *b = (struct bloque *)((char *)p - CABECERA);
    return b->tam - CABECERA;
}

void *calloc(size_t n, size_t tam)
{
    if (!n || !tam) return 0;

    /* La multiplicacion se comprueba ANTES. Sin esto, calloc(2, SIZE_MAX)
     * daria un bloque de dos bytes y el que llama escribiria en toda la
     * memoria creyendo que es suya. Es un fallo clasico y sigue
     * apareciendo. */
    if (n > (uint64_t)-1 / tam) return 0;

    uint64_t total = (uint64_t)n * tam;
    char *p = malloc(total);
    if (!p) return 0;

    for (uint64_t i = 0; i < total; i++) p[i] = 0;
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }

    uint64_t tenia = sitio_de(p);

    /* Si ya cabe, se queda donde esta. No se encoge el bloque: partirlo
     * complicaria el asignador para ahorrar unos bytes que casi siempre
     * se vuelven a pedir. */
    if (tenia >= n) return p;

    char *nuevo = malloc(n);
    if (!nuevo) return 0;                 /* y el viejo SIGUE VALIENDO */

    const char *viejo = p;
    for (uint64_t i = 0; i < tenia; i++) nuevo[i] = viejo[i];

    free(p);
    return nuevo;
}

void free(void *p)
{
    if (!p) return;
    insertar((struct bloque *)((char *)p - CABECERA));
}
