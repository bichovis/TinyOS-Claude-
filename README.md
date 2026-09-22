# TinyOS

Microkernel para Raspberry Pi 3B (BCM2837, Cortex-A53, AArch64) escrito en
ensamblador ARM64 + C, desde cero y sin dependencias externas.

## Requisitos

    brew install aarch64-elf-gcc qemu

## Uso

    make          # compila build/kernel8.img
    make run      # arranca en QEMU (salir: Ctrl-A, luego X)
    make debug    # arranca parado esperando gdb en :1234
    make dump     # desensambla el kernel
    make clean

## En hardware real

Formatea una SD en FAT32 y copia:

  - el firmware de la Pi (`bootcode.bin`, `start.elf`, `fixup.dat`) desde
    https://github.com/raspberrypi/firmware/tree/master/boot
  - `config.txt` (incluido en este repo)
  - `build/kernel8.img`

Conecta un adaptador USB-serie a GND (pin 6), GPIO14/TXD (pin 8) y
GPIO15/RXD (pin 10) a 115200 8N1.

## Estado

| Paso | Contenido                                   | Estado |
|------|---------------------------------------------|--------|
| 1    | Arranque, stack, .bss, consola serie PL011  | hecho  |
| 2    | Bajada a EL1, vectores de excepción         | hecho  |
| 3    | Temporizador e interrupciones               | hecho  |
| 4    | MMU, paginación, gestor de memoria física   | —      |
| 5    | Hilos y planificador                        | —      |
| 6    | Espacios de usuario, syscalls, IPC          | —      |
| 7    | Drivers en espacio de usuario               | —      |

## Estructura

    boot.S       punto de entrada: aparca cores 1-3, baja EL3/EL2 -> EL1,
                 stack, .bss, y salta a C
    vectors.S    tabla de 16 vectores de excepcion + guardado de contexto
    exception.c  decodifica ESR_EL1 y vuelca el estado; panic()
    linker.ld    mapa de memoria (carga en 0x80000)
    irq.c        los dos controladores de interrupcion del BCM2837
    timer.c      temporizador generico de ARM: tick de 100 Hz
    uart.c       driver PL011: salida por polling, entrada por interrupcion
    kernel.c     kernel_main
