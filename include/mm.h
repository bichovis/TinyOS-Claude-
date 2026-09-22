/* mm.h - Gestion de memoria: paginas fisicas y tablas de traduccion */
#pragma once
#include <stdint.h>

#define PAGE_SIZE        4096UL
#define PAGE_SHIFT       12
#define BLOCK_2MB        (2UL * 1024 * 1024)

/* Mapa fisico de la Raspberry Pi 3B:
 *   0x00000000 - 0x3EFFFFFF   RAM vista por la CPU ARM
 *   0x3F000000 - 0x3FFFFFFF   perifericos (UART, GPIO, controlador IRQ...)
 *   0x40000000 - 0x400000FF   "ARM local peripherals" (timers, mailboxes)  */
#define RAM_TOP          0x3F000000UL
#define PERIPH_START     0x3F000000UL
#define PERIPH_END       0x40200000UL

/* --- Indices dentro de MAIR_EL1 ---------------------------------------
 * MAIR es una tabla de 8 "tipos de memoria". Cada descriptor de pagina no
 * lleva los atributos, lleva un INDICE de 3 bits a esta tabla. */
#define MT_DEVICE_nGnRnE 0   /* sin buffer, sin reorden: lo mas estricto   */
#define MT_DEVICE_nGnRE  1   /* perifericos normales                        */
#define MT_NORMAL        2   /* RAM: write-back, cacheable                  */
#define MT_NORMAL_NC     3   /* RAM sin cachear (DMA, por ejemplo)          */

/* --- Bits de un descriptor de tabla/bloque/pagina ---------------------- */
#define PTE_VALID        (1UL << 0)   /* si es 0, la entrada no existe      */
#define PTE_TABLE        (1UL << 1)   /* 0b11 = apunta a otra tabla         */
#define PTE_BLOCK        (0UL << 1)   /* 0b01 = traduce aqui mismo          */
#define PTE_PAGE         (1UL << 1)   /* en L3, 0b11 = pagina de 4 KB       */
#define PTE_ATTR(i)      ((uint64_t)(i) << 2)
#define PTE_AP_RW_EL1    (0UL << 6)   /* lectura/escritura solo en EL1      */
#define PTE_AP_RW_ALL    (1UL << 6)   /* lectura/escritura tambien en EL0   */
#define PTE_AP_RO_EL1    (2UL << 6)   /* solo lectura, solo EL1             */
#define PTE_AP_RO_ALL    (3UL << 6)   /* solo lectura, EL1 y EL0            */
#define PTE_SH_INNER     (3UL << 8)   /* inner shareable: coherente con     */
                                      /* los otros nucleos                  */
#define PTE_AF           (1UL << 10)  /* Access Flag: si es 0, FALLA        */
#define PTE_nG           (1UL << 11)  /* entrada no global (por proceso)    */
#define PTE_PXN          (1UL << 53)  /* prohibido ejecutar desde EL1       */
#define PTE_UXN          (1UL << 54)  /* prohibido ejecutar desde EL0       */

#define PTE_ADDR_MASK    0x0000FFFFFFFFF000UL

/* Combinaciones que usamos */
#define MM_RAM_RW    (PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR(MT_NORMAL) \
                      | PTE_AP_RW_EL1 | PTE_UXN | PTE_PXN)
#define MM_RAM_RO    (PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR(MT_NORMAL) \
                      | PTE_AP_RO_EL1 | PTE_UXN | PTE_PXN)
#define MM_RAM_CODE  (PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR(MT_NORMAL) \
                      | PTE_AP_RW_EL1 | PTE_UXN)
#define MM_DEVICE    (PTE_VALID | PTE_AF | PTE_ATTR(MT_DEVICE_nGnRE)          \
                      | PTE_AP_RW_EL1 | PTE_UXN | PTE_PXN)

/* Paginas de un proceso de usuario. PTE_nG las marca "no globales": la TLB
 * no las comparte entre espacios de direcciones distintos.
 *   CODE: solo lectura, ejecutable en EL0 (UXN=0) pero NO en EL1 (PXN=1),
 *         para que un puntero de funcion corrupto del kernel no pueda
 *         acabar ejecutando codigo del proceso con privilegios.
 *   DATA: lectura/escritura desde EL0, nunca ejecutable.                  */
#define MM_USER_CODE (PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR(MT_NORMAL) \
                      | PTE_AP_RO_ALL | PTE_PXN | PTE_nG)
#define MM_USER_DATA (PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR(MT_NORMAL) \
                      | PTE_AP_RW_ALL | PTE_PXN | PTE_UXN | PTE_nG)

/* Mapa de un proceso de usuario. Vive a partir de 2 GB porque las dos
 * primeras entradas L1 (0-2 GB) las ocupa el kernel, compartidas por todos
 * los espacios. El split TTBR0/TTBR1 liberaria el rango bajo.            */
#define USER_BASE        0x80000000UL      /* codigo                       */
#define USER_STACK_TOP   0x80200000UL      /* pila (crece hacia abajo)     */
#define USER_LIMIT       0xC0000000UL      /* nada de usuario por encima   */

/* --- Gestor de memoria fisica (pmm.c) --------------------------------- */
void     pmm_init(void);
uint64_t pmm_alloc(void);             /* una pagina de 4 KB, 0 si no hay    */
void     pmm_free(uint64_t pa);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
uint64_t pmm_used_pages(void);

/* --- Memoria virtual (vmm.c) ------------------------------------------ */
void     vmm_init(void);              /* construye las tablas del kernel    */
void     vmm_enable(void);            /* enciende MMU, cachés y todo        */
int      vmm_map_page(uint64_t va, uint64_t pa, uint64_t flags);
uint64_t vmm_translate(uint64_t va);  /* pregunta al hardware: VA -> PA      */

/* --- Espacios de direcciones por proceso ------------------------------- */
uint64_t *vmm_kernel_pgd(void);
uint64_t *vmm_create_pgd(void);       /* tabla nueva, con el kernel dentro   */
void      vmm_destroy_pgd(uint64_t *pgd);
int       vmm_map_in(uint64_t *pgd, uint64_t va, uint64_t pa, uint64_t flags);
void      vmm_switch_to(uint64_t *pgd);   /* cambia TTBR0                    */
uint64_t  vmm_translate_user(uint64_t va);/* traduce como lo veria EL0       */
