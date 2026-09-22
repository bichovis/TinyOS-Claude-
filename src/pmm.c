/* pmm.c - Gestor de memoria fisica (Physical Memory Manager)
 *
 * Reparte la RAM en paginas de 4 KB. La estructura es la mas simple que
 * existe: un mapa de bits con un bit por pagina, 0 = libre, 1 = ocupada.
 * Para 1 GB de RAM son 258048 bits = 31.5 KB de bitmap. Barato.
 *
 * Con esto ya se pueden pedir paginas para tablas de traduccion, pilas de
 * hilos o espacios de direcciones de procesos.
 */
#include <stdint.h>
#include "mm.h"
#include "uart.h"

/* Lo define linker.ld, y es una direccion VIRTUAL: el kernel esta enlazado
 * arriba. El PMM razona en fisico, asi que lo primero que hace con el es
 * bajarlo al mapa lineal.                                                */
extern char __kernel_end[];

/* El bitmap se dimensiona para el caso maximo; cuantas paginas se reparten
 * de verdad lo decide el limite que nos da la GPU en el arranque. */
#define MAX_PAGES   (RAM_MAX / PAGE_SIZE)          /* 258048 paginas */
#define BITMAP_WORDS (MAX_PAGES / 64)

static uint64_t bitmap[BITMAP_WORDS];
static uint64_t first_page;          /* primera pagina que podemos repartir */
static uint64_t total, used;
static uint64_t hint;                /* por donde seguir buscando           */

static inline void mark_used(uint64_t pfn) { bitmap[pfn / 64] |=  (1UL << (pfn % 64)); }
static inline void mark_free(uint64_t pfn) { bitmap[pfn / 64] &= ~(1UL << (pfn % 64)); }
static inline int  is_used(uint64_t pfn)   { return (bitmap[pfn / 64] >> (pfn % 64)) & 1; }

void pmm_init(uint64_t ram_limit)
{
    if (ram_limit == 0 || ram_limit > RAM_MAX)
        ram_limit = RAM_MAX;
    uint64_t last_page = ram_limit / PAGE_SIZE;

    /* Todo ocupado de entrada; luego liberamos lo que de verdad es nuestro. */
    for (uint64_t i = 0; i < BITMAP_WORDS; i++)
        bitmap[i] = ~0UL;

    /* La RAM libre empieza justo detras del kernel (codigo + datos + bss +
     * stack), ya alineado a 4 KB por el linker script, y llega hasta donde
     * empiezan los perifericos. Lo de debajo del kernel (vectores del
     * firmware, el propio kernel) no se toca. */
    first_page = virt_to_phys(__kernel_end) / PAGE_SIZE;

    total = 0;
    for (uint64_t pfn = first_page; pfn < last_page; pfn++) {
        mark_free(pfn);
        total++;
    }
    used = 0;
    hint = first_page;
}

uint64_t pmm_alloc(void)
{
    /* Busqueda circular a partir de la ultima asignacion. Ingenuo pero
     * suficiente; cuando duela lo cambiaremos por listas por orden. */
    for (uint64_t n = 0; n < MAX_PAGES; n++) {
        uint64_t pfn = hint + n;
        if (pfn >= MAX_PAGES) pfn -= (MAX_PAGES - first_page);
        if (pfn < first_page) continue;

        if (!is_used(pfn)) {
            mark_used(pfn);
            used++;
            hint = pfn + 1;

            /* Entregar paginas con basura dentro es una fuente inagotable de
             * bugs (y una fuga de informacion entre procesos). Se limpian. */
            uint64_t pa = pfn * PAGE_SIZE;
            uint64_t *p = phys_to_virt(pa);   /* el kernel no puede tocar */
                                              /* una direccion fisica     */
            for (uint64_t i = 0; i < PAGE_SIZE / 8; i++)
                p[i] = 0;
            return pa;
        }
    }
    return 0;                        /* sin memoria */
}

void pmm_free(uint64_t pa)
{
    uint64_t pfn = pa / PAGE_SIZE;
    if (pfn < first_page || pfn >= MAX_PAGES) return;
    if (!is_used(pfn)) return;       /* doble free: lo ignoramos */
    mark_free(pfn);
    used--;
}

uint64_t pmm_total_pages(void) { return total; }
uint64_t pmm_used_pages(void)  { return used;  }
uint64_t pmm_free_pages(void)  { return total - used; }
