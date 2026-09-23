/* boot.S - Punto de entrada de TinyOS (AArch64, Cortex-A53)
 *
 * Estado de la maquina cuando el firmware nos cede el control:
 *   - MMU desactivada, cachés desactivadas
 *   - x0 = puntero al device tree (dtb), x1..x3 = 0
 *   - Los 4 nucleos arrancan (o estan parados en una spin-table)
 *   - NO hay stack: no podemos llamar a codigo C todavia
 *   - Nivel de privilegio: EL3 en QEMU, EL2 en una Pi 3B real
 *
 * Y una complicacion nueva: el kernel esta ENLAZADO en 0xFFFFFF8000080000,
 * pero nos han cargado en 0x80000. Mientras la MMU este apagada, ninguna
 * direccion del enlazador sirve para nada. Asi que todo lo de aqui abajo
 * trabaja en fisico, restando KERNEL_VA_BASE a mano, hasta que la MMU esta
 * encendida y podemos saltar "arriba" de una vez.
 */

// Los mismos bits que include/mm.h, repetidos aqui porque el ensamblador no
// entiende los sufijos UL de C. Si tocas uno alli, tocalo aqui.
.equ KERNEL_VA_BASE, 0xFFFFFF8000000000
.equ RAM_MAX,        0x3F000000
.equ LOCAL_PA,       0x40000000

.equ PTE_VALID,   (1 << 0)
.equ PTE_TABLE,   (1 << 1)
.equ PTE_AF,      (1 << 10)
.equ PTE_SH_IN,   (3 << 8)
.equ PTE_PXN,     (1 << 53)
.equ PTE_UXN,     (1 << 54)
.equ ATTR_DEV,    (1 << 2)      // indice 1 en MAIR: Device-nGnRE
.equ ATTR_NORM,   (2 << 2)      // indice 2 en MAIR: Normal write-back

.equ MM_RAM_RW,   (PTE_VALID | PTE_AF | PTE_SH_IN | ATTR_NORM | PTE_UXN | PTE_PXN)
.equ MM_RAM_CODE, (PTE_VALID | PTE_AF | PTE_SH_IN | ATTR_NORM | PTE_UXN)
.equ MM_DEVICE,   (PTE_VALID | PTE_AF | ATTR_DEV  | PTE_UXN | PTE_PXN)

// MAIR_EL1: los cuatro tipos de memoria, un byte cada uno
.equ MAIR_VALUE,  (0x00 << 0) | (0x04 << 8) | (0xFF << 16) | (0x44 << 24)

// TCR_EL1: 39 bits de VA en los dos lados, granulo de 4 KB, tablas
// cacheables, PA de 40 bits. Lo que YA NO esta es EPD1: TTBR1 se enciende.
//
// Dos bits que valen 0 y conviene no perder de vista, porque de ellos
// dependen los ASIDs:
//   A1 (bit 22) = 0 -> el ASID activo sale de TTBR0_EL1, no de TTBR1_EL1
//   AS (bit 36) = 0 -> el ASID son 8 bits, no 16. 256 espacios bastan.
.equ TCR_VALUE,   (25) | (1 << 8) | (1 << 10) | (3 << 12) | \
                  (25 << 16) | (1 << 24) | (1 << 26) | (3 << 28) | \
                  (2 << 30) | (2 << 32)

// ==========================================================================
//  Traza de arranque
//
//  boot.S corre antes de que exista uart.c, asi que si algo falla aqui no
//  hay forma de contarlo... salvo escribiendo a pelo en el registro de
//  datos de la PL011. En la Pi el firmware ya la deja configurada
//  (enable_uart=1 en config.txt), asi que un 'str' basta.
//
//  Es la unica herramienta que hay para depurar esta parte en hardware
//  real, donde no hay ni gdb ni printf: la maquina arranca o no arranca, y
//  sin esto no dice por que. Poner a 1 para encenderla; imprime una letra
//  por cada hito del arranque.
// ==========================================================================
#define BOOT_TRACE 0

.macro trace ch
#if BOOT_TRACE
    mov     x16, #0x1000
    movk    x16, #0x3F20, lsl #16    // 0x3F201000 = PL011 (fisico)
