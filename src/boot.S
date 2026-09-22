/* boot.S - Punto de entrada de TinyOS (AArch64, Cortex-A53)
 *
 * Estado de la maquina cuando el firmware nos cede el control:
 *   - MMU desactivada, cachés desactivadas
 *   - x0 = puntero al device tree (dtb), x1..x3 = 0
 *   - Los 4 nucleos arrancan (o estan parados en una spin-table)
 *   - NO hay stack: no podemos llamar a codigo C todavia
 *   - Nivel de privilegio: EL3 en QEMU, EL2 en una Pi 3B real
 */

.section ".text.boot"        // seccion que el linker coloca en 0x80000
.global _start

_start:
    // --- 1. Aparcar los nucleos 1, 2 y 3 ---------------------------------
    // MPIDR_EL1 identifica el nucleo actual. Los bits [7:0] son el "Aff0",
    // es decir el numero de nucleo (0..3 en la Pi 3B).
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbz     x1, el_descend           // si soy el nucleo 0 -> sigo arrancando

park_core:
    wfe                              // Wait For Event: duerme, consumo casi cero
    b       park_core                // (los despertaremos en un paso futuro)


    // --- 2. Bajar hasta EL1 ----------------------------------------------
    // No existe instruccion "bajar de nivel": se usa 'eret', el retorno de
    // excepcion. Preparamos ELR (a donde ir) y SPSR (como llegar) y saltamos.
el_descend:
    mrs     x1, CurrentEL
    lsr     x1, x1, #2               // CurrentEL guarda el nivel en bits [3:2]
    cmp     x1, #3
    b.eq    from_el3
    cmp     x1, #2
    b.eq    from_el2
    b       at_el1                   // ya estabamos en EL1: nada que hacer

    // ---- Tramo EL3 -> EL2 (solo lo recorre QEMU) ------------------------
from_el3:
    // SCR_EL3: como se comporta el mundo por debajo de EL3.
    //   bit 0  NS  = 1 -> los niveles inferiores son "non-secure"
    //   bit 8  HCE = 1 -> habilita la instruccion HVC (llamada al hipervisor)
    //   bit 10 RW  = 1 -> EL2 ejecuta AArch64 (si no, seria AArch32)
    mov     x1, #(1 << 10)
    orr     x1, x1, #(1 << 0)
    orr     x1, x1, #(1 << 8)
    msr     scr_el3, x1

    // SPSR_EL3: el estado con el que "volvemos" a EL2.
    //   0x3C9 = DAIF a 1 (todas las interrupciones enmascaradas)
    //         + M[3:0] = 0b1001 = EL2h  ("h" = usa su propio stack, SP_EL2)
    mov     x1, #0x3C9
    msr     spsr_el3, x1

    adr     x1, from_el2             // direccion a la que "retornamos"
    msr     elr_el3, x1
    eret                             // <-- aterrizamos en from_el2, ya en EL2

    // ---- Tramo EL2 -> EL1 (lo recorren QEMU y la placa real) ------------
from_el2:
    // HCR_EL2.RW (bit 31) = 1 -> EL1 ejecuta AArch64.
    // Si se queda a 0 el 'eret' nos dejaria en EL1 modo ARM de 32 bits y
    // todo nuestro codigo seria basura. Es el bit mas facil de olvidar.
    mov     x1, #(1 << 31)
    msr     hcr_el2, x1

    // SCTLR_EL1: registro de control del sistema del nivel al que vamos.
    // Contiene basura al arrancar, asi que lo fijamos a un valor conocido:
    // MMU off, cache de datos off, cache de instrucciones off, y los bits
    // RES1 que la arquitectura obliga a mantener a 1. Eso es 0x30D00800.
    ldr     x1, =0x30D00800
    msr     sctlr_el1, x1

    // Dar a EL1 acceso a los contadores y temporizadores fisicos, que por
    // defecto atrapa EL2. Sin esto, el paso 3 (timer) fallaria.
    mrs     x1, cnthctl_el2
    orr     x1, x1, #3               // EL1PCTEN | EL1PCEN
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr         // sin desplazamiento de reloj virtual

    // SPSR_EL2: 0x3C5 = DAIF enmascarado + M[3:0] = 0b0101 = EL1h
    mov     x1, #0x3C5
    msr     spsr_el2, x1

    adr     x1, at_el1
    msr     elr_el2, x1
    eret                             // <-- aterrizamos en at_el1, ya en EL1


    // --- 3. Ya en EL1: instalar el stack ---------------------------------
at_el1:
    // Ojo: en EL1h el registro SP activo es SP_EL1, que esta sin inicializar.
    // Escribir en 'sp' aqui escribe en SP_EL1.
    ldr     x1, =__stack_top
    mov     sp, x1

    // Permitir a EL1/EL0 usar FP/SIMD sin que la CPU lo atrape.
    // Nuestro kernel compila con -mgeneral-regs-only y no los usa, pero si
    // quedan atrapados una excepcion inesperada seria dificil de diagnosticar.
    mov     x1, #(3 << 20)           // CPACR_EL1.FPEN = 0b11
    msr     cpacr_el1, x1
    isb                              // que surta efecto antes de continuar

    // --- 4. Poner .bss a cero --------------------------------------------
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

    // --- 5. Saltar a C ----------------------------------------------------
    // x0 sigue conteniendo el puntero al dtb -> llega como 1er argumento.
    bl      kernel_main

    // --- 6. Si kernel_main retorna, aparcamos este nucleo tambien ---------
halt:
    wfe
    b       halt
