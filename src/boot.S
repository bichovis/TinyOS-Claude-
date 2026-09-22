/* boot.S - Punto de entrada de TinyOS (AArch64, Cortex-A53)
 *
 * Estado de la maquina cuando el firmware nos cede el control:
 *   - MMU desactivada, cachés desactivadas
 *   - x0 = puntero al device tree (dtb), x1..x3 = 0
 *   - Los 4 nucleos arrancan (o estan parados en una spin-table)
 *   - NO hay stack: no podemos llamar a codigo C todavia
 */

.section ".text.boot"        // seccion que el linker coloca en 0x80000
.global _start

_start:
    // --- 1. Aparcar los nucleos 1, 2 y 3 ---------------------------------
    // MPIDR_EL1 identifica el nucleo actual. Los bits [7:0] son el "Aff0",
    // es decir el numero de nucleo (0..3 en la Pi 3B).
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbz     x1, core0_start          // si soy el nucleo 0 -> sigo arrancando

park_core:
    wfe                              // Wait For Event: duerme, consumo casi cero
    b       park_core                // (los despertaremos en un paso futuro)

core0_start:
    // --- 2. Instalar el stack --------------------------------------------
    // El stack crece hacia abajo desde __stack_top (definido en linker.ld).
    ldr     x1, =__stack_top
    mov     sp, x1

    // --- 3. Poner .bss a cero --------------------------------------------
    // El estandar de C exige que las globales sin inicializar valgan 0.
    // Nadie lo hace por nosotros: el binario ni siquiera contiene esos bytes.
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
bss_loop:
    cmp     x1, x2
    b.hs    bss_done                 // hs = higher-or-same (sin signo)
    str     xzr, [x1], #8            // xzr = registro que siempre vale 0
    b       bss_loop                 // [x1], #8 = escribe y luego x1 += 8
bss_done:

    // --- 4. Saltar a C ----------------------------------------------------
    // x0 sigue conteniendo el puntero al dtb -> llega como 1er argumento.
    bl      kernel_main

    // --- 5. Si kernel_main retorna, aparcamos este nucleo tambien ---------
halt:
    wfe
    b       halt
