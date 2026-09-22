/* user.ld - Mapa de memoria de un proceso de usuario
 *
 * Nada que ver con el del kernel: otro espacio de direcciones, otra base.
 * 0x80000000 porque las dos primeras entradas L1 (0-2 GB) las ocupa el
 * kernel, compartidas por todos los procesos.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    .text : {
        KEEP(*(.text.start))    /* _start el primero: el kernel salta ahi */
        *(.text .text.*)
    }
    .rodata : ALIGN(8) { *(.rodata .rodata.*) }
    .data   : ALIGN(8) { *(.data .data.*) }
    .bss (NOLOAD) : ALIGN(16) { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note.*) }
}
