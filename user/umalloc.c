/* user/umalloc.c - El monton de un proceso
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

void *malloc(uint64_t n)
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

void free(void *p)
{
    if (!p) return;
    insertar((struct bloque *)((char *)p - CABECERA));
}
