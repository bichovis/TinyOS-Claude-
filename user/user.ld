/* user.ld - Mapa de memoria de un proceso de usuario
 *
 * Nada que ver con el del kernel: otro espacio de direcciones, otra base.
 * Desde que el kernel vive en TTBR1, el proceso tiene TTBR0 entero para el
 * y puede empezar abajo del todo. 4 MB, y no 0, para que un puntero nulo
 * falle en vez de acertar. Tiene que coincidir con USER_BASE de mm.h.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        KEEP(*(.text.start))    /* _start el primero: el kernel salta ahi */
        *(.text .text.*)
    }
    .rodata : ALIGN(8) { *(.rodata .rodata.*) }
    .data   : ALIGN(8) { *(.data .data.*) }
    .bss (NOLOAD) : ALIGN(16) { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note.*) }
}
