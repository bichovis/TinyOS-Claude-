/* linker.ld - Mapa de memoria de TinyOS
 *
 * El firmware de la Raspberry Pi carga kernel8.img en 0x80000 y salta ahi.
 * Por tanto TODO nuestro codigo debe estar enlazado para vivir en esa direccion.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x80000;                /* direccion de carga que impone el firmware */

    .text : {
        KEEP(*(.text.boot))     /* boot.S PRIMERO: _start debe caer en 0x80000 */
        *(.text .text.*)
    }

    .rodata : ALIGN(8) { *(.rodata .rodata.*) }
    .data   : ALIGN(8) { *(.data .data.*) }

    /* .bss = variables globales sin inicializar. No ocupan sitio en el binario,
     * asi que somos nosotros quienes debemos ponerlas a cero al arrancar.     */
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    /* Stack del nucleo 0: 16 KB creciendo hacia abajo desde __stack_top */
    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x4000;
    __stack_top = .;

    /* Alineado a 4 KB: a partir de aqui empieza la RAM que reparte el PMM */
    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note.*) }
}
