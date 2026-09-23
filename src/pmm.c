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
#include "spinlock.h"
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

/* El bitmap es de los cuatro nucleos. Dos 'pmm_alloc' a la vez sin cerrojo
 * pueden ver el mismo bit libre y entregar la misma pagina dos veces: el
 * peor error que puede cometer un gestor de memoria, porque no falla aqui
 * sino mucho despues y en otro sitio. */
static struct spinlock pmm_lock = SPINLOCK("pmm");

/* Cuantos la estan usando.
 *
 * Hasta ahora una pagina tenia un duenyo y punto, asi que sobraba. Con el
 * copy-on-write puede tener varios: padre e hijo comparten las mismas
 * paginas hasta que uno escribe. Liberar deja de significar "devuelvela" y
 * pasa a significar "yo ya no la uso"; solo vuelve al bitmap cuando no la
 * usa nadie.
 *
 * Un byte por pagina son 258 KB de .bss para el mapa entero. Es mucho para
 * lo que hace, y la alternativa -una estructura dispersa con solo las
 * paginas compartidas- es bastante mas codigo para ahorrar memoria que en
 * esta placa sobra. */
static uint8_t refs[MAX_PAGES];
static uint64_t first_page;          /* primera pagina que podemos repartir */
static uint64_t total, used;
static uint64_t hint;                /* por donde seguir buscando           */

static inline void mark_used(uint64_t pfn) { bitmap[pfn / 64] |=  (1UL << (pfn % 64)); }
static inline void mark_free(uint64_t pfn) { bitmap[pfn / 64] &= ~(1UL << (pfn % 64)); }
static inline int  is_used(uint64_t pfn)   { return (bitmap[pfn / 64] >> (pfn % 64)) & 1; }

static uint64_t last_page;       /* la primera que ya no existe */

void pmm_init(uint64_t ram_limit)
{
    if (ram_limit == 0 || ram_limit > RAM_MAX)
        ram_limit = RAM_MAX;
    last_page = ram_limit / PAGE_SIZE;

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
    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    /* Busqueda circular a partir de la ultima asignacion. Ingenuo pero
     * suficiente; cuando duela lo cambiaremos por listas por orden. */
    for (uint64_t n = 0; n < MAX_PAGES; n++) {
        uint64_t pfn = hint + n;
        if (pfn >= MAX_PAGES) pfn -= (MAX_PAGES - first_page);
        if (pfn < first_page) continue;

        if (!is_used(pfn)) {
            mark_used(pfn);
            refs[pfn] = 1;
            used++;
            hint = pfn + 1;

            /* Entregar paginas con basura dentro es una fuente inagotable de
             * bugs (y una fuga de informacion entre procesos). Se limpian. */
            uint64_t pa = pfn * PAGE_SIZE;
            spin_unlock_irqrestore(&pmm_lock, flags);

            /* El borrado, ya fuera del cerrojo: la pagina es nuestra y de
             * nadie mas, y son 4 KB que no hay por que hacer esperar a los
             * otros tres nucleos. */
            uint64_t *p = phys_to_virt(pa);   /* el kernel no puede tocar */
                                              /* una direccion fisica     */
            for (uint64_t i = 0; i < PAGE_SIZE / 8; i++)
                p[i] = 0;
            return pa;
        }
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0;                        /* sin memoria */
}

/* Apuntarse como usuario de una pagina que ya existe. */
void pmm_ref(uint64_t pa)
{
    uint64_t pfn = pa / PAGE_SIZE;
    if (pfn < first_page || pfn >= MAX_PAGES) return;

    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    if (is_used(pfn) && refs[pfn] < 255) refs[pfn]++;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_refs(uint64_t pa)
{
    uint64_t pfn = pa / PAGE_SIZE;
    if (pfn < first_page || pfn >= MAX_PAGES) return 0;

    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint64_t n = refs[pfn];
    spin_unlock_irqrestore(&pmm_lock, flags);
    return n;
}

/* "Yo ya no la uso". Solo vuelve al bitmap cuando no la usa nadie mas. */
void pmm_free(uint64_t pa)
{
    uint64_t pfn = pa / PAGE_SIZE;
    if (pfn < first_page || pfn >= MAX_PAGES) return;

    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    if (is_used(pfn)) {              /* si no, es un doble free: lo ignoramos */
        if (refs[pfn] > 1) {
            refs[pfn]--;             /* queda gente usandola */
        } else {
            refs[pfn] = 0;
            mark_free(pfn);
            used--;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_total_pages(void) { return total; }
uint64_t pmm_used_pages(void)  { return used;  }
uint64_t pmm_free_pages(void)  { return total - used; }

/* --- Paginas contiguas ------------------------------------------------
 *
 * pmm_alloc() reparte de una en una y no promete nada sobre donde caen.
 * Para el monton del kernel hace falta otra cosa: un trozo seguido, porque
 * un objeto de 6 KB tiene que caber entero en direcciones consecutivas.
 *
 * La busqueda es tonta a proposito -recorre el bitmap mirando si hay n
 * huecos seguidos- y eso la hace lenta cuando la memoria esta fragmentada.
 * Se puede permitir porque solo se llama cuando el monton se queda corto,
 * que con trozos de 64 KB es muy de vez en cuando. Si algun dia duele, lo
 * que hay que traer es un asignador por compañeros ("buddy").
 */
uint64_t pmm_alloc_contig(uint64_t n)
{
    if (n == 0) return 0;

    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    for (uint64_t inicio = first_page; inicio + n <= last_page; inicio++) {
        uint64_t i = 0;
        while (i < n && !is_used(inicio + i)) i++;

        if (i < n) {                       /* topamos con una ocupada */
            inicio += i;                   /* y saltamos hasta ella */
            continue;
        }

        for (uint64_t k = 0; k < n; k++) { mark_used(inicio + k); refs[inicio + k] = 1; }
        used += n;

        uint64_t pa = inicio * PAGE_SIZE;
        spin_unlock_irqrestore(&pmm_lock, flags);

        /* El borrado, fuera del cerrojo: ya son nuestras. */
        uint64_t *p = phys_to_virt(pa);
        for (uint64_t i2 = 0; i2 < n * PAGE_SIZE / 8; i2++) p[i2] = 0;
        return pa;
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0;
}

void pmm_free_contig(uint64_t pa, uint64_t n)
{
    uint64_t pfn = pa / PAGE_SIZE;

    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    for (uint64_t k = 0; k < n; k++) {
        if (pfn + k < first_page || pfn + k >= last_page) continue;
        if (!is_used(pfn + k)) continue;
        if (refs[pfn + k] > 1) { refs[pfn + k]--; continue; }
        refs[pfn + k] = 0;
        mark_free(pfn + k);
        used--;
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}
