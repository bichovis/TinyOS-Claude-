/* switch.S - Cambio de contexto entre hilos
 *
 * void cpu_switch_to(struct task *prev, struct task *next)
 *
 * Solo salva los registros "callee-saved" del ABI de AArch64 (x19-x28, x29,
 * sp, x30). El resto no hace falta: para el compilador esto es una llamada a
 * funcion normal, asi que ya ha puesto a salvo lo que le importaba.
 *
 * El truco esta en el 'ret' final. Carga x30 desde el contexto de 'next', asi
 * que no vuelve a quien nos llamo: vuelve a donde 'next' se quedo la ultima
 * vez que paso por aqui. Cada hilo cree que cpu_switch_to simplemente ha
 * tardado un rato en volver.
 */

.global cpu_switch_to
cpu_switch_to:
    // x0 = prev, x1 = next. struct cpu_context esta al principio de ambos.
    mov     x9,  sp
    stp     x19, x20, [x0, #16 * 0]
    stp     x21, x22, [x0, #16 * 1]
    stp     x23, x24, [x0, #16 * 2]
    stp     x25, x26, [x0, #16 * 3]
    stp     x27, x28, [x0, #16 * 4]
    stp     x29, x9,  [x0, #16 * 5]    // fp y sp
    str     x30,      [x0, #16 * 6]    // por donde retomar

    ldp     x19, x20, [x1, #16 * 0]
    ldp     x21, x22, [x1, #16 * 1]
    ldp     x23, x24, [x1, #16 * 2]
    ldp     x25, x26, [x1, #16 * 3]
    ldp     x27, x28, [x1, #16 * 4]
    ldp     x29, x9,  [x1, #16 * 5]
    ldr     x30,      [x1, #16 * 6]
    mov     sp,  x9                    // a partir de aqui, otra pila
    ret                                // salta al x30 de 'next'


/* Primera vez que un hilo recien creado toma la CPU.
 *
 * task_create deja ctx.pc apuntando aqui, ctx.x19 = funcion y ctx.x20 = arg,
 * asi que el 'ret' de cpu_switch_to aterriza en esta etiqueta con todo listo.
 */
.global ret_from_fork
ret_from_fork:
    // Nacemos dentro del handler de la interrupcion del timer, que corre con
    // las IRQ tapadas por hardware. Como este camino no pasa por kernel_exit
    // (no hay 'eret' que restaure SPSR), hay que destaparlas a mano o el
    // hilo nuevo correria para siempre sin que nadie pudiera desalojarlo.
    msr     daifclr, #2

    mov     x0,  x20                   // argumento
    blr     x19                        // llamar a la funcion del hilo
    bl      task_exit                  // si retorna, el hilo termina
1:  b       1b                         // task_exit no vuelve
