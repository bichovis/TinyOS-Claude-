/* fpu.S - Guardar y restaurar el estado de coma flotante
 *
 * Son 32 registros de 128 bits (q0-q31) mas dos de control, FPSR y FPCR:
 * 528 bytes. Para comparar, el cambio de contexto entero de un hilo -lo que
 * salva switch.S- son 104. Cinco veces mas caro, y por eso este fichero no
 * se llama en cada cambio de contexto sino solo cuando hace falta.
 *
 * El kernel se compila con -mgeneral-regs-only, asi que estas
 * instrucciones no pueden salir de C: tienen que estar aqui.
 */
.arch armv8-a+fp+simd

/* void fp_save(void *dst)  -  dst tiene que estar alineado a 16 */
.global fp_save
.type   fp_save, %function
fp_save:
    stp     q0,  q1,  [x0, #16 *  0]
    stp     q2,  q3,  [x0, #16 *  2]
    stp     q4,  q5,  [x0, #16 *  4]
    stp     q6,  q7,  [x0, #16 *  6]
    stp     q8,  q9,  [x0, #16 *  8]
    stp     q10, q11, [x0, #16 * 10]
    stp     q12, q13, [x0, #16 * 12]
    stp     q14, q15, [x0, #16 * 14]
    stp     q16, q17, [x0, #16 * 16]
    stp     q18, q19, [x0, #16 * 18]
    stp     q20, q21, [x0, #16 * 20]
    stp     q22, q23, [x0, #16 * 22]
    stp     q24, q25, [x0, #16 * 24]
    stp     q26, q27, [x0, #16 * 26]
    stp     q28, q29, [x0, #16 * 28]
    stp     q30, q31, [x0, #16 * 30]

    /* Y los dos de control. FPSR lleva las banderas acumuladas -division
     * por cero, desbordamiento- y FPCR el modo de redondeo. Olvidarlos
     * seria un fallo dificil de ver: el programa daria numeros correctos
     * casi siempre. */
    /* Sueltos y no con un stp: el desplazamiento de stp se codifica en 7
     * bits con signo por 8, o sea hasta 504, y aqui hacen falta 512. str
     * lo codifica en 12 bits sin signo por 8 y llega de sobra. Dos
     * instrucciones que parecen la misma con limites distintos. */
    mrs     x1, fpsr
    mrs     x2, fpcr
    str     x1, [x0, #512]
    str     x2, [x0, #520]
    ret
.size fp_save, . - fp_save

/* void fp_restore(const void *src) */
.global fp_restore
.type   fp_restore, %function
fp_restore:
    ldp     q0,  q1,  [x0, #16 *  0]
    ldp     q2,  q3,  [x0, #16 *  2]
    ldp     q4,  q5,  [x0, #16 *  4]
    ldp     q6,  q7,  [x0, #16 *  6]
    ldp     q8,  q9,  [x0, #16 *  8]
    ldp     q10, q11, [x0, #16 * 10]
    ldp     q12, q13, [x0, #16 * 12]
    ldp     q14, q15, [x0, #16 * 14]
    ldp     q16, q17, [x0, #16 * 16]
    ldp     q18, q19, [x0, #16 * 18]
    ldp     q20, q21, [x0, #16 * 20]
    ldp     q22, q23, [x0, #16 * 22]
    ldp     q24, q25, [x0, #16 * 24]
    ldp     q26, q27, [x0, #16 * 26]
    ldp     q28, q29, [x0, #16 * 28]
    ldp     q30, q31, [x0, #16 * 30]

    ldr     x1, [x0, #512]
    ldr     x2, [x0, #520]
    msr     fpsr, x1
    msr     fpcr, x2
    ret
.size fp_restore, . - fp_restore

/* void fp_hw_enable(void) / void fp_hw_disable(void)
 *
 * CPACR_EL1.FPEN, bits [21:20]:
 *     0b00  atrapar en EL0 y en EL1
 *     0b01  atrapar solo en EL0
 *     0b11  permitir en los dos
 *
 * El isb no es opcional. Cambiar CPACR afecta a como se DECODIFICAN las
 * instrucciones siguientes, y la CPU ya tiene varias en vuelo: sin la
 * barrera, la primera instruccion de FP de despues puede haber pasado por
 * el decodificador cuando FPEN todavia valia cero, y atrapar otra vez.
 */
.global fp_hw_enable
.type   fp_hw_enable, %function
fp_hw_enable:
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb
    ret
.size fp_hw_enable, . - fp_hw_enable

.global fp_hw_disable
.type   fp_hw_disable, %function
fp_hw_disable:
    mrs     x0, cpacr_el1
    bic     x0, x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb
    ret
.size fp_hw_disable, . - fp_hw_disable