8\@:
    ldr     w17, [x16, #0x18]        // FR: registro de estado
    tbnz    w17, #5, 8\@b            // TXFF: la cola esta llena, esperar
    mov     w17, #\ch
    str     w17, [x16]               // DR: y va el caracter
#endif
.endm

.section ".text.boot"        // seccion que el linker coloca en 0x80000
.global _start

_start:
    // --- 1. Todos los nucleos empiezan igual -----------------------------
    // Ojo, que aqui no llega lo mismo en QEMU que en la placa:
    //
    //   QEMU     suelta los cuatro nucleos en 0x80000. Los tres de mas
    //            acaban esperando en secondary_wait, mas abajo.
    //   Pi 3B    solo suelta el nucleo 0. A los otros tres los tiene el
    //            firmware en su propia spin-table, y para sacarlos de ahi
    //            hay que escribirles una direccion en su buzon (ver
    //            src/smp.c). Cuando salen, entran por secondary_entry.
    //
    // En los dos casos el reparto se hace en at_el1, mirando MPIDR.
    b       el_descend

    // Por aqui entran los nucleos 1-3 en hardware real: recien soltados por
    // el firmware, sin pasar por nada de lo anterior y en EL2. Tienen que
    // recorrer el mismo descenso que hizo el nucleo 0.
.global secondary_entry
secondary_entry:
    b       el_descend


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
    trace 'A'                        // hemos llegado a EL1

    // x27 = el puntero al device tree, que el firmware nos dejo en x0. Hay
    // que apartarlo: mas abajo llamamos a dcache_invalidate_all, que usa x0
    // para leer CLIDR_EL1 y se lo lleva por delante.
    mov     x27, x0

    // x9 = KERNEL_VA_BASE. A partir de aqui, toda direccion que venga del
    // enlazador hay que bajarla a fisico restandole esto.
    ldr     x9, =KERNEL_VA_BASE

    // x28 = numero de nucleo, y ya no se toca en todo el arranque.
    // MPIDR_EL1 identifica el nucleo actual; los bits [7:0] son el "Aff0".
    mrs     x28, mpidr_el1
    and     x28, x28, #0xFF

    // Cada nucleo su pila: 16 KB hacia abajo desde __stack_top. El 0 se
    // lleva la de arriba del todo, el 1 la siguiente, y asi.
    // Ojo: en EL1h el registro SP activo es SP_EL1, que esta sin inicializar.
    // Escribir en 'sp' aqui escribe en SP_EL1.
    ldr     x1, =__stack_top
    sub     x1, x1, x9               // ...en fisico, que la MMU aun no esta
    lsl     x2, x28, #14             // 16 KB por nucleo
    sub     x1, x1, x2
    mov     sp, x1

    // Permitir a EL1/EL0 usar FP/SIMD sin que la CPU lo atrape.
    // Nuestro kernel compila con -mgeneral-regs-only y no los usa, pero si
    // quedan atrapados una excepcion inesperada seria dificil de diagnosticar.
    mov     x1, #(3 << 20)           // CPACR_EL1.FPEN = 0b11
    msr     cpacr_el1, x1
    isb                              // que surta efecto antes de continuar

    // AQUI NO se toca CPUECTLR_EL1.SMPEN, el bit que mete a este nucleo en
    // el dominio de coherencia, y conviene saber por que.
    //
    // Es un registro "implementation defined" (S3_1_C15_C2_1), y que EL1
    // pueda leerlo depende de lo que hayan dejado ACTLR_EL2 y ACTLR_EL3,
    // o sea del firmware. QEMU lo deja pasar; en una Pi de verdad el
    // acceso se atrapa, y una excepcion en este punto -con VBAR_EL1 aun
    // sin poner- es una muerte silenciosa: ni un caracter por la UART.
    //
    // No hace falta: el armstub del firmware ya pone SMPEN en los cuatro
    // nucleos antes de soltarlos. Este es el tipo de bit que se pone donde
    // hay privilegio para ponerlo, y no donde a uno le viene bien.

    // Y aqui se separan los caminos: el nucleo 0 construye, el resto espera.
    cbnz    x28, secondary_wait

    // --- 4. Poner .bss a cero --------------------------------------------
    // El estandar de C exige que las globales sin inicializar valgan 0.
    // Nadie lo hace por nosotros: el binario ni siquiera contiene esos bytes.
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
    sub     x1, x1, x9               // en fisico: la MMU sigue apagada
    sub     x2, x2, x9
bss_loop:
    cmp     x1, x2
    b.hs    bss_done                 // hs = higher-or-same (sin signo)
    str     xzr, [x1], #8            // xzr = registro que siempre vale 0
    b       bss_loop                 // [x1], #8 = escribe y luego x1 += 8
bss_done:
    trace 'B'                        // .bss a cero

    // --- 5. Construir las tablas de traduccion de arranque ---------------
    //
    // Tres tablas, todas en .bss (o sea, ya puestas a cero):
    //   l1_table[0]  -> l2_low     cubre 0 - 1 GB
    //   l1_table[1]  -> l2_local   cubre 1 - 2 GB
    //
    // Con 39 bits de VA, el indice L1 son los bits [38:30] de la direccion.
    // Y aqui esta el truco que hace que todo esto sea sencillo: para una VA
    // alta 0xFFFFFF80_00000000 + PA, esos bits valen exactamente PA >> 30.
    // Es decir, LA MISMA TABLA sirve de mapa de identidad por TTBR0 y de
    // mapa lineal del kernel por TTBR1. No hay que duplicar nada.
    ldr     x10, =l1_table
    sub     x10, x10, x9
    ldr     x11, =l2_low
    sub     x11, x11, x9
    ldr     x12, =l2_local
    sub     x12, x12, x9

    orr     x13, x11, #(PTE_VALID | PTE_TABLE)
    str     x13, [x10]               // l1_table[0] = l2_low
    orr     x13, x12, #(PTE_VALID | PTE_TABLE)
    str     x13, [x10, #8]           // l1_table[1] = l2_local

    // --- 0 - 1 GB en bloques de 2 MB: RAM abajo, perifericos arriba ------
    ldr     x14, =MM_RAM_RW
    ldr     x15, =MM_DEVICE
    ldr     x16, =RAM_MAX
    mov     x13, #0                  // i = 0
tbl_loop:
    lsl     x17, x13, #21            // pa = i * 2 MB
    cmp     x17, x16
    csel    x18, x14, x15, lo        // ¿RAM o periferico?
    cbnz    x13, 1f
    ldr     x18, =MM_RAM_CODE        // el bloque 0 lleva el kernel: ejecutable
1:  orr     x18, x17, x18
    str     x18, [x11], #8
    add     x13, x13, #1
    cmp     x13, #512
    b.lo    tbl_loop

    // --- 1 - 2 GB: solo el primer bloque, los "ARM local peripherals" ----
    ldr     x13, =LOCAL_PA
    orr     x13, x13, x15
    str     x13, [x12]

    // --- 6. Encender la MMU ----------------------------------------------
    // TTBR0 y TTBR1 apuntan a la MISMA tabla: abajo queda un mapa de
    // identidad (para que la instruccion siguiente al encendido siga
    // existiendo) y arriba el mapa lineal del kernel.
    trace 'C'                        // tablas construidas

    ldr     x13, =MAIR_VALUE
    msr     mair_el1, x13
    ldr     x13, =TCR_VALUE
    msr     tcr_el1, x13
    msr     ttbr0_el1, x10
    msr     ttbr1_el1, x10
    isb

    ic      iallu                    // tirar la cache de instrucciones
    tlbi    vmalle1                  // y la TLB entera
    dsb     nsh
    isb

    // Tirar lo que la cache de datos pudiera tener de antes del arranque.
    // Ahora mismo esta apagada, asi que todo lo que hemos escrito (las
    // tablas incluidas) esta en RAM de verdad y no perdemos nada. En cuanto
    // pongamos SCTLR.C, cualquier linea residual pasaria a ser valida.
    bl      dcache_invalidate_all
    ldr     x9, =KERNEL_VA_BASE      // esa rutina se lleva x9 por delante

    mrs     x13, sctlr_el1
    orr     x13, x13, #(1 << 0)      // M: MMU
    orr     x13, x13, #(1 << 2)      // C: cache de datos
    orr     x13, x13, #(1 << 12)     // I: cache de instrucciones
    msr     sctlr_el1, x13
    isb                              // <- a partir de aqui hay traduccion

    trace 'D'                        // MMU encendida (sigue valiendo la
                                     // identidad, por eso 0x3F201000 aun
                                     // se puede tocar)

    // --- 7. La mudanza ----------------------------------------------------
    // Seguimos ejecutando en 0x80xxx gracias al mapa de identidad. Este
    // salto absoluto es el que nos lleva de verdad arriba: 'ldr =' carga la
    // direccion que puso el enlazador, que es la alta.
    ldr     x13, =en_alto
    br      x13

en_alto:
    trace 'E'                        // ya ejecutando arriba

    // Ya estamos en 0xFFFFFF80_000xxxxx. El stack todavia apunta abajo.
    ldr     x1, =__stack_top
    mov     sp, x1

    // Y ahora se puede quitar el mapa de identidad: TTBR0 pasa a una tabla
    // vacia, que es lo que tendra cualquier hilo de kernel. Desde este
    // instante, tocar una direccion baja es una excepcion, que es justo lo
    // que queremos: el kernel no tiene nada que hacer en el espacio del
    // usuario salvo cuando lo pide explicitamente.
    ldr     x1, =empty_pgd
    sub     x1, x1, x9
    msr     ttbr0_el1, x1
    dsb     ish
    tlbi    vmalle1
    dsb     ish
    isb

    // --- 8. Saltar a C ----------------------------------------------------
    // El puntero al dtb, de vuelta a x0: llega como 1er argumento.
    mov     x0, x27
    bl      kernel_main

    // --- 9. Si kernel_main retorna, aparcamos este nucleo tambien ---------
halt:
    wfe
    b       halt


// ==========================================================================
//  Los nucleos 1, 2 y 3
//
//  Llegan aqui en EL1, con la MMU apagada y con su propia pila fisica ya
//  puesta, pero las tablas de traduccion todavia no existen: las esta
//  construyendo el nucleo 0. Asi que lo primero es esperar.
// ==========================================================================
secondary_wait:
    // Nuestra propia spin-table. smp_go vive en .data y NO en .bss, y eso
    // es deliberado: .data llega del fichero ya inicializada, antes de que
    // ejecute ningun nucleo. Una variable de .bss valdria basura hasta que
    // el nucleo 0 la limpiara, y aqui hay nucleos leyendola antes de eso:
    // uno podria leer un valor cualquiera distinto de cero y arrancar con
    // las tablas a medio construir.
    ldr     x1, =smp_go
    sub     x1, x1, x9
1:  wfe                              // dormido hasta que alguien haga 'sev'
    ldr     x2, [x1]
    cbz     x2, 1b

    // --- Encender la MMU, con las tablas que ya hizo el nucleo 0 --------
    // Aqui no se construye nada: se comparten. TTBR1 apunta a la misma
    // tabla que el nucleo 0, que es justo lo que hace que el kernel sea uno
    // solo visto desde los cuatro nucleos.
    ldr     x13, =MAIR_VALUE
    msr     mair_el1, x13
    ldr     x13, =TCR_VALUE
    msr     tcr_el1, x13
    ldr     x10, =l1_table
    sub     x10, x10, x9
    msr     ttbr0_el1, x10
    msr     ttbr1_el1, x10
    isb

    ic      iallu
    tlbi    vmalle1
    dsb     nsh
    isb

    bl      dcache_invalidate_all
    ldr     x9, =KERNEL_VA_BASE      // esa rutina se lleva x9 por delante

    mrs     x13, sctlr_el1
    orr     x13, x13, #(1 << 0)      // M
    orr     x13, x13, #(1 << 2)      // C
    orr     x13, x13, #(1 << 12)     // I
    msr     sctlr_el1, x13
    isb

    ldr     x13, =secondary_high
    br      x13

secondary_high:
    // La misma mudanza que hizo el nucleo 0: pila arriba y fuera el mapa
    // de identidad.
    ldr     x1, =__stack_top
    lsl     x2, x28, #14
    sub     x1, x1, x2
    mov     sp, x1

    ldr     x1, =empty_pgd
    sub     x1, x1, x9
    msr     ttbr0_el1, x1
    dsb     ish
    tlbi    vmalle1
    dsb     ish
    isb

    mov     x0, x28                  // el numero de nucleo, como argumento
    bl      secondary_main

2:  wfi                              // si vuelve, a dormir
    b       2b


// ==========================================================================
//  Las tablas de traduccion de arranque
//
//  Viven en .bss (no ocupan sitio en la imagen y boot.S las pone a cero) y
//  estan alineadas a 4 KB porque un descriptor de tabla solo guarda los bits
//  [47:12] de la direccion: los 12 de abajo tienen que ser cero.
// ==========================================================================
.section ".bss.pgtables", "aw", %nobits
.align 12

.global l1_table
l1_table:   .space 4096
.global l2_low
l2_low:     .space 4096
.global l2_local
l2_local:   .space 4096

// Tabla vacia: el TTBR0 de un hilo que no tiene espacio de usuario.
.global empty_pgd
empty_pgd:  .space 4096


// ==========================================================================
//  La senyal de arranque de los nucleos 1-3
//
//  En .data y no en .bss: asi llega del fichero valiendo cero, antes de que
//  ejecute un solo nucleo. Ver el comentario de secondary_wait.
// ==========================================================================
.section ".data.smp", "aw"
.align 3
.global smp_go
smp_go: .quad 0
