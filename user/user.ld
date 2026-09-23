/* user.ld - Mapa de memoria de un proceso de usuario
 *
 * Nada que ver con el del kernel: otro espacio de direcciones, otra base.
 * Desde que el kernel vive en TTBR1, el proceso tiene TTBR0 entero para el
 * y puede empezar abajo del todo. 4 MB, y no 0, para que un puntero nulo
 * falle en vez de acertar.
 *
 * El corte entre codigo y datos esta alineado a una pagina, y eso ya no es
 * cosmetica: el enlazador emite un segmento ELF por cada tramo con
 * permisos distintos, y el cargador mapea cada uno tal cual. Una pagina no
 * puede ser medio ejecutable.
 *
 * (Se compila con -z max-page-size=4096. Sin eso, el enlazador de AArch64
 * alinea los segmentos a 64 KB y el fichero engorda quince veces.)
 */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    /* Tramo 1: se leera y se ejecutara, pero no se escribira. */
    .text : {
        KEEP(*(.text.start))    /* _start el primero, por legibilidad */
        *(.text .text.*)
    }
    .rodata : ALIGN(8) { *(.rodata .rodata.*) }

    . = ALIGN(4096);            /* aqui cambian los permisos */

    /* Tramo 2: se leera y se escribira, pero no se ejecutara. */
    .data : { *(.data .data.*) }

    /* .bss no ocupa sitio en el fichero: el enlazador la cuenta en
     * p_memsz y no en p_filesz, y el cargador rellena esa diferencia con
     * ceros. Es la definicion de .bss expresada en el formato. */
    .bss (NOLOAD) : ALIGN(16) {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
    }

    /DISCARD/ : { *(.comment) *(.note.*) }
}
