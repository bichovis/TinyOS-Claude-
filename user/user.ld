/* user.ld - Mapa de memoria de un proceso de usuario
 *
 * Nada que ver con el del kernel: otro espacio de direcciones, otra base.
 * Desde que el kernel vive en TTBR1, el proceso tiene TTBR0 entero para el
 * y puede empezar abajo del todo. 4 MB, y no 0, para que un puntero nulo
 * falle en vez de acertar. Tiene que coincidir con USER_BASE de mm.h.
 *
 * El reparto en dos tramos no es cosmetico: el kernel los mapea con
 * permisos distintos, y por eso el corte entre ellos esta alineado a una
 * pagina. Una pagina no puede ser medio ejecutable.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;
    __text_start = .;

    /* --- Tramo 1: solo lectura y ejecutable --- */
    .text : {
        KEEP(*(.text.header))   /* la cabecera, lo primero de todo */
        KEEP(*(.text.start))    /* _start detras: el kernel ya no lo supone,
                                   lo lee de la cabecera, pero se lee mejor */
        *(.text .text.*)
    }
    .rodata : ALIGN(8) { *(.rodata .rodata.*) }

    . = ALIGN(4096);            /* el corte de permisos, en frontera de pagina */
    __text_end = .;

    /* --- Tramo 2: lectura/escritura, nunca ejecutable --- */
    __data_start = .;
    .data : { *(.data .data.*) }
    __data_end = .;             /* sin alinear: esto marca el final real de
                                   la imagen, y objcopy no escribe relleno
                                   detras del ultimo byte con contenido */

    /* .bss no ocupa sitio en la imagen: el kernel la entrega ya a cero,
     * porque las paginas que reparte el PMM vienen limpias. */
    .bss (NOLOAD) : ALIGN(16) {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
    }
    __bss_end = .;

    /DISCARD/ : { *(.comment) *(.note.*) }
}
