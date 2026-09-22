/* vmm.c - Tablas de traduccion y encendido de la MMU
 *
 * Configuracion: paginas de 4 KB, 39 bits de direccion virtual (512 GB por
 * espacio). Con 39 bits la traduccion empieza en L1, asi que tenemos tres
 * niveles en vez de cuatro:
 *
 *   L1  bits [38:30]  512 entradas de 1 GB
 *   L2  bits [29:21]  512 entradas de 2 MB
 *   L3  bits [20:12]  512 entradas de 4 KB
 *
 * El kernel se mapea LINEAL (VA = PA + KERNEL_VA_BASE) con bloques de 2 MB:
 * cubrir 1 GB cuesta 512 entradas en vez de 262144 paginas.
 *
 * Las tablas del kernel y el encendido de la MMU ya no estan en este
 * fichero: ocurren en boot.S, porque el kernel esta enlazado arriba y sin
 * MMU no puede ejecutar ni una linea de C. Lo que queda aqui es todo lo
 * demas: mapear paginas sueltas, crear y destruir espacios de usuario, y
 * preguntarle traducciones al hardware.
 */
#include <stdint.h>
#include "mm.h"
#include "irq.h"
#include "uart.h"

#define VA_BITS         39

#define L1_INDEX(va)    (((va) >> 30) & 0x1FF)
#define L2_INDEX(va)    (((va) >> 21) & 0x1FF)
#define L3_INDEX(va)    (((va) >> 12) & 0x1FF)

/* Las tablas las define y las rellena boot.S; aqui solo las nombramos.
 * l1_table hace de TTBR1 (el mapa lineal del kernel) y empty_pgd es el
 * TTBR0 de quien no tiene espacio de usuario. */
extern uint64_t l1_table[512];
extern uint64_t empty_pgd[512];

/* --- SCTLR_EL1: los interruptores ------------------------------------- */
#define SCTLR_C         (1UL << 2)     /* cache de datos                   */
#define SCTLR_I         (1UL << 12)    /* cache de instrucciones           */

/* Baja un nivel, creando la tabla si no existe. */
/* Ojo con lo que guarda una entrada y lo que puede tocar el kernel: la
 * entrada guarda una direccion FISICA (es lo que lee la MMU), pero para
 * escribir en esa tabla hace falta una direccion virtual. De ahi el
 * phys_to_virt: mientras el kernel estuvo mapeado en identidad las dos eran
 * el mismo numero y no se notaba la diferencia. Ahora si. */
static uint64_t *next_table(uint64_t *table, uint64_t index)
{
    if (!(table[index] & PTE_VALID)) {
        uint64_t pa = pmm_alloc();          /* una pagina para la tabla */
        if (!pa) return 0;
        table[index] = pa | PTE_VALID | PTE_TABLE;
    } else if (!(table[index] & PTE_TABLE)) {
        return 0;    /* aqui hay un bloque de 2 MB: no lo partimos */
    }
    return (uint64_t *)phys_to_virt(table[index] & PTE_ADDR_MASK);
}

int vmm_map_in(uint64_t *pgd, uint64_t va, uint64_t pa, uint64_t flags)
{
    uint64_t *l2 = next_table(pgd, L1_INDEX(va));
    if (!l2) return -1;
    uint64_t *l3 = next_table(l2, L2_INDEX(va));
    if (!l3) return -1;

    l3[L3_INDEX(va)] = (pa & PTE_ADDR_MASK) | flags | PTE_PAGE;

    /* La TLB es una cache de traducciones. Si no la invalidamos, la CPU
     * puede seguir usando la traduccion vieja (o la ausencia de ella)
     * durante un rato indeterminado. Este olvido produce los bugs mas
     * desquiciantes que existen: el mapa esta bien, pero no funciona. */
    __asm__ volatile(
        "dsb ishst\n"                    /* que la escritura sea visible   */
        "tlbi vaae1is, %0\n"             /* invalida esa VA en todos los   */
                                         /* nucleos del inner shareable    */
        "dsb ish\n"
        "isb\n"
        :: "r"(va >> PAGE_SHIFT) : "memory");
    return 0;
}

int vmm_map_page(uint64_t va, uint64_t pa, uint64_t flags)
{
    return vmm_map_in(l1_table, va, pa, flags);
}

uint64_t *vmm_empty_pgd(void) { return empty_pgd; }

/* Tabla de traduccion nueva para un proceso: vacia del todo.
 *
 * Hasta el paso anterior habia que copiarle al proceso las dos primeras
 * entradas del kernel, para que el kernel siguiera existiendo cuando
 * entrara una excepcion con TTBR0 apuntando aqui. Ya no hace falta: el
 * kernel vive en TTBR1 y TTBR1 no cambia nunca. Los dos mundos ya no
 * comparten ni una entrada de tabla.
 *
 * Devolvemos un puntero VIRTUAL (lineal); la direccion fisica, que es la
 * que acaba en TTBR0, se saca con virt_to_phys cuando hace falta. */
uint64_t *vmm_create_pgd(void)
{
    uint64_t pa = pmm_alloc();
    if (!pa) return 0;

    return phys_to_virt(pa);        /* pmm_alloc ya la entrega a cero */
}

