/* mm.h - Gestion de memoria: paginas fisicas y tablas de traduccion */
#pragma once
#include <stdint.h>

#define PAGE_SIZE        4096UL
#define PAGE_SHIFT       12
#define BLOCK_2MB        (2UL * 1024 * 1024)

/* Mapa fisico de la Raspberry Pi 3B:
 *   0x00000000 - ram_top      RAM que la GPU le ha dejado a la CPU
 *   ram_top    - 0x3EFFFFFF   RAM reservada para la GPU: NO tocar
 *   0x3F000000 - 0x3FFFFFFF   perifericos (UART, GPIO, controlador IRQ...)
 *   0x40000000 - 0x400000FF   "ARM local peripherals" (timers, mailboxes)
 *
 * RAM_MAX es el tope absoluto (donde empiezan los perifericos); el limite
 * de verdad lo dice la GPU por el buzon y puede ser bastante menor. */
#define RAM_MAX          0x3F000000UL
#define PERIPH_START     0x3F000000UL
#define PERIPH_END       0x40200000UL

/* --- El kernel vive arriba: el split TTBR0 / TTBR1 ---------------------
 *
 * AArch64 no traduce con una tabla, sino con DOS, y elige cual segun los
 * bits altos de la direccion. Con 39 bits de VA quedan dos mitades de
 * 512 GB separadas por un abismo de direcciones invalidas:
 *
 *   0x0000000000000000 - 0x0000007FFFFFFFFF   TTBR0  ->  el proceso
 *   (nada en medio: cualquier direccion de ahi es una excepcion)
 *   0xFFFFFF8000000000 - 0xFFFFFFFFFFFFFFFF   TTBR1  ->  el kernel
 *
 * El kernel se mapea LINEAL: VA = PA + KERNEL_VA_BASE. Convertir de una a
 * otra es una suma, sin consultar ninguna tabla. A cambio, el kernel solo
 * puede ver la RAM que tenga mapeada de antemano (los 2 primeros GB, que
 * en esta placa son toda).
 *
 * Que el kernel este en TTBR1 tiene dos consecuencias grandes:
 *   - cambiar de proceso solo toca TTBR0; el kernel no se mueve
 *   - TTBR0 queda entero para el usuario, que ya no empieza en 2 GB      */
#define KERNEL_VA_BASE   0xFFFFFF8000000000UL

/* --- Zona de pilas de kernel ------------------------------------------
 *
 * Las pilas de kernel NO pueden vivir en el mapa lineal, y el motivo es
 * geometrico: ese mapa esta hecho de bloques de 2 MB, y dentro de un
 * bloque no se puede dejar un hueco de 4 KB. Y un hueco es justo lo que
 * hace falta.
 *
 * Aqui cada tarea tiene dos paginas de espacio virtual: la de abajo se
 * queda SIN MAPEAR -es la pagina de guarda- y la de arriba es la pila de
 * verdad. Desbordar la pila deja de ser escribir en silencio encima de la
 * tarea de al lado y pasa a ser un fallo de traduccion inmediato, en la
 * instruccion exacta que se paso.
 *
 * Esta en el indice L1 numero 4, muy lejos del mapa lineal (que ocupa el
 * 0 y el 1) y de la ventana de pruebas de kernel.c (el 3).
 */
#define KSTACK_AREA      (KERNEL_VA_BASE + 0x100000000UL)
#define KSTACK_SLOT      (2 * PAGE_SIZE)   /* guarda + pila */

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

/* Los bits 55 a 58 de un descriptor los ignora el hardware: son para que
 * el sistema operativo apunte lo que quiera. Aqui marcan una pagina que
 * esta compartida y que hay que copiar en cuanto alguien escriba.
 *
 * Sin una marca asi no habria forma de distinguir "de solo lectura porque
 * es codigo" de "de solo lectura porque todavia no te he dado tu copia", y
 * son dos cosas muy distintas: la primera es una violacion y la segunda un
 * tramite. */
#define PTE_COW          (1UL << 55)

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

/* Un fichero mapeado: se lee, no se escribe y no se ejecuta.
 *
 * De solo lectura porque no hay nada que escriba los cambios de vuelta al
 * disco. Permitir escribir daria un mapeo que parece funcionar y pierde
 * todo lo escrito al morir el proceso, que es peor que no dejar. */
#define MM_USER_RO   (PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR(MT_NORMAL) \
                      | PTE_AP_RO_ALL | PTE_PXN | PTE_UXN | PTE_nG)

/* Mapa de un proceso de usuario. Vive abajo del todo porque TTBR0 es suyo
 * entero: el kernel ya no le ocupa ni una entrada. Empezamos en 4 MB y no
 * en 0 para que un puntero nulo (y sus vecinos) fallen en vez de acertar.
 *
 *   0x00400000  codigo y datos (lo que diga el ELF)
 *               |
 *               v  el monton crece hacia arriba desde donde acabe el ELF
 *   0x0F000000  tope del monton
 *   0x10000000  MMIO concedido, si es un driver
 *               ^  la pila crece hacia abajo
 *   0x20000000  tope de la pila
 *
 * Entre el monton y la pila hay un abismo de 256 MB a proposito: que
 * crezcan el uno contra el otro y se toquen es un error clasico, y con
 * esta distancia hace falta pedir mucho para llegar. */
