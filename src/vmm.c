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
        "tlbi vaae1is, %0\n"             /* esa VA, en todos los nucleos   */
                                         /* y en todos los ASIDs: aqui no  */
                                         /* sabemos de quien es la tabla,  */
                                         /* y pasarse de celoso es gratis  */
                                         /* (esto solo corre al crear un   */
                                         /* proceso o al mapear en el      */
                                         /* kernel, no en cada cambio)     */
        "dsb ish\n"
        "isb\n"
        :: "r"(va >> PAGE_SHIFT) : "memory");
    return 0;
}

/* Quitar una pagina del mapa y devolverla al gestor.
 *
 * Bajar sin crear: si por el camino no hay tabla, es que ahi no habia nada
 * mapeado y no hay nada que hacer. Las tablas intermedias se quedan aunque
 * se vacien; recogerlas costaria contar cuantas entradas les quedan vivas,
 * y son 4 KB cada una. */
int vmm_unmap_in(uint64_t *pgd, uint64_t va)
{
    uint64_t *t = pgd;

    for (int nivel = 0; nivel < 2; nivel++) {
        uint64_t i = nivel ? L2_INDEX(va) : L1_INDEX(va);
        if (!(t[i] & PTE_VALID) || !(t[i] & PTE_TABLE)) return -1;
        t = phys_to_virt(t[i] & PTE_ADDR_MASK);
    }

    uint64_t i = L3_INDEX(va);
    if (!(t[i] & PTE_VALID)) return -1;

    uint64_t pa = t[i] & PTE_ADDR_MASK;
    t[i] = 0;

    /* Borrar la entrada no basta: mientras la traduccion siga en la TLB, el
     * proceso sigue llegando a una pagina que ya no es suya. */
    __asm__ volatile(
        "dsb ishst\n"
        "tlbi vaae1is, %0\n"
        "dsb ish\n"
        "isb\n"
        :: "r"(va >> PAGE_SHIFT) : "memory");

    pmm_free(pa);
    return 0;
}

int vmm_map_page(uint64_t va, uint64_t pa, uint64_t flags)
{
    return vmm_map_in(l1_table, va, pa, flags);
}

int vmm_unmap_page(uint64_t va)
{
    return vmm_unmap_in(l1_table, va);
}

uint64_t *vmm_empty_pgd(void) { return empty_pgd; }

/* --- ASIDs: etiquetar la TLB -------------------------------------------
 *
 * Hasta aqui, cada cambio de proceso tiraba la TLB entera. Con el kernel en
 * TTBR1 eso era ademas absurdo: sus traducciones no cambian nunca y las
 * tirabamos igual, unas setecientas veces en diez segundos de arranque.
 *
 * Lo que ofrece el hardware es etiquetar. Cada entrada de la TLB que venga
 * de una pagina marcada 'nG' (non-global) se guarda con el ASID del espacio
 * que la creo, y la MMU solo la da por buena si coincide con el ASID
 * activo. Las paginas del kernel NO llevan nG: son globales, valen en todos
 * los espacios y ya nadie las echa.
 *
 * Y el ASID activo no vive en un registro aparte: son los bits [63:48] de
 * TTBR0_EL1, los mismos que la direccion de la tabla. Eso no es por ahorrar
 * registros, es para que cambiar de tabla y de etiqueta sea UNA escritura
 * de 64 bits. Si fueran dos, existiria un instante con la tabla nueva y la
 * etiqueta vieja, y lo que la MMU cachease en ese instante estaria mal
 * etiquetado para siempre.
 *
 * Usamos 8 bits (TCR_EL1.AS = 0): 256 espacios, de sobra para 16 tareas.
 * El ASID 0 se reserva para "no tengo espacio de usuario".
 */
#define ASID_MAX     256
#define ASID_WORDS   (ASID_MAX / 64)

static uint64_t asid_map[ASID_WORDS] = { 1 };   /* el 0 nace ocupado */

static uint64_t asid_alloc(void)
{
    for (uint64_t i = 1; i < ASID_MAX; i++) {
        if (!(asid_map[i / 64] & (1UL << (i % 64)))) {
            asid_map[i / 64] |= 1UL << (i % 64);
            return i;
        }
    }
    return 0;                        /* no quedan; 0 significa fallo */
}

static void asid_free(uint64_t asid)
{
    if (asid == 0 || asid >= ASID_MAX) return;

    /* Antes de reciclar una etiqueta hay que borrar de la TLB todo lo que
     * la lleve puesta: si no, el proximo proceso que la reciba heredaria
     * las traducciones del muerto y leeria su memoria. Esta es la unica
     * invalidacion que queda en la vida de un proceso, y ocurre cuando
     * muere, no cada vez que le toca la CPU. */
    __asm__ volatile(
        "dsb ishst\n"
        "tlbi aside1is, %0\n"        /* toda la TLB de ESE espacio */
        "dsb ish\n"
        "isb\n"
        :: "r"(asid << 48) : "memory");

    asid_map[asid / 64] &= ~(1UL << (asid % 64));
}

