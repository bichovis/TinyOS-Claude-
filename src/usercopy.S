/* usercopy.S - Copiar entre el kernel y un proceso, pudiendo fallar
 *
 * DOS IDEAS, Y LAS DOS SON DEL HARDWARE.
 *
 * 1. ldtr y sttr. Son "load/store unprivileged": ejecutandose en EL1,
 *    hacen el acceso con los permisos de EL0. Si la direccion no es del
 *    proceso -porque apunta al kernel, o a nada- la MMU lo rechaza igual
 *    que se lo rechazaria a el.
 *
 *    Eso cambia de sitio una responsabilidad. Hasta ahora el kernel
 *    COMPROBABA a mano que el puntero cayera dentro del espacio de
 *    usuario antes de tocarlo; ahora lo comprueba el silicio, en cada
 *    acceso, sin que se pueda olvidar. Un kernel que valida punteros con
 *    ifs acaba teniendo un if que falta.
 *
 * 2. La tabla de arreglos. Si el acceso falla, el manejador de
 *    excepciones busca la direccion de la instruccion en una tabla y, si
 *    la encuentra, cambia el punto de retorno por el que dice la tabla.
 *    El bucle "vuelve" a una etiqueta de error en vez de reventar.
 *
 *    Es la unica forma de fallo que a este kernel le faltaba por saber
 *    manejar: la suya propia, a proposito y con recuperacion.
 *
 * Las dos devuelven cuantos bytes NO se pudieron copiar. Cero es todo
 * bien, y ese convenio -en vez de "cuantos si"- es el de Linux por un
 * motivo practico: el caso normal comprueba contra cero.
 */

/* Anyadir un par a la tabla sin movernos de sitio: .pushsection escribe
 * en otra seccion y .popsection vuelve aqui. */
.macro  ARREGLO  instruccion, destino
    .pushsection .ex_table, "a"
    .align  3
    .quad   \instruccion
    .quad   \destino
    .popsection
.endm

/* uint64_t copy_from_user(void *dst, uint64_t uva, uint64_t n) */
.global copy_from_user
.type   copy_from_user, %function
copy_from_user:
    cbz     x2, 2f
1:  ldtrb   w3, [x1]                /* <- con permisos de EL0 */
    strb    w3, [x0], #1
    add     x1, x1, #1
    subs    x2, x2, #1
    b.ne    1b
2:  mov     x0, x2                  /* lo que quedo sin copiar */
    ret

3:  mov     x0, x2                  /* aqui salta el manejador si 1b falla */
    ret

    ARREGLO 1b, 3b
.size copy_from_user, . - copy_from_user

/* uint64_t copy_to_user(uint64_t uva, const void *src, uint64_t n) */
.global copy_to_user
.type   copy_to_user, %function
copy_to_user:
    cbz     x2, 2f
1:  ldrb    w3, [x1], #1
4:  sttrb   w3, [x0]                /* <- con permisos de EL0 */
    add     x0, x0, #1
    subs    x2, x2, #1
    b.ne    1b
2:  mov     x0, x2
    ret

3:  mov     x0, x2
    ret

    ARREGLO 4b, 3b
.size copy_to_user, . - copy_to_user
