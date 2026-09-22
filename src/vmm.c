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
 * El kernel se mapea en identidad (VA = PA) con bloques de 2 MB: cubrir
 * 1 GB cuesta 504 entradas en vez de 262144 paginas.
 */
#include <stdint.h>
#include "mm.h"
#include "irq.h"
#include "uart.h"

#define VA_BITS         39

#define L1_INDEX(va)    (((va) >> 30) & 0x1FF)
#define L2_INDEX(va)    (((va) >> 21) & 0x1FF)
#define L3_INDEX(va)    (((va) >> 12) & 0x1FF)

/* Tablas del kernel. Cada una son 512 * 8 = 4096 bytes: exactamente una
 * pagina, y por eso deben estar alineadas a 4 KB. Viven en .bss, que boot.S
 * ya pone a cero, asi que arrancan con todas las entradas invalidas. */
static uint64_t l1_table[512]  __attribute__((aligned(PAGE_SIZE)));
static uint64_t l2_low[512]    __attribute__((aligned(PAGE_SIZE)));  /* 0-1 GB */
static uint64_t l2_local[512]  __attribute__((aligned(PAGE_SIZE)));  /* 1-2 GB */

/* --- TCR_EL1: como se traduce ------------------------------------------ */
#define TCR_T0SZ        ((uint64_t)(64 - VA_BITS))
#define TCR_T1SZ        ((uint64_t)(64 - VA_BITS) << 16)
#define TCR_IRGN0_WB    (1UL << 8)     /* las propias tablas, cacheables   */
#define TCR_ORGN0_WB    (1UL << 10)
#define TCR_SH0_INNER   (3UL << 12)
#define TCR_TG0_4K      (0UL << 14)    /* granulo de 4 KB en TTBR0         */
#define TCR_EPD1        (1UL << 23)    /* TTBR1 desactivado, de momento    */
#define TCR_IRGN1_WB    (1UL << 24)
#define TCR_ORGN1_WB    (1UL << 26)
#define TCR_SH1_INNER   (3UL << 28)
#define TCR_TG1_4K      (2UL << 30)    /* ojo: TG1 se codifica distinto    */
#define TCR_IPS_40BIT   (2UL << 32)    /* el Cortex-A53 tiene 40 bits PA   */

#define TCR_VALUE  (TCR_T0SZ | TCR_IRGN0_WB | TCR_ORGN0_WB | TCR_SH0_INNER | \
                    TCR_TG0_4K | TCR_T1SZ | TCR_EPD1 | TCR_IRGN1_WB |        \
                    TCR_ORGN1_WB | TCR_SH1_INNER | TCR_TG1_4K | TCR_IPS_40BIT)

/* --- MAIR_EL1: los 8 tipos de memoria ---------------------------------
 * Cada byte describe un tipo; el descriptor de pagina solo guarda el indice.
 *   0x00 = Device-nGnRnE : ni buffer, ni reordenar, ni combinar
 *   0x04 = Device-nGnRE  : permite early write ack; el normal para MMIO
 *   0xFF = Normal, write-back, read/write allocate, inner y outer
 *   0x44 = Normal sin cachear
 */
#define MAIR_VALUE  ((0x00UL << (8 * MT_DEVICE_nGnRnE)) | \
                     (0x04UL << (8 * MT_DEVICE_nGnRE))  | \
                     (0xFFUL << (8 * MT_NORMAL))        | \
                     (0x44UL << (8 * MT_NORMAL_NC)))

/* --- SCTLR_EL1: los interruptores ------------------------------------- */
#define SCTLR_M         (1UL << 0)     /* MMU                              */
#define SCTLR_C         (1UL << 2)     /* cache de datos                   */
#define SCTLR_I         (1UL << 12)    /* cache de instrucciones           */

void vmm_init(void)
{
    /* L1[0] cubre 0 - 1 GB, y L1[1] cubre 1 - 2 GB. Ambas bajan a una tabla
     * L2 porque necesitamos mezclar atributos dentro de ese rango. */
    l1_table[0] = (uint64_t)l2_low   | PTE_VALID | PTE_TABLE;
    l1_table[1] = (uint64_t)l2_local | PTE_VALID | PTE_TABLE;

    /* --- 0 - 1 GB, en bloques de 2 MB --- */
    for (uint64_t i = 0; i < 512; i++) {
        uint64_t pa = i * BLOCK_2MB;

        if (pa < RAM_TOP) {
            /* RAM. El primer bloque (0 - 2 MB) contiene el kernel, asi que
             * tiene que ser ejecutable; todo lo demas lleva PXN, para que un
             * salto a un puntero corrupto muera en vez de ejecutar datos. */
            l2_low[i] = pa | ((i == 0) ? MM_RAM_CODE : MM_RAM_RW);
        } else {
            /* Perifericos (0x3F000000 en adelante): memoria de tipo Device.
             * Marcarlos cacheables seria catastrofico: leerias dos veces el
             * mismo registro de estado y la segunda vendria de la cache. */
            l2_low[i] = pa | MM_DEVICE;
        }
    }

    /* --- 1 - 2 GB: solo hace falta el primer bloque, los "ARM local
     *     peripherals" (0x40000000) con los timers y mailboxes por nucleo. */
    l2_local[0] = 0x40000000UL | MM_DEVICE;
}

/* Baja un nivel, creando la tabla si no existe. */
static uint64_t *next_table(uint64_t *table, uint64_t index)
{
    if (!(table[index] & PTE_VALID)) {
        uint64_t pa = pmm_alloc();          /* una pagina para la tabla */
        if (!pa) return 0;
        table[index] = pa | PTE_VALID | PTE_TABLE;
    } else if (!(table[index] & PTE_TABLE)) {
        return 0;    /* aqui hay un bloque de 2 MB: no lo partimos */
    }
    return (uint64_t *)(table[index] & PTE_ADDR_MASK);
}

int vmm_map_page(uint64_t va, uint64_t pa, uint64_t flags)
{
    uint64_t *l2 = next_table(l1_table, L1_INDEX(va));
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

void vmm_enable(void)
{
    uint64_t flags = irq_save();
    uint64_t sctlr;

    __asm__ volatile(
        /* 1. Publicar la configuracion. El orden importa: MAIR y TCR tienen
         *    que estar puestos antes de que la MMU mire una sola tabla. */
        "msr mair_el1, %0\n"
        "msr tcr_el1,  %1\n"
        "msr ttbr0_el1, %2\n"
        "msr ttbr1_el1, %2\n"
        "isb\n"

        /* 2. Tirar todo lo que la CPU pudiera tener cacheado de antes. */
        "ic iallu\n"                     /* cache de instrucciones          */
        "tlbi vmalle1\n"                 /* toda la TLB de EL1              */
        "dsb nsh\n"
        "isb\n"
        :: "r"(MAIR_VALUE), "r"(TCR_VALUE), "r"((uint64_t)l1_table)
         : "memory");

    /* 3. El salto al vacio: en la instruccion en que M pasa a 1, el suelo
     *    cambia. La siguiente instruccion ya se busca traduciendo su
     *    direccion por las tablas. Como el mapeo es identidad, el PC, el
     *    stack y todo lo demas siguen valiendo... si las tablas son
     *    correctas. Si no, no hay mensaje de error: la maquina desaparece. */
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    __asm__ volatile(
        "msr sctlr_el1, %0\n"
        "isb\n"                          /* sin esto, el pipeline podria    */
                                         /* ejecutar con el estado viejo    */
        :: "r"(sctlr) : "memory");

    irq_restore(flags);
}
