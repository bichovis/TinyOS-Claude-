/* linker.ld - Mapa de memoria de TinyOS
 *
 * Aqui hay dos direcciones para cada byte del kernel y conviene no
 * confundirlas:
 *
 *   LMA (load address)  0x00080000        donde el firmware deja la imagen
 *   VMA (virtual address) 0xFFFFFF8000080000  donde el kernel cree que vive
 *
 * El kernel esta enlazado en la mitad alta, la de TTBR1. Eso significa que
 * NADA de su codigo funciona hasta que la MMU esta encendida: una simple
 * referencia a una cadena de texto apunta a 0xFFFFFF80... y sin traduccion
 * esa direccion no existe. Por eso boot.S enciende la MMU a mano, usando
 * solo direcciones calculadas, antes de saltar aqui arriba.
 *
 * AT() es lo que le dice al enlazador "colocalo en la imagen como si fuera
 * a cargarse en 0x80000, aunque se ejecute desde arriba".
 */

KERNEL_VA_BASE = 0xFFFFFF8000000000;
PHYS_BASE      = 0x80000;            /* direccion de carga que impone el firmware */

ENTRY(_start)

SECTIONS
{
    . = KERNEL_VA_BASE + PHYS_BASE;

    .text : AT(PHYS_BASE) {
        KEEP(*(.text.boot))     /* boot.S PRIMERO: _start debe caer en 0x80000 */
        *(.text .text.*)
    }

    .rodata : ALIGN(8) { *(.rodata .rodata.*) }

    /* La tabla de arreglos: pares (instruccion que puede fallar, a donde
     * saltar si falla). La rellenan las propias funciones que tocan
     * memoria de usuario, cada una con una linea de ensamblador que
     * escribe aqui sin salirse de donde esta.
     *
     * Que la construya el ENLAZADOR y no el codigo es lo que la hace
     * barata: no hay que registrar nada al arrancar ni mantener una lista;
     * las entradas aparecen porque alguien escribio la instruccion. */
    .ex_table : ALIGN(8) {
        __ex_table_start = .;
        KEEP(*(.ex_table))
        __ex_table_end = .;
    }
    .data   : ALIGN(8) { *(.data .data.*) }

    /* .bss = variables globales sin inicializar. No ocupan sitio en el binario,
     * asi que somos nosotros quienes debemos ponerlas a cero al arrancar.
     * Aqui viven tambien las tablas de traduccion de arranque (boot.S), que
     * por eso deben estar alineadas a 4 KB.                                 */
    .bss (NOLOAD) : ALIGN(4096) {
        __bss_start = .;
        *(.bss.pgtables)        /* las tablas, primero y alineadas */
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    /* Pilas de los cuatro nucleos: 16 KB cada una, creciendo hacia abajo.
     * El nucleo 0 se lleva la de arriba del todo (__stack_top) y cada
     * nucleo N la suya, 16 KB mas abajo. Ver at_el1 en boot.S. */
    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x4000 * 4;
    __stack_top = .;

    /* Alineado a 4 KB: a partir de aqui empieza la RAM que reparte el PMM */
    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note.*) }
}
