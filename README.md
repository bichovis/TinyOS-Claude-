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

La Pi no arranca sola: la que manda es la GPU. Su ROM carga `bootcode.bin`,
que carga `start.elf`, que lee `config.txt`, reparte la RAM entre GPU y CPU,
copia `kernel8.img` en 0x80000 y recien entonces suelta los cuatro nucleos
ARM. Todo eso vive en una particion FAT32 de la SD.

    make firmware   # baja el firmware de Broadcom (una vez, con red)
    make sdcard     # deja en build/sdcard todo lo que hay que copiar
    make sd SD=/Volumes/TUSD    # y lo copia a una SD ya montada

(el firmware se queda en `build/sdcard`, asi que `make clean` obliga a
volver a bajarlo)

Para verlo, un adaptador USB-serie a GND (pin 6), GPIO14/TXD (pin 8) y
GPIO15/RXD (pin 10), a 115200 8N1:

    screen /dev/tty.usbserial-XXXX 115200

Tres cosas que QEMU perdona y el silicio no:

  - **Las caches.** Al encender SCTLR_EL1.C, las lineas que la cache tuviera
    de antes pasan a valer como verdad y pueden pisar la RAM. Hay que
    invalidarla a mano recorriendo niveles, conjuntos y vias (`src/cache.S`).
  - **La RAM no es toda tuya.** La GPU se queda un trozo del final. Cuanto,
    lo dice ella por el buzon (`src/mbox.c`); repartir paginas por encima de
    ese limite es corromper su memoria.
  - **La UART del conector.** La PL011 esta cableada al Bluetooth; por los
    pines sale la mini-UART, que es otro periferico. `dtoverlay=disable-bt`
    las intercambia, y para que esa linea surta efecto el firmware necesita
    tener el device tree de la placa en la SD.

## Estado

| Paso | Contenido                                   | Estado |
|------|---------------------------------------------|--------|
| 1    | Arranque, stack, .bss, consola serie PL011  | hecho  |
| 2    | Bajada a EL1, vectores de excepción         | hecho  |
| 3    | Temporizador e interrupciones               | hecho  |
| 4    | MMU, paginación, gestor de memoria física   | hecho  |
| 5    | Hilos y planificador                        | hecho  |
| 6    | Sincronización: colas de espera, mutex, canales | hecho |
| 7    | Procesos en EL0 y llamadas al sistema       | hecho  |
| 8    | IPC por puertos y driver de consola en EL0  | hecho  |
| 9    | Arranque en hardware real: buzon, caches, SD | hecho  |
| 10   | Kernel en alto (TTBR1) y ASIDs              | —      |

## Estructura

    boot.S       punto de entrada: aparca cores 1-3, baja EL3/EL2 -> EL1,
                 stack, .bss, y salta a C
    vectors.S    tabla de 16 vectores de excepcion + guardado de contexto
    exception.c  decodifica ESR_EL1 y vuelca el estado; panic()
    linker.ld    mapa de memoria (carga en 0x80000)
    sched.c      hilos del kernel y planificador round-robin
    sync.c       colas de espera, mutex, semaforos y canales de mensajes
    syscall.c    despacho de las llamadas al sistema desde EL0
    ipc.c        puertos de mensajes entre procesos
    user/        programas de usuario, compilados aparte y empotrados:
                 hello.c     usa syscalls directas
                 conserver.c driver de la UART en EL0, sirve el puerto 0
                 client.c    imprime mandando mensajes al servidor
    tools/       bin2c.py           binario de usuario -> array de C
                 fetch-firmware.sh  baja el firmware de Broadcom para la SD
    config.txt   lo que la GPU lee antes de arrancar la CPU
    switch.S     cambio de contexto (solo registros callee-saved)
    pmm.c        reparte la RAM en paginas de 4 KB (bitmap)
    vmm.c        tablas de traduccion de 3 niveles y encendido de la MMU
    irq.c        los dos controladores de interrupcion del BCM2837
    mbox.c       buzon de la VideoCore: le pregunta a la GPU cuanta RAM hay
    cache.S      invalidacion de la cache de datos antes de encender la MMU
    timer.c      temporizador generico de ARM: tick de 100 Hz
    uart.c       driver PL011: salida por polling, entrada por interrupcion
    kernel.c     kernel_main

## Limitaciones conocidas

- Los procesos viven a partir de 2 GB porque el kernel ocupa las dos primeras
  entradas L1 de cada espacio, compartidas. El split TTBR0/TTBR1 (paso 10)
  liberaria el rango bajo y separaria del todo los dos mundos.
- Sin ASIDs: cada cambio de espacio de direcciones invalida la TLB entera.
- El cargador de procesos mapea toda la imagen como codigo de solo lectura,
  asi que un programa de usuario no puede tener variables globales
  escribibles; solo pila.
- Un proceso que muere queda zombi y no se liberan ni su pgd ni sus paginas:
  falta un recolector.
- El kernel conserva su propio driver de UART para depuracion, asi que
  cuando el servidor de consola esta activo hay dos escritores sobre el
  mismo hardware y el texto puede entremezclarse. Un microkernel estricto
  dejaria en el kernel, como mucho, una salida de panico.
