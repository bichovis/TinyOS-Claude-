/* kheap.c - El monton del kernel
 *
 * Hasta aqui el kernel solo sabia repartir paginas de 4 KB. Todo lo que
 * necesitaba otro tamanyo se declaraba como un array estatico y con eso se
 * fijaba un limite para siempre: MAX_TASKS, MAX_PORTS, MAX_ARGS. Esto es
 * lo que quita esos techos.
 *
 * El diseño es el de toda la vida: una lista de los trozos libres, un
 * poquito de cabecera delante de cada trozo, y dos operaciones que son la
 * una la inversa de la otra.
 *
 *   kmalloc  busca el primer hueco donde quepa, y si sobra mucho lo parte
 *   kfree    lo devuelve a la lista y lo FUNDE con sus vecinos
 *
 * Fundir es la mitad que se olvida, y es la que decide si el monton dura.
 * Sin fundir, cada pareja de reserva y liberacion deja la lista un poco
 * mas picada: al cabo de un rato hay memoria libre de sobra pero ningun
 * hueco lo bastante grande. Eso es la fragmentacion, y es una forma de
 * quedarse sin memoria teniendola.
 *
 * Por eso la lista esta ordenada POR DIRECCION y no por tamanyo: asi los
 * vecinos en memoria son vecinos en la lista, y fundirlos es mirar si el
 * de al lado empieza justo donde acaba este.
 */
#include <stdint.h>
#include "mm.h"
#include "spinlock.h"
#include "uart.h"

/* Todo se alinea a 16 porque el ABI de AArch64 lo pide para los tipos mas
 * exigentes, y porque la pila tambien va asi. */
#define ALINEA      16
#define CHUNK_PAGS  16           /* 64 KB cada vez que hay que pedir mas */

struct bloque {
    uint64_t       tam;          /* bytes TOTALES, cabecera incluida     */
    struct bloque *sig;          /* solo significa algo si esta libre    */
};

#define CABECERA   (sizeof(struct bloque))
#define MINIMO     (CABECERA + ALINEA)

static struct bloque  *libres;
static struct spinlock heap_lock = SPINLOCK("heap");
static uint64_t        total, usado;

/* Meter un bloque en la lista, en su sitio, y fundirlo con quien toque.
 * Se llama con el cerrojo cogido. */
static void insertar(struct bloque *b)
{
    struct bloque *prev = 0, *cur = libres;

    while (cur && cur < b) { prev = cur; cur = cur->sig; }

    b->sig = cur;
    if (prev) prev->sig = b;
    else      libres    = b;

    /* Con el de despues. La comprobacion es literal: ¿empieza justo donde
     * acabo yo? Si no, son de trozos distintos del monton y fundirlos
     * seria inventarse memoria que no existe. */
    if (cur && (char *)b + b->tam == (char *)cur) {
        b->tam += cur->tam;
        b->sig  = cur->sig;
    }

    /* Y con el de antes. */
    if (prev && (char *)prev + prev->tam == (char *)b) {
        prev->tam += b->tam;
        prev->sig  = b->sig;
    }
}

/* Pedirle mas memoria al gestor de paginas. Con el cerrojo cogido. */
static int crecer(uint64_t falta)
{
    uint64_t pags = (falta + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pags < CHUNK_PAGS) pags = CHUNK_PAGS;

    uint64_t pa = pmm_alloc_contig(pags);
    if (!pa) return -1;

    struct bloque *b = phys_to_virt(pa);
    b->tam = pags * PAGE_SIZE;
    total += b->tam;
    insertar(b);
    return 0;
}

/* Primer hueco donde quepa. Con el cerrojo cogido. */
static void *tomar(uint64_t necesita)
{
    struct bloque *prev = 0;

    for (struct bloque *b = libres; b; prev = b, b = b->sig) {
        if (b->tam < necesita) continue;

        if (b->tam - necesita >= MINIMO) {
            /* Sobra bastante: se parte y la cola sigue libre. Partir es lo
             * que evita gastar un bloque de 64 KB en una peticion de 32
             * bytes. */
            struct bloque *resto = (struct bloque *)((char *)b + necesita);
            resto->tam = b->tam - necesita;
            resto->sig = b->sig;
            b->tam     = necesita;

            if (prev) prev->sig = resto;
            else      libres    = resto;
        } else {
            /* No merece la pena partir: se entrega entero, con su pico de
             * mas. Ese desperdicio tiene nombre, fragmentacion interna, y
             * es el precio de no llenar la lista de migajas inservibles. */
            if (prev) prev->sig = b->sig;
            else      libres    = b->sig;
        }

        usado += b->tam;
        return (char *)b + CABECERA;
    }
    return 0;
}

void *kmalloc(uint64_t n)
{
    if (!n) return 0;

    uint64_t necesita = (n + CABECERA + ALINEA - 1) & ~(uint64_t)(ALINEA - 1);
    if (necesita < MINIMO) necesita = MINIMO;

    uint64_t flags = spin_lock_irqsave(&heap_lock);

    void *p = tomar(necesita);
    if (!p && crecer(necesita) == 0)
        p = tomar(necesita);

    spin_unlock_irqrestore(&heap_lock, flags);
    return p;
}

void kfree(void *p)
{
    if (!p) return;

    struct bloque *b = (struct bloque *)((char *)p - CABECERA);

    uint64_t flags = spin_lock_irqsave(&heap_lock);
    usado -= b->tam;
    insertar(b);
    spin_unlock_irqrestore(&heap_lock, flags);
}

void kheap_stats(uint64_t *tot, uint64_t *uso, uint64_t *huecos,
                 uint64_t *mayor)
{
    uint64_t flags = spin_lock_irqsave(&heap_lock);

    uint64_t n = 0, max = 0;
    for (struct bloque *b = libres; b; b = b->sig) {
        n++;
        if (b->tam > max) max = b->tam;
    }

    if (tot)    *tot    = total;
    if (uso)    *uso    = usado;
    if (huecos) *huecos = n;
    if (mayor)  *mayor  = max;

    spin_unlock_irqrestore(&heap_lock, flags);
}