#define USER_BASE        0x00400000UL      /* codigo, en 4 MB              */
#define USER_HEAP_MAX    0x0F000000UL      /* hasta donde puede crecer     */
#define USER_MMIO_BASE   0x10000000UL      /* MMIO concedido a un driver   */
#define USER_STACK_TOP   0x20000000UL      /* pila (crece hacia abajo)     */
#define USER_STACK_MIN   0x1FF00000UL      /* ...hasta aqui: 1 MB de pila  */
/* Ficheros mapeados. Encima de la pila, que crece hacia abajo, asi que
 * entre las dos zonas queda un hueco de 256 MB que nadie puede alcanzar
 * por accidente. */
#define USER_MMAP_BASE   0x30000000UL      /* aqui empiezan los ficheros   */
#define USER_MMAP_MAX    0x38000000UL      /* ...y aqui se acaban: 128 MB  */
#define USER_LIMIT       0x40000000UL      /* nada de usuario por encima   */

/* --- Gestor de memoria fisica (pmm.c) --------------------------------- */
void     pmm_init(uint64_t ram_limit);
uint64_t pmm_alloc(void);             /* una pagina de 4 KB, 0 si no hay    */
uint64_t pmm_alloc_contig(uint64_t n);   /* n paginas SEGUIDAS              */
void     pmm_free_contig(uint64_t pa, uint64_t n);
void     pmm_free(uint64_t pa);       /* "yo ya no la uso"                  */
void     pmm_ref(uint64_t pa);        /* "yo tambien la uso"                */
uint64_t pmm_refs(uint64_t pa);       /* cuantos la usan                    */
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
uint64_t pmm_used_pages(void);

/* --- El monton del kernel (kheap.c) -----------------------------------
 * Memoria de tamanyo arbitrario. Por debajo pide paginas contiguas al PMM
 * y las va partiendo; al liberar funde los trozos vecinos, que es lo que
 * evita que el monton se pique hasta quedarse sin huecos grandes. */
void *kmalloc(uint64_t n);
void  kfree(void *p);
void  kheap_stats(uint64_t *total, uint64_t *usado,
                  uint64_t *huecos, uint64_t *mayor);

/* --- Memoria virtual (vmm.c) ------------------------------------------ */
/* Las tablas del kernel y el encendido de la MMU ya no estan aqui: ocurren
 * en boot.S, antes de la primera instruccion de C. No hay alternativa: el
 * kernel esta enlazado en direcciones altas, asi que sin MMU no podria ni
 * leer una cadena de texto. */
void     caches_disable(void);        /* apaga D+I (para medir)             */
void     caches_enable(void);         /* y las vuelve a encender            */
int      vmm_map_page(uint64_t va, uint64_t pa, uint64_t flags);
int      vmm_unmap_page(uint64_t va);   /* en el espacio del kernel        */
uint64_t vmm_translate(uint64_t va);  /* pregunta al hardware: VA -> PA      */

/* --- Espacios de direcciones por proceso ------------------------------- */
uint64_t *vmm_empty_pgd(void);        /* TTBR0 de un hilo de kernel      */
uint64_t *vmm_create_pgd(uint64_t *asid_out);   /* tabla nueva + su ASID     */
void      vmm_destroy_pgd(uint64_t *pgd, uint64_t asid);

/* Duplicar un espacio de direcciones compartiendo sus paginas, marcadas
 * para copiarse cuando alguien escriba. */
uint64_t *vmm_fork(uint64_t *padre, uint64_t asid_padre, uint64_t *asid_hijo);

/* Alguien ha escrito en una pagina COW. Devuelve 1 si lo ha resuelto. */
int       vmm_cow_fault(uint64_t *pgd, uint64_t va, uint64_t asid);
int       vmm_map_in(uint64_t *pgd, uint64_t va, uint64_t pa, uint64_t flags);
int       vmm_unmap_in(uint64_t *pgd, uint64_t va);  /* y devuelve la pagina */
void      vmm_switch_to(uint64_t *pgd, uint64_t asid);  /* tabla + etiqueta  */
uint64_t  vmm_translate_user(uint64_t va);      /* ¿puede EL0 LEER aqui?     */
int       user_touch_r(uint64_t va);           /* ...y si no, traerla       */

/* Copiar entre el kernel y un proceso SIN fiarse del puntero: si la
 * direccion no vale, esto vuelve con un numero en vez de reventar.
 * Devuelven los bytes que NO se pudieron copiar. Ver src/usercopy.S. */
uint64_t  copy_from_user(void *dst, uint64_t uva, uint64_t n);
uint64_t  copy_to_user(uint64_t uva, const void *src, uint64_t n);
uint64_t  vmm_translate_user_w(uint64_t va);    /* ¿puede EL0 ESCRIBIR aqui? */

/* --- Lineal <-> fisico -------------------------------------------------
 * El kernel maneja direcciones fisicas a menudo (pmm_alloc devuelve una,
 * las entradas de las tablas guardan otra), pero no puede desreferenciarlas:
 * lo que la CPU traduce son direcciones virtuales. Estas dos funciones son
 * el puente, y solo valen para la RAM que cubre el mapa lineal.           */
static inline void *phys_to_virt(uint64_t pa)
{
    return (void *)(pa + KERNEL_VA_BASE);
}

static inline uint64_t virt_to_phys(const void *va)
{
    return (uint64_t)va - KERNEL_VA_BASE;
}
