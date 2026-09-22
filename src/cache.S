/* cache.S - Invalidacion de la cache de datos
 *
 * Antes de encender SCTLR_EL1.C hay que asegurarse de que la cache de datos
 * no contiene lineas indeterminadas: en el instante en que se activa, esas
 * lineas pasan a ser "verdad" y pueden pisar lo que hay en RAM. QEMU no
 * emula caches y perdona este olvido; el silicio no.
 *
 * No existe una instruccion "invalida toda la cache": hay que recorrer a
 * mano cada nivel, cada conjunto y cada via, y preguntarle a la propia CPU
 * su geometria. Esta rutina es un clasico que se repite en todos los
 * kernels de ARM64 porque no hay otra forma de escribirla.
 *
 *   CLIDR_EL1   dice cuantos niveles de cache hay y de que tipo
 *   CSSELR_EL1  selecciona el nivel que queremos consultar
 *   CCSIDR_EL1  da la geometria del nivel seleccionado
 *   dc isw      invalida una linea por (conjunto, via)
 *
 * Usamos 'invalidate' y no 'clean+invalidate' a proposito: con las caches
 * apagadas, todo lo que hemos escrito ha ido directo a RAM, asi que no hay
 * nada sucio que valga la pena conservar.
 */

.global dcache_invalidate_all
dcache_invalidate_all:
    mrs     x0, clidr_el1
    and     w3, w0, #0x07000000         // LoC: nivel de coherencia
    lsr     w3, w3, #23                 // w3 = LoC * 2
    cbz     w3, 5f                      // no hay caches: nada que hacer
    mov     w10, #0                     // w10 = 2 * nivel actual
    mov     w8, #1

1:  add     w2, w10, w10, lsr #1        // w2 = 3 * nivel
    lsr     w1, w0, w2
    and     w1, w1, #7                  // tipo de cache de este nivel
    cmp     w1, #2
    b.lt    4f                          // <2: no tiene cache de datos

    msr     csselr_el1, x10             // seleccionar el nivel
    isb                                 // ...y esperar a que surta efecto
    mrs     x1, ccsidr_el1              // leer su geometria

    and     w2, w1, #7                  // log2(tamano de linea) - 4
    add     w2, w2, #4                  // desplazamiento del conjunto
    ubfx    w4, w1, #3, #10             // numero de vias - 1
    clz     w5, w4                      // posicion del campo "via"
    lsl     w9, w4, w5
    lsl     w16, w8, w5

2:  ubfx    w7, w1, #13, #15            // numero de conjuntos - 1
3:  lsl     w11, w7, w2
    orr     w11, w9, w11                // via
    orr     w11, w11, w10               // nivel
    dc      isw, x11                    // invalidar esa linea
    subs    w7, w7, #1
    b.ge    3b
    subs    x9, x9, x16
    b.ge    2b

4:  add     w10, w10, #2                // siguiente nivel
    cmp     w3, w10
    dsb     sy
    b.gt    1b

5:  mov     x10, #0
    msr     csselr_el1, x10             // dejar CSSELR como estaba
    dsb     sy
    isb
    ret
