/* vectors.S - Tabla de vectores de excepcion de AArch64 (EL1)
 *
 * Cuando ocurre una excepcion la CPU NO busca una direccion en una tabla de
 * punteros (eso es x86). Salta a VBAR_EL1 + un desplazamiento fijo, y alli
 * tiene que haber CODIGO. Cada hueco mide 128 bytes (32 instrucciones).
 *
 * La tabla tiene 16 huecos = 2 KB, y debe estar alineada a 2 KB:
 *
 *   +0x000  Excepcion del MISMO nivel, usando SP_EL0        (EL1t)
 *   +0x200  Excepcion del MISMO nivel, usando SP_EL1        (EL1h) <- el normal
 *   +0x400  Excepcion de un nivel INFERIOR en AArch64       (EL0 de 64 bits)
 *   +0x600  Excepcion de un nivel INFERIOR en AArch32       (EL0 de 32 bits)
 *
 * y dentro de cada grupo, cuatro causas de 128 bytes:
 *   +0x00 Synchronous  (syscall, fallo de pagina, instruccion ilegal...)
 *   +0x80 IRQ          (interrupcion normal)
 *   +0x100 FIQ         (interrupcion rapida)
 *   +0x180 SError      (error asincrono del bus: memoria que no responde)
 */

/* --- Guardar el contexto del codigo interrumpido -----------------------
 * Reservamos 272 bytes en el stack y volcamos ahi los 31 registros de
 * proposito general mas el estado de la excepcion. Debe coincidir EXACTAMENTE
 * con 'struct trap_frame' de include/exception.h.
 */
.macro kernel_entry
    sub     sp,  sp,  #288
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]

    // Ya tenemos x21..x24 a salvo: podemos usarlos como temporales.
    mrs     x21, elr_el1             // direccion que se estaba ejecutando
    mrs     x22, spsr_el1            // estado del procesador al interrumpirse
    mrs     x23, esr_el1             // "Exception Syndrome": por que ha pasado
    mrs     x24, sp_el0              // pila del proceso de usuario
    stp     x30, x21, [sp, #16 * 15]
    stp     x22, x23, [sp, #16 * 16]
    str     x24,      [sp, #16 * 17]
.endm

/* --- Restaurar el contexto y volver ------------------------------------ */
.macro kernel_exit
    ldr     x24,      [sp, #16 * 17]
    msr     sp_el0,   x24            // devolverle su pila al proceso
    ldp     x22, x23, [sp, #16 * 16]
    ldp     x30, x21, [sp, #16 * 15]
    msr     elr_el1,  x21            // permite al handler C modificar el PC
    msr     spsr_el1, x22

    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]  // sobrescribe el temporal: correcto
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    add     sp,  sp,  #288
    eret                             // vuelve a ELR_EL1 con el estado SPSR_EL1
.endm

/* --- Cuerpo comun de cada excepcion ------------------------------------
 * x0 = puntero al trap_frame (que es el propio sp), x1 = indice de vector.
 */
.macro handle_exception, index
    kernel_entry
    mov     x0, sp
    mov     x1, #\index
    bl      exception_dispatch
    kernel_exit
.endm

/* --- Una entrada de la tabla -------------------------------------------
 * Solo cabe un salto: .align 7 la coloca en el siguiente multiplo de 128.
 */
.macro ventry, label
    .align  7
    b       \label
.endm

/* ======================= LA TABLA ======================= */
.section ".text"
.align  11                           // 2^11 = 2048: alineacion obligatoria
.global vector_table
vector_table:
    /* Mismo nivel (EL1), usando SP_EL0 - no deberia pasarnos nunca */
    ventry  el1t_sync
    ventry  el1t_irq
    ventry  el1t_fiq
    ventry  el1t_serror

    /* Mismo nivel (EL1), usando SP_EL1 - el caso habitual del kernel */
    ventry  el1h_sync
    ventry  el1h_irq
    ventry  el1h_fiq
    ventry  el1h_serror

    /* Desde EL0 en AArch64 - syscalls y fallos de procesos de usuario */
    ventry  el0_64_sync
    ventry  el0_64_irq
    ventry  el0_64_fiq
    ventry  el0_64_serror

    /* Desde EL0 en AArch32 - no soportamos procesos de 32 bits */
    ventry  el0_32_sync
    ventry  el0_32_irq
    ventry  el0_32_fiq
    ventry  el0_32_serror

/* ================== LOS 16 MANEJADORES ================== */
el1t_sync:      handle_exception  0
el1t_irq:       handle_exception  1
el1t_fiq:       handle_exception  2
el1t_serror:    handle_exception  3
el1h_sync:      handle_exception  4
el1h_irq:       handle_exception  5
el1h_fiq:       handle_exception  6
el1h_serror:    handle_exception  7
el0_64_sync:    handle_exception  8
el0_64_irq:     handle_exception  9
el0_64_fiq:     handle_exception 10
el0_64_serror:  handle_exception 11
el0_32_sync:    handle_exception 12
el0_32_irq:     handle_exception 13
el0_32_fiq:     handle_exception 14
el0_32_serror:  handle_exception 15


/* Primera entrada a EL0 de un proceso recien creado.
 *
 * task_create_user() deja un trap_frame fabricado a mano en el tope de la
 * pila de kernel del proceso, con ELR = punto de entrada, SPSR = EL0t y
 * SP_EL0 = pila de usuario. Aqui solo hay que desapilarlo: el 'eret' de
 * kernel_exit hace el resto. Es el mismo mecanismo con el que bajamos de
 * EL3 a EL1 en el arranque, y el mismo por el que se vuelve de una syscall:
 * para la CPU, entrar en un proceso es "retornar" a el.
 */
.global ret_to_user
ret_to_user:
    kernel_exit