/* Tabla de traduccion nueva para un proceso: vacia del todo.
 *
 * Hasta el paso anterior habia que copiarle al proceso las dos primeras
 * entradas del kernel, para que el kernel siguiera existiendo cuando
 * entrara una excepcion con TTBR0 apuntando aqui. Ya no hace falta: el
 * kernel vive en TTBR1 y TTBR1 no cambia nunca. Los dos mundos ya no
 * comparten ni una entrada de tabla.
 *
 * Devolvemos un puntero VIRTUAL (lineal); la direccion fisica, que es la
 * que acaba en TTBR0, se saca con virt_to_phys cuando hace falta. Y de
 * paso repartimos el ASID: un espacio de direcciones es la tabla mas la
 * etiqueta, y no tiene sentido tener una sin la otra. */
uint64_t *vmm_create_pgd(uint64_t *asid_out)
{
    uint64_t asid = asid_alloc();
    if (!asid) return 0;             /* sin etiquetas libres */

    uint64_t pa = pmm_alloc();
    if (!pa) { asid_free(asid); return 0; }

    *asid_out = asid;
    return phys_to_virt(pa);         /* pmm_alloc ya la entrega a cero */
}

static void copiar_pagina(void *dst, const void *src)
{
    uint64_t *d = dst;
    const uint64_t *s = src;
    for (uint64_t i = 0; i < PAGE_SIZE / 8; i++) d[i] = s[i];
}

/* --- Duplicar un espacio de direcciones sin copiarlo ------------------
 *
 * Es el truco entero del fork. Se recorre la tabla del padre y se monta
 * la misma en el hijo, apuntando a LAS MISMAS paginas fisicas. No se copia
 * ni un byte de datos: se copian punteros.
 *
 * A cambio, todo lo que era escribible pasa a solo lectura y marcado COW
 * EN LOS DOS. En el padre tambien, y eso es lo que mas cuesta ver: si el
 * padre conservara su permiso de escritura, escribiria en paginas que ya
 * no son solo suyas y el hijo veria cambios que no le corresponden.
 *
 * El sistema esta mintiendo a los dos: les dice que no pueden escribir
 * cuando en realidad si pueden. La mentira se deshace, pagina a pagina y
 * solo cuando hace falta, en vmm_cow_fault().
 */
uint64_t *vmm_fork(uint64_t *padre, uint64_t asid_padre, uint64_t *asid_hijo)
{
    uint64_t *hijo = vmm_create_pgd(asid_hijo);
    if (!hijo) return 0;

    for (uint64_t i1 = 0; i1 < 512; i1++) {
        if (!(padre[i1] & PTE_VALID) || !(padre[i1] & PTE_TABLE)) continue;
        uint64_t *l2 = phys_to_virt(padre[i1] & PTE_ADDR_MASK);

        for (uint64_t i2 = 0; i2 < 512; i2++) {
            if (!(l2[i2] & PTE_VALID) || !(l2[i2] & PTE_TABLE)) continue;
            uint64_t *l3 = phys_to_virt(l2[i2] & PTE_ADDR_MASK);

            for (uint64_t i3 = 0; i3 < 512; i3++) {
                uint64_t e = l3[i3];
                if (!(e & PTE_VALID)) continue;

                uint64_t va = (i1 << 30) | (i2 << 21) | (i3 << 12);
                uint64_t pa = e & PTE_ADDR_MASK;

                /* El MMIO concedido a un driver se comparte tal cual: unos
                 * registros de periferico no se copian ni tiene sentido
                 * que se copien. */
                if (((e >> 2) & 7) != MT_NORMAL) {
                    vmm_map_in(hijo, va, pa, e & ~PTE_ADDR_MASK);
                    continue;
                }

                if ((e & (3UL << 6)) == PTE_AP_RW_ALL) {
                    e = (e & ~(3UL << 6)) | PTE_AP_RO_ALL | PTE_COW;
                    l3[i3] = e;                  /* el padre, tambien */
                }

                pmm_ref(pa);
                if (vmm_map_in(hijo, va, pa, e & ~PTE_ADDR_MASK) < 0) {
                    pmm_free(pa);
                    vmm_destroy_pgd(hijo, *asid_hijo);
                    return 0;
                }
            }
        }
    }

    /* Al padre se le han cambiado los permisos por debajo: lo que tenga en
     * la TLB de su espacio ya no vale. */
    __asm__ volatile(
        "dsb ishst\n"
        "tlbi aside1is, %0\n"
        "dsb ish\n"
        "isb\n"
        :: "r"(asid_padre << 48) : "memory");

    return hijo;
}

/* --- Deshacer la mentira ----------------------------------------------
 *
 * Alguien ha intentado escribir en una pagina marcada COW. Hay dos casos y
 * la diferencia entre ellos es todo el ahorro del fork:
 *
 *   la usa uno solo  -> no hay nada que copiar, se le devuelve el permiso
 *   la usan varios   -> ahora si, se le hace su copia privada
 *
 * El primer caso es el que hace que un fork seguido de exec no copie casi
 * nada, y el que hace que si el padre muere, el hijo se quede con las
 * paginas originales sin pagar una sola copia.
 */