void vmm_destroy_pgd(uint64_t *pgd)
{
    if (!pgd) return;

    /* Ahora la tabla es entera del proceso: se libera desde la entrada 0.
     * Antes habia que saltarse las dos primeras, que eran del kernel. */
    for (uint64_t i = 0; i < 512; i++) {
        if (!(pgd[i] & PTE_VALID) || !(pgd[i] & PTE_TABLE)) continue;
        uint64_t *l2 = phys_to_virt(pgd[i] & PTE_ADDR_MASK);

        for (uint64_t j = 0; j < 512; j++) {
            if (!(l2[j] & PTE_VALID) || !(l2[j] & PTE_TABLE)) continue;
            uint64_t *l3 = phys_to_virt(l2[j] & PTE_ADDR_MASK);

            for (uint64_t k = 0; k < 512; k++)
                if (l3[k] & PTE_VALID)
                    pmm_free(l3[k] & PTE_ADDR_MASK);   /* la pagina de datos */

            pmm_free(virt_to_phys(l3));
        }
        pmm_free(virt_to_phys(l2));
    }
    pmm_free(virt_to_phys(pgd));
}

/* Cambiar de proceso es ahora tocar UN registro. TTBR1 (el kernel) se queda
 * donde esta, asi que nada de lo que el kernel tenga en la TLB se pierde...
 * salvo porque seguimos tirando la TLB entera. Eso lo arregla el paso 10b
 * con los ASIDs. */
void vmm_switch_to(uint64_t *pgd)
{
    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "isb\n"
        /* Sin ASIDs hay que tirar la TLB entera en cada cambio de proceso.
         * Es correcto pero caro; los ASID permiten conservar las entradas
         * de cada espacio y es una de las mejoras evidentes de este codigo. */
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        :: "r"(virt_to_phys(pgd)) : "memory");
}

/* Traduce una direccion COMO LA VERIA EL0. Es la forma correcta de validar
 * un puntero que viene de un proceso: si 'at s1e0r' falla, ese proceso no
 * tiene derecho a leer ahi, por muy valida que sea la direccion para el
 * kernel. Sin esta comprobacion, un proceso pasaria un puntero al kernel y
 * le haria leer memoria que no le corresponde. */
uint64_t vmm_translate_user(uint64_t va)
{
    uint64_t par;
    __asm__ volatile("at s1e0r, %1\n isb\n mrs %0, par_el1"
                     : "=r"(par) : "r"(va) : "memory");
    if (par & 1)
        return 0;
    return (par & PTE_ADDR_MASK) | (va & (PAGE_SIZE - 1));
}

/* Igual, pero preguntando por ESCRITURA ('w' en vez de 'r'). Hace falta
 * para recv(): el kernel va a escribir el mensaje en un buffer que ha
 * elegido el proceso, y hay que asegurarse de que ese buffer es suyo y es
 * escribible. Si no, un proceso podria hacer que el kernel le machacara
 * memoria a otro, o a si mismo su propio codigo de solo lectura. */
uint64_t vmm_translate_user_w(uint64_t va)
{
    uint64_t par;
    __asm__ volatile("at s1e0w, %1\n isb\n mrs %0, par_el1"
                     : "=r"(par) : "r"(va) : "memory");
    if (par & 1)
        return 0;
    return (par & PTE_ADDR_MASK) | (va & (PAGE_SIZE - 1));
}

uint64_t vmm_translate(uint64_t va)
{
    /* 'at' = Address Translate: le pide a la MMU que traduzca una direccion
     * sin acceder a ella, y deja el resultado en PAR_EL1. Es la forma de
     * preguntarle al hardware "¿tu que crees que es esta direccion?". */
    uint64_t par;
    __asm__ volatile("at s1e1r, %1\n isb\n mrs %0, par_el1"
                     : "=r"(par) : "r"(va) : "memory");
    if (par & 1)
        return 0;                        /* bit 0 a 1 = la traduccion fallo */
    return (par & PTE_ADDR_MASK) | (va & (PAGE_SIZE - 1));
}

void dcache_invalidate_all(void);          /* cache.S */
void dcache_clean_invalidate_all(void);    /* cache.S */

/* Apagar y encender las caches en caliente. Sirve para una sola cosa:
 * medir cuanto valen. No es una operacion inocente.
 *
 * Al APAGAR hay que vaciar antes lo sucio a la RAM (clean+invalidate): si
 * no, esas escrituras se quedarian en una cache que ya nadie mira y se
 * perderian. Al ENCENDER hay que invalidar: mientras estaban apagadas todo
 * fue directo a la RAM, asi que lo que quedaba en la cache esta viejo. */
void caches_disable(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr &= ~(SCTLR_C | SCTLR_I);
    dcache_clean_invalidate_all();
    __asm__ volatile("msr sctlr_el1, %0\n isb\n ic iallu\n dsb sy\n isb"
                     :: "r"(sctlr) : "memory");
}

void caches_enable(void)
{
    uint64_t sctlr;
    dcache_invalidate_all();
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= SCTLR_C | SCTLR_I;
    __asm__ volatile("msr sctlr_el1, %0\n isb\n ic iallu\n dsb sy\n isb"
                     :: "r"(sctlr) : "memory");
}
