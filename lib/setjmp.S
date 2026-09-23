/* setjmp.S - Guardar y restaurar un punto de retorno
 *
 * Lo que se guarda es exactamente lo que el ABI de AArch64 obliga a
 * conservar entre llamadas: x19-x28, x29 (el marco), x30 (el retorno), el
 * puntero de pila, y d8-d15. Lo demas no hace falta, porque quien llame a
 * setjmp ya da por hecho que puede cambiar.
 *
 * Es la misma lista que salva switch.S al cambiar de hilo, mas los ocho
 * registros de coma flotante. Y no es casualidad: las dos cosas son lo
 * mismo -congelar un punto de ejecucion para volver a el- con la
 * diferencia de que un cambio de contexto vuelve solo una vez y esto
 * puede volver muchas.
 *
 * OJO A LOS d8-d15. Guardarlos toca la FPU, y en este sistema la FPU esta
 * apagada hasta que alguien la usa (paso 30). O sea que el primer setjmp
 * de un proceso provoca una excepcion, le reserva sus 528 bytes y sigue.
 * Cuesta una trampa y es correcto; no guardarlos seria mas barato y
 * estaria mal en cuanto alguien hiciera cuentas con decimales alrededor
 * de un longjmp.
 */
.arch armv8-a+fp+simd

/* int setjmp(jmp_buf destino) */
.global setjmp
.type   setjmp, %function
setjmp:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]      /* marco y direccion de retorno */

    mov     x1, sp
    str     x1, [x0, #96]

    stp     d8,  d9,  [x0, #112]
    stp     d10, d11, [x0, #128]
    stp     d12, d13, [x0, #144]
    stp     d14, d15, [x0, #160]

    mov     w0, #0                   /* la PRIMERA vez se devuelve cero */
    ret
.size setjmp, . - setjmp

/* void longjmp(jmp_buf destino, int valor) */
.global longjmp
.type   longjmp, %function
longjmp:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]

    ldr     x2, [x0, #96]
    mov     sp, x2                   /* a partir de aqui, otra pila */

    ldp     d8,  d9,  [x0, #112]
    ldp     d10, d11, [x0, #128]
    ldp     d12, d13, [x0, #144]
    ldp     d14, d15, [x0, #160]

    /* longjmp(buf, 0) tiene que devolver 1, no 0: si devolviera cero, el
     * que llamo a setjmp no podria distinguir la vuelta de verdad de la
     * primera. Lo dice el estandar y es la clase de detalle que, sin
     * arreglar, hace que un bucle de reintentos no termine nunca. */
    cmp     w1, #0
    csinc   w0, w1, wzr, ne

    /* Y aqui esta el truco: x30 ya no es quien nos llamo, es quien llamo
     * a setjmp. Este "ret" aterriza DENTRO de setjmp, justo detras de su
     * propio ret, y setjmp devuelve por segunda vez. */
    ret
.size longjmp, . - longjmp