int vmm_cow_fault(uint64_t *pgd, uint64_t va, uint64_t asid)
{
    uint64_t *t = pgd;

    for (int n = 0; n < 2; n++) {
        uint64_t i = n ? L2_INDEX(va) : L1_INDEX(va);
        if (!(t[i] & PTE_VALID) || !(t[i] & PTE_TABLE)) return 0;
        t = phys_to_virt(t[i] & PTE_ADDR_MASK);
    }

    uint64_t i = L3_INDEX(va);
    uint64_t e = t[i];
    if (!(e & PTE_VALID) || !(e & PTE_COW)) return 0;   /* no es asunto nuestro */

    uint64_t pa    = e & PTE_ADDR_MASK;
    uint64_t flags = (e & ~PTE_ADDR_MASK & ~PTE_COW & ~(3UL << 6)) | PTE_AP_RW_ALL;

    if (pmm_refs(pa) <= 1) {
        t[i] = pa | flags;                  /* ya no la comparte nadie */
    } else {
        uint64_t nueva = pmm_alloc();
        if (!nueva) return 0;
        copiar_pagina(phys_to_virt(nueva), phys_to_virt(pa));
        t[i] = (nueva & PTE_ADDR_MASK) | flags;
        pmm_free(pa);                       /* una referencia menos */
    }

    __asm__ volatile(
        "dsb ishst\n"
        "tlbi vae1is, %0\n"
        "dsb ish\n"
        "isb\n"
        :: "r"((asid << 48) | (va >> PAGE_SHIFT)) : "memory");

    return 1;
}

void vmm_destroy_pgd(uint64_t *pgd, uint64_t asid)
{
    if (!pgd) return;

    asid_free(asid);          /* devuelve la etiqueta y limpia su TLB */

    /* Ahora la tabla es entera del proceso: se libera desde la entrada 0.
     * Antes habia que saltarse las dos primeras, que eran del kernel. */
    for (uint64_t i = 0; i < 512; i++) {
        if (!(pgd[i] & PTE_VALID) || !(pgd[i] & PTE_TABLE)) continue;
        uint64_t *l2 = phys_to_virt(pgd[i] & PTE_ADDR_MASK);

        for (uint64_t j = 0; j < 512; j++) {
            if (!(l2[j] & PTE_VALID) || !(l2[j] & PTE_TABLE)) continue;
            uint64_t *l3 = phys_to_virt(l2[j] & PTE_ADDR_MASK);

            for (uint64_t k = 0; k < 512; k++) {
                if (!(l3[k] & PTE_VALID)) continue;

                /* Ojo: no toda pagina mapeada es RAM nuestra. A un driver
                 * de EL0 le hemos mapeado los registros de un periferico;
                 * devolver eso al PMM seria repartir la UART como si fuera
                 * memoria libre. Se distingue por el indice de MAIR que
                 * lleva el propio descriptor. */
                if (((l3[k] >> 2) & 7) != MT_NORMAL) continue;

                pmm_free(l3[k] & PTE_ADDR_MASK);       /* la pagina de datos */
            }

            pmm_free(virt_to_phys(l3));
        }
        pmm_free(virt_to_phys(l2));
    }
    pmm_free(virt_to_phys(pgd));
}

/* Cambiar de proceso es, por fin, UNA escritura.
 *
 * La tabla y la etiqueta van juntas en TTBR0_EL1, y no hay ninguna
 * invalidacion: lo del proceso que se va queda en la TLB con su ASID, lo
 * del que entra con el suyo, y lo del kernel es global y no se toca. Si el
 * proceso que se va vuelve dentro de tres turnos, sus traducciones siguen
 * ahi.
 *
 * El 'isb' sigue siendo obligatorio: sin el, las instrucciones que ya
 * estan en el pipeline podrian traducirse con el TTBR0 anterior. */
void vmm_switch_to(uint64_t *pgd, uint64_t asid)
{
    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "isb\n"
        :: "r"(virt_to_phys(pgd) | (asid << 48)) : "memory");
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
 * medir cuanto valen. No es una operacion inocente, y tiene una
 * consecuencia que no se ve venir:
 *
 *   MIENTRAS LAS CACHES ESTAN APAGADAS NO SE PUEDE COGER UN SPINLOCK.
 *
 * Con SCTLR_EL1.C a 0, toda la memoria normal pasa a tratarse como NO
 * cacheable. Y las instrucciones exclusivas -ldaxr/stlxr, de las que vive
 * spin_lock()- necesitan un monitor que en un Cortex-A53 va con la cache:
 * sobre memoria no cacheable el store-exclusive falla SIEMPRE, y el bucle
 * de spin_lock gira hasta el fin de los tiempos.
 *
 * Por eso quien llame a esto tiene que tapar las interrupciones durante
 * todo el tramo sin caches: una IRQ ahi acaba en scheduler_tick(), que
 * coge el cerrojo del planificador, y ahi se acabo la maquina.
 *
 * QEMU no lo reproduce: sus exclusivas siempre funcionan. El silicio no
 * perdona, y esto se paga con un cuelgue mudo.
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
