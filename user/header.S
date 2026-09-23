/* user/header.S - Los primeros 48 bytes de todo proceso
 *
 * Esto no es codigo, son datos: la cabecera que lee el kernel para saber
 * como mapear la imagen. Va en ensamblador y no en C porque los valores
 * son simbolos del enlazador, y en C no hay forma de meter uno de esos en
 * un inicializador constante.
 *
 * El campo a campo esta en include/user_abi.h.
 */

.section ".text.header", "a"
.global __user_header
__user_header:
    .ascii  "TOSU"          // magic
    .long   1               // version
    .quad   _start          // entry
    .quad   __text_start
    .quad   __text_end
    .quad   __data_end
    .quad   __bss_end
