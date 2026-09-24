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
| 10a  | Kernel en alto: split TTBR0 / TTBR1         | hecho  |
| 10b  | ASIDs: dejar de tirar la TLB entera         | hecho  |
| 11   | Recolector: devolver lo que deja un muerto  | hecho  |
| 12   | Cargador con secciones: W^X y globales      | hecho  |
| 13a  | Despertar los nucleos 1, 2 y 3              | hecho  |
| 13b  | Cerrojos de verdad y `current` por nucleo   | hecho  |
| 13c-1| Temporizador e interrupciones por nucleo    | hecho  |
| 13c-2| Que los cuatro nucleos ejecuten hilos       | hecho  |
| 14   | Pulido: IPIs, cerrojos mas finos            | hecho  |
| 15   | Servidor de ficheros: SD, FAT16 y `spawn`   | hecho  |
| 16   | Procesos en ELF y argumentos                | hecho  |
| 17   | Un interprete de ordenes en EL0             | hecho  |
| 18   | Escritura en FAT16                          | hecho  |
| 19   | kmalloc: el monton del kernel               | hecho  |
| 20   | Memoria para los procesos: sbrk y malloc    | hecho  |
| 21   | Paginacion bajo demanda: la pila crece sola | hecho  |
| 22   | Pagina de guarda en las pilas de kernel     | hecho  |
| 23   | fork con copy-on-write                      | hecho  |
| 24   | exec: convertirse en otro programa          | hecho  |
| 25   | Senyales, y Ctrl-C                          | hecho  |
| 26   | Sueño interrumpible, descriptores y tuberias| hecho  |
| 27   | El teclado, tambien en EL0                  | hecho  |
| 28   | Redireccion: < y >                          | hecho  |
| 29   | Una libc: crt0, printf y libc.a             | hecho  |
| 30   | Coma flotante, y el fallo como aviso        | hecho  |
| 31   | Subdirectorios, y el directorio actual      | hecho  |
| 32   | Nombres largos, y una interrupcion perdida  | hecho  |
| 33   | mmap: el kernel pide y un proceso contesta  | hecho  |
| 34   | exec por mmap: comprobar pasa a ser traer   | hecho  |
| 35   | Dos particiones, /boot, /usr/bin y comillas | hecho  |
| 36   | exec con argv[]: cada uno a lo suyo         | hecho  |
| 37   | Las senyales guardan la coma flotante       | hecho  |
| 38   | rmdir y mv, o deshacer lo que se hizo       | hecho  |
| 39   | Escribir nombres largos, y el ~1            | hecho  |
| 40   | Variables de entorno, y el PATH fuera       | hecho  |
| 41   | El kernel aprende a fallar y recuperarse    | hecho  |
| 42   | init: el kernel deja de saber que es un shell| hecho |
| 43   | Una sola puerta, y fuera las demostraciones | hecho  |
| 44   | Rutas largas y un reloj inventado           | hecho  |
| 45   | La superficie de fichero que espera una libc| hecho  |
| 46   | Memoria anonima, y el ultimo programa sin IPC| hecho |
| 47   | La libc crece: setjmp, qsort, strtol        | hecho  |
| 48   | errno, o hacer que el kernel diga por que   | hecho  |
| 49   | Ficheros con buffer: FILE, fopen, fprintf   | hecho  |
| 50   | Anyadir al final: O_APPEND y `>>`           | hecho  |
| 51   | Grupos de procesos, segundo plano y Ctrl-C  | hecho  |
| 52   | Detenido: Ctrl-Z, fg, bg y SIGTTIN          | hecho  |
| 53   | La disciplina de linea: eco, borrado, Ctrl-D| hecho  |

## Estructura

    boot.S       punto de entrada de los cuatro nucleos: baja EL3/EL2 -> EL1,
                 pila por nucleo, .bss, construye las tablas, ENCIENDE LA MMU
                 y salta al kernel, que vive en 0xFFFFFF8000080000
    vectors.S    tabla de 16 vectores de excepcion + guardado de contexto
    exception.c  decodifica ESR_EL1 y vuelca el estado; panic()
    linker.ld    mapa de memoria (carga en 0x80000)
    sched.c      hilos del kernel y planificador round-robin
    sync.c       colas de espera, mutex, semaforos y canales de mensajes
    syscall.c    despacho de las llamadas al sistema desde EL0
    ipc.c        puertos de mensajes entre procesos
    user/        programas de usuario, compilados aparte y empotrados:
                 hello.c     usa syscalls directas
                 sd.c        driver de la tarjeta SD (EMMC/SDHCI)
                 fs.c        servidor de ficheros FAT16, sirve el puerto 1
                 ls.c        lista la tarjeta      cat.c  vuelca un fichero
                 run.c       carga un programa de la tarjeta y lo arranca
                 sh.c        interprete de ordenes: lee, carga, arranca, espera
                 write.c     escribe un fichero      rm.c  lo borra
                 cp.c        copia uno en otro       mem.c  ensenya el monton
                 deep.c      recursion honda: se come la pila a proposito
                 forkd.c     se bifurca y mide lo que NO cuesta hacerlo
                 trap.c      atrapa Ctrl-C     kill.c  manda senyales
                 upper.c     filtro: lee de la entrada y escribe en la salida
                 wc.c        cuenta lo que le pasa por delante
                 mkdir.c     crea un directorio
                 map.c       mapea un fichero y mide cuando se lee
                 init.c      el primer proceso: arranca todo lo demas
                 fecha.c     ve y pone la hora
                 env.c       ensenya el entorno   echo.c  repite lo que le den
                 rmdir.c     borra un directorio vacio
                 mv.c        mueve y renombra: la misma operacion
                 fp.c        coma flotante: cuentas y supervivencia
                 conserver.c driver de la UART en EL0, sirve el puerto 0
                 client.c    imprime mandando mensajes al servidor
    usercopy.S   copiar de/a un proceso pudiendo fallar sin morir
    path.c       juntar el directorio actual con una ruta relativa
    fpu.c        cuando encender la FPU y a quien salvarsela
    fpu.S        los 32 registros de 128 bits, y CPACR_EL1
    lib/         la libc de los programas, archivada en build/libc.a:
                 crt0.S      _start: lo que corre ANTES de main()
                 stdio.c     printf, snprintf, putchar, puts, getchar
                 string.c    memcpy, memset, strlen, strcmp... las de siempre
                 errno.c     el motivo del ultimo fallo, y su texto
                 stdlib.c    exit, atoi, qsort, bsearch, strtol
                 setjmp.S    volver a un punto de antes
                 malloc.c    malloc/free de usuario, encima de sbrk
                 signal.c    el trampolin por donde vuelve un manejador
    tools/       bin2c.py           binario de usuario -> array de C
                 fetch-firmware.sh  baja el firmware de Broadcom para la SD
    config.txt   lo que la GPU lee antes de arrancar la CPU
    switch.S     cambio de contexto (solo registros callee-saved)
    pmm.c        reparte la RAM en paginas de 4 KB (bitmap)
    kheap.c      el monton: memoria de tamanyo arbitrario sobre el PMM
    vmm.c        tablas de traduccion de 3 niveles y espacios de usuario
    elf.h        lo justo de ELF64 para cargar un programa
    smp.c        despierta los nucleos 1-3 y el demo del contador
    spinlock.c   exclusion mutua entre nucleos (ldaxr/stlxr)
    irq.c        los dos controladores de interrupcion del BCM2837
    mbox.c       buzon de la VideoCore: le pregunta a la GPU cuanta RAM hay
    cache.S      invalidacion de la cache de datos antes de encender la MMU
    timer.c      temporizador generico de ARM: tick de 100 Hz
    uart.c       driver PL011: salida por polling, entrada por interrupcion
    kernel.c     kernel_main

## Los dos mundos

AArch64 traduce con dos tablas a la vez y elige segun los bits altos de la
direccion. Con 39 bits de VA quedan dos mitades de 512 GB con un abismo en
medio:

    0x0000000000000000 - 0x0000007FFFFFFFFF   TTBR0   el proceso
    0xFFFFFF8000000000 - 0xFFFFFFFFFFFFFFFF   TTBR1   el kernel

El kernel esta enlazado arriba y mapeado LINEAL: `VA = PA + KERNEL_VA_BASE`.
Pasar de una a otra es una suma (`phys_to_virt`, `virt_to_phys` en `mm.h`),
sin consultar ninguna tabla.

Eso obliga a encender la MMU en `boot.S`, antes de la primera instruccion de
C: el kernel esta enlazado en direcciones que sin traduccion no existen, asi
que ni siquiera podria leer una cadena de texto. `boot.S` trabaja en fisico
(restando `KERNEL_VA_BASE` a mano), construye las tablas, enciende la MMU
con TTBR0 y TTBR1 apuntando a la misma tabla — identidad abajo, lineal
arriba — salta a la direccion alta, y solo entonces quita la identidad.

A cambio:

  - cambiar de proceso toca **solo TTBR0**; el kernel no se mueve
  - un proceso ya no comparte **ni una entrada** de tabla con el kernel: si
    intenta leerlo, no es un fallo de permisos, es que ahi no hay nada
  - TTBR0 queda entero para el usuario, que ahora empieza en 4 MB

### ASIDs

Faltaba la otra mitad: aunque el kernel ya no se mueva, cada cambio de
proceso seguia tirando la TLB entera, entradas del kernel incluidas.

Un ASID es una etiqueta. Toda entrada de la TLB que venga de una pagina
marcada `nG` (las de usuario, ver `MM_USER_CODE` y `MM_USER_DATA`) se guarda
con el ASID del espacio que la creo, y la MMU solo la da por buena si
coincide con el activo. Las del kernel no llevan `nG`: son globales y valen
siempre.

El ASID activo no es un registro aparte, son los bits [63:48] de
**TTBR0_EL1** — los mismos que la direccion de la tabla. No es por ahorrar
registros: es para que cambiar de tabla y de etiqueta sea *una* escritura
de 64 bits. Si fueran dos, existiria un instante con la tabla nueva y la
etiqueta vieja, y lo que la MMU cachease ahi quedaria mal etiquetado.

Asi que `vmm_switch_to()` es ahora una escritura y un `isb`, sin ninguna
invalidacion. La unica que queda en la vida de un proceso es un
`tlbi aside1is` cuando muere y su etiqueta se recicla, en `vmm_destroy_pgd()`.

## El recolector

Un hilo que termina no puede limpiar lo suyo: esta corriendo **encima** de
ello. Su pila de kernel es la que tiene bajo los pies y su tabla de
traduccion es la que hay puesta en TTBR0 en ese instante. No se puede tirar
de la alfombra estando de pie sobre ella.

Por eso `task_exit()` solo se marca zombi y avisa a una cola de espera. El
entierro lo hace `reaper`, el primer hilo que crea el sistema (pid 1), que
tiene su propia pila y la tabla vacia en TTBR0:

    vmm_destroy_pgd(t->pgd, t->asid)   tablas, paginas y el ASID
    pmm_free(...)                      su pila
    t->state = TASK_UNUSED             y la ranura, la ultima

Que sea seguro descansa en un detalle del planificador: para que el
recolector llegue a ejecutarse, el zombi ha tenido que dejar la CPU, y
`pick_next()` no vuelve a elegirlo jamas. Desde ese momento su pila es papel
mojado.

Un detalle que casi muerde: `vmm_destroy_pgd()` recorre el espacio del
proceso devolviendo paginas al gestor de memoria, pero **no toda pagina
mapeada es RAM nuestra** — a un driver de EL0 le hemos mapeado los registros
de un periferico. Devolver eso al PMM seria repartir la UART como si fuera
memoria libre. Se distingue por el indice de MAIR del propio descriptor.

## El cargador

El kernel recibe un **ELF** y solo mira una parte minuscula: los program
headers de tipo `PT_LOAD`. Cada uno dice "coge estos bytes del fichero,
ponlos en esta direccion, rellena el resto con ceros, y dale estos
permisos". Todo lo demas —secciones, simbolos, reubicaciones— es para el
enlazador y el depurador, no para quien ejecuta.

    LOAD  off 0x1000  vaddr 0x400000  filesz 0x444  memsz 0x444  R E
    LOAD  off 0x2000  vaddr 0x401000  filesz 0x004  memsz 0x030  RW

Ahi esta todo: dos tramos con permisos distintos, y un `memsz` mayor que
`filesz` en el segundo. Esa diferencia es exactamente `.bss`, y el cargador
no tiene que hacer nada para rellenarla porque las paginas del PMM ya vienen
a cero.

**Esto sustituyo a una cabecera que nos habiamos inventado.** En el paso 12,
el kernel recibia un binario plano —una tira de bytes sin secciones, porque
`objcopy` se las habia comido— y le poniamos delante 48 bytes con magia
"TOSU" diciendo donde acababa el codigo. Funcionaba. Pero ese problema
estaba resuelto desde 1999, el enlazador emite ELF sin que se lo pidas, y
ademas **trae los permisos**, que en la version casera habia que deducir por
convenio. Menos codigo nuestro y mas garantias.

El corte entre tramos sigue alineado a 4 KB en `user/user.ld`, y no por
estetica: una pagina no puede ser medio ejecutable. Y hay que enlazar con
`-z max-page-size=4096`, o el enlazador de AArch64 alinea los segmentos a
64 KB y el fichero engorda quince veces.

## Los argumentos

Un proceso nuevo no tenia forma de saber que se esperaba de el: `cat`
llevaba el nombre del fichero escrito dentro.

El convenio es el de siempre —`x0` = argc, `x1` = argv, y argv apunta a un
array de punteros terminado en cero— pero lo interesante es *donde* vive
todo eso. El kernel lo escribe en la **pila del proceso**, que es el unico
sitio que ya es suyo cuando todavia no existe, y lo hace por el mapa lineal
del kernel porque esa pagina aun no esta en ningun TTBR0 activo.

    >> me han llamado con 3 argumento(s): [hello] [uno] [dos]

Con eso la cadena entera funciona de verdad:

    run HELLO.ELF  ->  lee el ELF del servidor de ficheros
                   ->  spawn(imagen, bytes, "HELLO.ELF")
                   ->  el kernel monta el ELF y le deja argv en la pila
                   ->  >> me han llamado con 1 argumento(s): [HELLO.ELF]

## Los cuatro nucleos

Desde el paso 1 habia tres cuartas partes de la maquina aparcadas en un
`wfe`. El paso 13a las enciende: cada nucleo baja a EL1, coge su propia pila
(16 KB por nucleo, ver `linker.ld`) y enciende su MMU con **las mismas
tablas** que el nucleo 0. Compartir TTBR1 es lo que hace que el kernel sea
uno solo visto desde los cuatro.

Despertarlos tiene una complicacion que QEMU y la placa no comparten:

  - **QEMU** suelta los cuatro nucleos en 0x80000. Los tres de mas bajan a
    EL1 y se quedan esperando en `secondary_wait`, nuestra propia
    spin-table.
  - **La Pi 3B** solo suelta el nucleo 0. A los otros tres los tiene el
    firmware dando vueltas en la suya, esperando que alguien les escriba una
    direccion en su buzon (0xE0, 0xE8 y 0xF0). Cuando salen, entran por
    `secondary_entry`.

Los dos caminos son reales y cada maquina usa el suyo: en la placa, las
letras de `BOOT_TRACE` de los nucleos 1-3 no salen al arrancar sino en el
momento exacto en que se escriben esos buzones.

`smp_start_secondaries()` dispara los dos caminos y no le importa cual
funcione. Con un detalle que ya conociamos del paso 9: el que espera lo hace
con las caches apagadas, asi que la senyal hay que bajarla a la RAM a mano
con `dcache_clean_range()` o no la veria nunca.

Un bit que NO se toca aqui, y conviene saber por que: `CPUECTLR_EL1.SMPEN`,
el que mete al nucleo en el dominio de coherencia. Es un registro
*implementation defined*, y que EL1 pueda leerlo depende de lo que hayan
dejado `ACTLR_EL2`/`ACTLR_EL3`, o sea del firmware. QEMU lo deja pasar; en
la Pi el acceso se atrapa, y una excepcion en ese punto — con `VBAR_EL1`
todavia sin poner — es una muerte silenciosa, ni un caracter por la UART.
El armstub del firmware ya lo pone en los cuatro nucleos.

Y por si hace falta depurar esta parte otra vez: `BOOT_TRACE` en `boot.S`
enciende una traza que escribe a pelo en la PL011, sin driver ni nada
inicializado, una letra por hito del arranque (`A` EL1, `B` .bss, `C`
tablas, `D` MMU, `E` arriba). Es lo unico que funciona antes de que exista
`uart.c`.

Lo que 13a **no** hace es dejarles tocar nada compartido. Rellenan su ficha
y se duermen; de imprimirla se encarga el nucleo 0.

El motivo se vio solo la primera vez que esto arranco en la placa: con
`BOOT_TRACE` encendida, los tres nucleos deberian haber escrito tres `A` al
despertar. Salieron dos. Entre el "¿hay hueco en la cola?" y el "escribe" de
la macro no hay nada que impida que otro nucleo haga lo mismo, y con cuatro
escritores sobre el mismo FIFO una letra se perdio. Cinco instrucciones
bastan para tener una carrera de datos; el resto del kernel esta lleno de
sitios peores. Eso es el paso 13b.

## Cerrojos

`irq_save()` no es un cerrojo, y hasta que hubo cuatro nucleos no habia
forma de notarlo: tapar las interrupciones calla al nucleo propio y a los
otros tres no les dice nada.

Un cerrojo de verdad necesita una operacion atomica, y en ARM no existe un
"test and set": existe un PAR de instrucciones con el hardware vigilando en
medio. `ldaxr` carga y reserva la direccion, `stlxr` escribe solo si la
reserva sigue viva y dice si lo consiguio. Las dos letras del medio son lo
que hace que el cerrojo proteja datos y no solo a si mismo: la `a` de
*acquire* impide que la seccion critica se lea antes de tenerla, la `l` de
*release* garantiza que lo escrito dentro ya es visible cuando otro ve el
cerrojo abierto.

`irq_save()` no desaparece: pasa a ser la mitad de un cerrojo. Uno que
tambien se coge desde un manejador de interrupciones hay que cogerlo con las
IRQ tapadas, o a este mismo nucleo le entra una IRQ teniendolo, el manejador
intenta cogerlo, y se queda esperando a alguien que no va a soltarlo nunca:
el mismo. De ahi `spin_lock_irqsave()`, y de ahi que el orden importe —
primero tapar, despues cerrar.

El comando `w` del menu lo enseña. Los cuatro nucleos suman 50000 veces
cada uno sobre el mismo contador, en una Pi 3B de verdad:

    Esperado: 200000
      sin cerrojo :  50036
      con cerrojo : 200000

Fijate en que 50036 es casi exactamente 50000, una sola tanda. Los cuatro
nucleos trabajaron y el resultado es el de uno: el trabajo de tres se
evaporo entero.

`contador++` son tres pasos —leer, sumar, escribir— y los cuatro nucleos se
pelean ademas por la misma linea de cache, asi que mientras uno da los tres
pasos los otros tres ya han leido el valor viejo. Sin cerrojo no es que
pierdas algunos incrementos: es que pierdes casi todo el paralelismo que
creias haber ganado.

(En QEMU salen unos 79000, porque solapa los nucleos menos que el silicio.
El hardware es mas duro con este error, no mas indulgente.)

Y `current` deja de ser una variable global, porque hacen falta cuatro. Su
sitio es **TPIDR_EL1**, un registro por nucleo que la arquitectura reserva
justo para esto. Se sigue escribiendo `current` en todo el kernel, pero
ahora cada nucleo lee el suyo. Las ranuras 0 a 3 de `tasks[]` quedan
reservadas para la tarea idle de cada nucleo: el contexto en el que ya
estaba cuando arranco.

## Un temporizador por nucleo

El temporizador generico de ARM no es un periferico compartido: `CNTP_TVAL_EL0`
y `CNTP_CTL_EL0` son registros de CPU, uno por nucleo. Y en el BCM2837,
`CORE0_TIMER_IRQCNTL` tampoco era un registro — era el primero de cuatro,
uno cada 4 bytes, igual que `CORE0_IRQ_SOURCE`. Mientras solo habia un
nucleo despierto la diferencia no se veia.

Ahora cada nucleo arma el suyo y `irq_handle()` pregunta por el registro del
nucleo en el que esta. Se ve en el comando `j`:

    nucleo  MPIDR_EL1           EL  SP (su pila)        IRQ atendidas
      0     0x0000000080000000  1   0xFFFFFF80000E80D0  641
      1     0x0000000080000001  1   0xFFFFFF80000E4100  640
      2     0x0000000080000002  1   0xFFFFFF80000E0100  640
      3     0x0000000080000003  1   0xFFFFFF80000DC100  640

Dos cosas siguen siendo de uno solo, a proposito:

  - **El reloj del sistema.** `timer_ticks()` lo sube solo el nucleo 0. Si
    lo subieran los cuatro, el tiempo correria al cuadruple y
    `task_sleep(100)` dormiria 250 ms en vez de un segundo.
  - **Las IRQ de perifericos.** `GPU_INT_ROUTING` las manda todas al nucleo
    0, asi que el bit `SRC_GPU` solo se enciende alli.

Lo que si es de cada uno es `need_resched` y la contabilidad de su hilo: que
al nucleo 2 se le acabe el turno no dice nada de lo que hace el 3.

Y `pick_next()` ya no acepta `TASK_RUNNING`. Antes daba igual, porque
RUNNING solo podia significar "la de esta CPU"; con cuatro nucleos significa
"corriendo en alguno", y elegirla seria ponerla a ejecutar en dos sitios a
la vez sobre la misma pila.

## El cerrojo que cierra uno y abre otro

Para que cuatro nucleos planifiquen hace falta un cerrojo sobre la tabla de
tareas. Y ahi aparece el problema que no se resuelve poniendo un cerrojo:
`wq_wait()` llama a `schedule()` desde DENTRO de su seccion critica, porque
"apuntarse en la cola" y "dormirse" tienen que ser indivisibles.

Un spinlock no se puede llevar a traves de un cambio de contexto. Soltarlo
antes dejaria la tabla a medias a la vista de los otros tres nucleos;
soltarlo despues es imposible, porque despues ya no somos nosotros.

La salida es que el cerrojo no se suelte: **se pasa de mano**. Lo cierra el
hilo que sale y lo abre el hilo que entra, cuando llegue a su propio
`sched_unlock_irqrestore()` — el de la llamada a `schedule()` en la que a el
lo desalojaron, hace quiza mucho rato. Durante todo el cambio nadie mas
puede mirar la tabla, que es exactamente lo que hace falta.

El unico que no tiene marco donde soltarlo es un hilo recien nacido, que
nunca ha pasado por `schedule()`. De ese se encargan `ret_from_fork`
(`switch.S`) y `ret_to_user` (`vectors.S`) llamando a
`sched_unlock_new_task()`. Es el mismo sitio donde `ret_from_fork` ya
destapaba las IRQ a mano desde el paso 5, y por la misma razon: ese camino
no es como los demas.

Con eso, el recolector sale gratis. No hace falta ninguna marca de "ya he
dejado la CPU": el zombi se marca y cambia de contexto sin soltar el
cerrojo, asi que el recolector no puede ni mirar la tabla hasta que el
cambio ha terminado. Cuando lo encuentra, la pila que va a liberar hace
rato que no la pisa nadie.

**Orden de cerrojos**, y hay que respetarlo: `sched_lock` se coge ANTES que
el de la UART y el del PMM, nunca despues.

## Como se entera un nucleo ocioso de que hay trabajo

Esta pregunta tuvo dos respuestas, y la primera la encontro el propio
comando `w` al empezar a dar el resultado exacto sin cerrojo — imposible si
hubiera carrera. Los cuatro hilos martillo corrian en el mismo nucleo:

    sin cerrojo : (nucleos: 3 3 3 3 ) 800000     <- los cuatro en el mismo
    sin cerrojo : (nucleos: 0 2 1 1 ) 403172     <- ya repartidos

Un dato compartido no se corrompe por compartirlo: se corrompe por
compartirlo AL MISMO TIEMPO. (De ahi tambien la barrera de salida de los
hilos martillo, sin la cual cada uno terminaba antes de que arrancara el
siguiente.)

**Primera respuesta: `wfe` en vez de `wfi`.** La tarea idle esperaba con
`wfi`, que solo despierta con una interrupcion, asi que un nucleo ocioso
tardaba hasta un tick — 10 ms — en enterarse. `wfe` despierta ademas con
`sev`, y `spin_unlock()` ya hacia `sev`: el mecanismo llevaba ahi desde el
paso 13b sin usar. Funcionaba, pero era un martillazo — despertaba a los
cuatro nucleos cada vez que alguien soltaba cualquier cerrojo.

**Segunda respuesta, la buena: un IPI.** El BCM2837 da cuatro buzones por
nucleo; escribir en el de otro le enciende una interrupcion. El buzon 0 es
"mirate el turno": no lleva contenido, porque lo que hay que mirar ya esta
en la tabla de tareas. `sched_kick_idle()` busca un nucleo ocioso y le da
un toque, y solo a el.

Saber quien esta ocioso no necesita leer el `TPIDR_EL1` de los demas: la
tarea idle del nucleo N solo la ejecuta el nucleo N, asi que verla en
`RUNNING` es verlo a el sin nada que hacer.

Se nota en el comando `j`: antes las cuatro cuentas de interrupciones iban
al unisono, porque solo las daba el temporizador. Ahora van desiguales, y
esa diferencia son los toques.

La busqueda del nucleo ocioso empieza cada vez por uno distinto. Mirando
siempre desde el 0 se veia en la placa que el nucleo 1 se llevaba casi
todos los avisos (el 0 suele estar ocupado), lo cual reparte el trabajo
pero no el coste de interrumpirse por los demas.

## Tres cosas que QEMU hace funcionar y el silicio no

Las tres se manifestaron igual, con un cuelgue mudo, y las tres estan
comentadas en el sitio donde importan:

  - **`CPUECTLR_EL1.SMPEN`** (`boot.S`). Es un registro *implementation
    defined*, y que EL1 pueda tocarlo depende de lo que hayan dejado
    `ACTLR_EL2`/`ACTLR_EL3`, o sea del firmware. En la Pi el acceso se
    atrapa; con `VBAR_EL1` aun sin poner, eso es la muerte.
  - **`TPIDR_EL1`** (`boot.S`). Al sacar `current` de `.bss` y meterlo en un
    registro se pierde el cero gratis que da el enlazador. El registro
    arranca con basura del firmware, y el `if (!current)` de
    `scheduler_tick()` pasaba de largo sobre un puntero inventado.
  - **`ldaxr`/`stlxr` sin cache** (`vmm.c`). Con `SCTLR_EL1.C` a 0 toda la
    memoria normal pasa a no cacheable, y en un Cortex-A53 las exclusivas
    necesitan la cache: el store-exclusive falla siempre y `spin_lock()`
    gira para siempre. Un spinlock necesita la cache encendida.

## El servidor de ficheros

El kernel **no sabe leer ficheros**, y eso es el punto entero.

`user/sd.c` habla con el controlador EMMC (0x3F300000) desde EL0, con la
pagina de registros concedida igual que la PL011 del servidor de consola.
`user/fs.c` monta FAT16 encima y sirve el puerto 1. Si cualquiera de los
dos se cuelga, se cuelga el: el kernel ni se entera.

Arrancar una tarjeta SD es una conversacion con un orden fijo, porque la
tarjeta es una maquina de estados:

    CMD0    "reiniciate"            -> estado idle
    CMD8    "¿aguantas 2.7-3.6 V?"  -> distingue las modernas
    ACMD41  "enciendete"            -> se repite hasta que dice que si
    CMD2    "dime quien eres"       -> su numero de serie
    CMD3    "toma una direccion"    -> la RCA
    CMD7    "te elijo a ti"         -> pasa a estado transfer

Y el reloj sube por etapas: la identificacion va a 400 kHz porque es lo
unico que toda tarjeta garantiza entender, y solo despues se sube a 25 MHz.

El reloj base NO se puede dar por supuesto, y esto costo un arranque en la
placa. Estaba escrito a mano a 41,666 MHz, copiado de un tutorial. La Pi
dice **200 MHz** y QEMU dice 50. Con el numero inventado, el divisor de los
25 MHz salia 0, se quedaba en 1, y la tarjeta acababa a 100 MHz: cuatro
veces por encima de lo que admite. Todo el protocolo estaba bien —CMD0 a
CMD3 pasaban limpiamente a 400 kHz— y fallaba en CMD7, la primera orden
despues de subir el reloj. Ahora se lo preguntamos a la GPU
(`SYS_clock_rate`), y el divisor redondea hacia ARRIBA: truncar da un
divisor menor, y un divisor menor es un reloj mas rapido del que se pide.

Un driver de EL0 no puede hablar con el buzon de la GPU —es uno solo para
toda la maquina y darlo entero seria dar el control de la placa—, asi que
el kernel contesta esa pregunta concreta y ninguna mas, con una lista
blanca de tres relojes. Mismo criterio que con el pin mux: el kernel es el
dueño de lo que es de todos, y responde preguntas estrechas.

**El protocolo no tiene open/close**, y es deliberado: cada peticion lleva
el nombre y el desplazamiento. Un servidor sin estado no tiene descriptores
que perder cuando un cliente muere sin avisar, ni tabla que limpiar, ni
limite de ficheros abiertos. Se paga con una busqueda por peticion, que el
servidor se cachea.

### Y quien carga los programas

Hasta aqui todos los programas de usuario venian empotrados en la imagen
del kernel por `tools/bin2c.py`. El comando `e` los saca de la tarjeta, y
fijate en quien hace que:

    [run] leyendo HELLO.BIN de la tarjeta...
    [run] 4100 bytes leidos, se los paso al kernel
    [run] arrancado como pid 16
    >> Hola desde EL0. Soy un proceso de usuario.

`user/run.c` lee los bytes del servidor de ficheros y luego llama a
`spawn()`, que es lo unico que pone el kernel: convertir bytes en proceso.
Si fuera al reves —el kernel leyendo de un servidor de usuario— el kernel
dependeria de un proceso que puede morirse, y eso es justo lo que un
microkernel no hace. `exec` es cosa del usuario.

## El interprete de ordenes

`user/sh.c` es el primer programa que ata todo lo demas, y no tiene ningun
privilegio especial. No sabe hacer nada por si mismo:

    lee una linea           con SYS_read
    busca el programa       preguntandole al servidor de ficheros
    lo arranca              con spawn(), pasandole la linea de argumentos
    espera a que termine    con waitpid()

```
$ ls

  Contenido de la tarjeta:
    HOLA.TXT      82 bytes
    HELLO.ELF     8616 bytes
    LS.ELF        5264 bytes
    CAT.ELF       5296 bytes
    RUN.ELF       5920 bytes

$ cat HOLA.TXT

  --- HOLA.TXT ---
Hola desde la tarjeta SD.
Este fichero lo ha puesto un Mac y lo va a leer TinyOS.
  --- fin ---

$ pepe
  no encuentro PEPE.ELF
```

**Todas las ordenes son programas de la tarjeta**, sin ninguna dentro del
shell. Eso no es purismo: significa que anyadir una orden es copiar un
fichero a la SD, sin tocar ni recompilar nada.

Hicieron falta dos cosas que no existian. `SYS_read`, porque la entrada de
consola seguia siendo del kernel. Y `SYS_waitpid`, porque sin el el prompt
volvia antes de que el programa hubiera abierto la boca.

### Un teclado, dos lectores

La consola tiene UN teclado, y en cuanto arranca el shell hay dos lectores:
el hilo del menu del kernel y el proceso. Cada caracter se lo llevaria el
que despertara antes, que es tanto como repartir lo que escribes a cara o
cruz.

Se resuelve cediendo la entrada entera: mientras el shell viva, el hilo del
kernel no lee. Lo comprueba con `task_alive()` y se duerme. `salir` le
devuelve la consola, y el menu de una tecla sigue ahi para lo que es: un
depurador del kernel.

Dos detalles de comodidad. Un proceso arrancado con `spawn()` toma su
nombre de su propio `argv[0]`, copiado a la tarea porque el puntero del que
llama vive en un espacio de direcciones que puede desaparecer antes: por eso
`l` dice `hello` y no `spawn`. Y `SYS_exit` solo anuncia las salidas con
error — que un programa termine bien es lo normal, y decirlo en voz alta
llena de ruido una sesion de shell. Ahi el silencio es la respuesta.

## Escribir en FAT

Leer un sistema de ficheros es seguir punteros. Escribirlo es **tocar tres
sitios en el orden correcto**: los datos, la tabla FAT que dice que cluster
va detras de cual, y la entrada de directorio que dice el tamanyo. Si solo
se toca uno, el fichero queda a medias.

    $ write NOTA.TXT hola mundo desde tinyos
      escrito en NOTA.TXT
    $ cp hello.elf copia.elf
      copiados 8616 bytes en copia.elf
    $ copia
      >> Hola desde EL0. Soy un proceso de usuario.

Ese `cp` es la prueba de fuego: 8616 bytes son casi noventa idas y venidas
por el IPC y varios clusters encadenados. Y el resultado se **ejecuta**, que
es una comprobacion mas dura que cualquier `cmp`: un solo byte mal y el ELF
no carga.

Visto desde fuera, con la imagen montada en el Mac:

    cmp hello.elf /Volumes/TINYOS/COPIA.ELF   -> identicos
    diskutil verifyVolume                     -> exit code 0

Que lo valide una implementacion que no es la nuestra es lo mas parecido a
una prueba que hay aqui.

### Tres detalles que no perdonan

**La FAT se escribe por duplicado.** Hay dos copias (a veces mas) porque es
lo unico irremplazable del volumen: perder los datos de un fichero es perder
un fichero, perder la FAT es perderlos todos. Escribir solo en la primera
"funciona" hasta que alguien repara el disco con la segunda.

**El tamanyo se apunta el ultimo.** Hasta que no esta en la entrada de
directorio, los bytes escritos no existen para nadie.

**Un tope duro al numero de clusters.** El BPB dice cuantos hay, pero la
tabla FAT tiene 256 casillas por sector y no pueden ser mas de las que
caben. Sin esa comprobacion, un BPB raro haria que el buscador de sitio
libre escribiera *pasada* la tabla, encima del directorio raiz. Es el tipo
de fallo que no avisa: la tarjeta sigue pareciendo correcta hasta que se
pierde entera.

Borrar, por cierto, es poner un `0xE5` en la primera letra del nombre y
devolver los clusters. Los datos siguen ahi intactos — y por eso se pueden
recuperar ficheros borrados mientras nadie escriba encima.

## El monton del kernel

Hasta aqui el kernel solo sabia repartir paginas de 4 KB. Todo lo que
necesitaba otro tamanyo se declaraba como un array estatico, y con eso se
fijaba un techo para siempre: `MAX_TASKS`, `MAX_PORTS`, `MAX_ARGS`.

El diseño es el de toda la vida: una lista de los trozos libres, una
cabecera pequenya delante de cada uno, y dos operaciones inversas. `kmalloc`
busca el primer hueco donde quepa y lo parte si sobra mucho; `kfree` lo
devuelve y lo **funde** con sus vecinos.

Fundir es la mitad que se olvida, y es la que decide si el monton dura. Sin
fundir, cada pareja de reserva y liberacion deja la lista un poco mas
picada: al cabo de un rato hay memoria libre de sobra pero ningun hueco lo
bastante grande. Eso es la fragmentacion, y es una forma de quedarse sin
memoria teniendola.

Por eso la lista esta ordenada **por direccion** y no por tamanyo: asi los
vecinos en memoria son vecinos en la lista, y fundirlos es mirar si el de al
lado empieza justo donde acaba este.

El comando `g` lo castiga a proposito — reparte 96 bloques de tamanyos
dispares, suelta uno de cada dos (huecos alternos, el peor caso) y vuelve a
pedir:

    al empezar   total 0       usado 0       huecos 0
    96 bloques   total 196608  usado 139008  huecos 1
    mitad fuera  total 196608  usado 68400   huecos 49
    rellenado    total 196608  usado 110608  huecos 43
    todo fuera   total 196608  usado 0       huecos 1   mayor 196608

Los **49 huecos vuelven a ser uno solo** de 192 KB. Eso, y que `usado`
regrese a cero exacto, es lo unico que hay que comprobar de un asignador:
que devuelva la memoria entera y que la funda. Cada bloque lleva ademas un
patron que depende de su indice, asi que si un `kmalloc` entregara memoria
que ya era de otro, se veria.

Su primer cliente de verdad son las colas de los puertos IPC, que con
mensajes de 128 bytes eran 10 KB de `.bss` reservados siempre. Ahora se
piden al crear el puerto y se devuelven al morir su duenyo: tras arrancar
el sistema entero, el monton dice `usado 2464`, que son exactamente las dos
colas vivas con sus cabeceras.

**Orden de cerrojos**, que ahora son tres: `sched_lock` antes que
`heap_lock`, y `heap_lock` antes que el del PMM. La cola de un puerto se
pide ANTES de coger el del planificador, porque `kmalloc` puede tener que ir
a por paginas y borrar 64 KB, y eso con los otros tres nucleos parados no.

## Memoria para los procesos

Un proceso tenia lo que declarase en tiempo de compilacion y nada mas: sus
segmentos del ELF y una pagina de pila. Por eso `sh` llevaba un buffer
estatico de 32 KB para cargar programas, que era a la vez desperdicio y
techo.

`sbrk(delta)` mueve el **tope** del monton: la primera direccion que el
proceso todavia no tiene. Hacia arriba es pedir, hacia abajo devolver, y lo
que se devuelve es el tope VIEJO, que es justo el principio de lo que se
acaba de conseguir. Es la interfaz mas tonta que existe para pedir memoria,
y por eso la llevan los Unix desde 1971: el kernel no sabe de bloques ni de
listas, solo de "hasta aqui".

Repartir ese espacio en trozos es trabajo del proceso, y `user/umalloc.c`
lo hace con **el mismo algoritmo** que `src/kheap.c`. Merece la pena verlos
juntos, porque lo unico que cambia entre un asignador de kernel y uno de
usuario es de donde sale la memoria cuando se acaba:

    el del kernel   se la pide al gestor de paginas   pmm_alloc_contig()
    el del proceso  se la pide al kernel              sbrk()

El resto —partir al reservar, fundir al liberar, la fragmentacion que
aparece si no fundes— es identico, porque el problema es el mismo.

La orden `mem` lo ensenya:

    tope del monton (sbrk)   que acaba de pasar
    4202496   al empezar: el monton esta vacio
    4284416   despues de 64 bloques de 1 KB
    4284416   despues de soltarlos: no baja, y es lo correcto
    4284416   despues de pedir 48 KB de golpe

Las dos ultimas lineas son las interesantes. `free()` **no baja el tope**:
suelta el trozo en la lista del proceso para que el siguiente `malloc` lo
reaproveche, y esta bien que sea asi — devolverlo al kernel para volver a
pedirlo dos lineas despues serian dos llamadas al sistema tiradas. Y pedir
48 KB de golpe **tampoco lo sube**, porque los 64 huecos de 1 KB se habian
fundido en uno solo.

### El mapa del proceso, ampliado

La pila estaba en 8 MB y el codigo en 4, asi que un monton que creciera
chocaba con ella en seguida. Ahora:

    0x00400000  codigo y datos (lo que diga el ELF)
                |  el monton crece hacia arriba
    0x0F000000  tope del monton
    0x10000000  MMIO concedido, si es un driver
                ^  la pila crece hacia abajo
    0x20000000  tope de la pila

Los 256 MB de abismo entre el monton y la pila son a proposito: que crezcan
el uno contra el otro y se toquen es un error clasico.

## El fallo de pagina deja de ser un error

Hasta aqui, TODO fallo de traduccion en un proceso significaba lo mismo:
*ha tocado donde no debia, se muere*. La pila era una pagina fija de 4 KB y
cualquier recursion decente la desbordaba en silencio, escribiendo encima
de lo que hubiera debajo.

Esto cambia esa lectura. Un fallo justo debajo de la pila no es un error,
es una **peticion**: el proceso necesita mas sitio y la forma de pedirlo es
usarlo. El kernel le da una pagina y **reintenta la instruccion**; el
proceso no se entera de nada.

Ese cambio de interpretacion es el corazon de la memoria virtual moderna.
De aqui salen `mmap`, el copy-on-write y el `fork`: en los tres, el fallo
de pagina deja de ser un accidente y pasa a ser el mecanismo.

    $ deep 300
      pila al empezar : 536870864
      [kernel] la pila de deep crece a 2 paginas
      ...
      se ha comido    : 155 KB
      y he vuelto entero

La comprobacion clave es la del **puntero de pila**. Se da una pagina si la
direccion tocada esta por encima del SP del proceso, que es como se ve una
pila creciendo de verdad: el compilador baja SP primero y escribe despues.
Un puntero perdido que apunte mucho mas abajo sigue siendo mortal, y eso es
lo que tiene que ser:

    $ deep 5000
      Detalle : Fallo de traduccion (pagina no mapeada) (escritura)
      Direccion (FAR_EL1) : 0x000000001FEFFFA0
      [kernel] el proceso deep ha violado la ley. Lo mato y sigo.

Esa direccion cae justo por debajo de `USER_STACK_MIN`. El limite es de
1 MB, se respeta, y pasarse sigue siendo fatal — solo que ahora el limite
es de un megabyte y no de cuatro kilobytes.

Tampoco entra aqui un fallo de **permisos**: escribir en el propio codigo
sigue siendo una violacion y no una peticion. Lo distingue el ISS de
`ESR_EL1`, donde los fallos de traduccion son `0b0001xx`.

### El compilador casi arruina la prueba

La primera version de `deep.c` no gastaba pila: escribia su buffer local,
llamaba, y no volvia a mirarlo. Al compilador le basto con eso para ver que
el buffer estaba muerto durante la llamada, reaprovechar el mismo sitio en
todos los niveles y **convertir la recursion en un bucle**. Cuarenta
niveles, medio kilobyte de pila en total, y una prueba que decia que todo
iba bien sin haber probado nada.

Para que un nivel conserve su marco, el marco tiene que seguir haciendo
falta cuando vuelve la llamada. Es un recordatorio util: una prueba que no
falla cuando deberia no esta midiendo lo que crees.

## La pagina de guarda

La pila de usuario ya crece sola. La de KERNEL no podia: es una pagina
fija, y desbordarla no daba ningun fallo — daba una escritura silenciosa
encima de la tarea de al lado, que reventaba mucho despues y en otro sitio.
El `STACK_MAGIC` del paso 5 lo detectaba, pero *a posteriori* y solo si a
alguien se le ocurria mirar.

La solucion es dejar un hueco debajo. Y ahi aparece un problema geometrico
que no se ve venir: **las pilas de kernel vivian en el mapa lineal, que
esta hecho de bloques de 2 MB**, y dentro de un bloque no se puede dejar un
hueco de 4 KB.

Asi que se mudan a su propia zona del espacio del kernel, mapeada pagina a
pagina, con dos paginas de espacio virtual por tarea:

    KSTACK_AREA + ranura*8K        la pagina de guarda, SIN MAPEAR
    KSTACK_AREA + ranura*8K + 4K   la pila de verdad

La ranura de la tarea decide donde cae la suya, asi que no hay nada que
apuntar: la tarea 7 siempre tiene la suya en el mismo sitio.

El resultado, con el comando `5`:

    ############  PILA DE KERNEL DESBORDADA  ############
    La tarea de la ranura 8 (shell) se ha salido de su pila
    y ha tocado su pagina de guarda, en 0xFFFFFF8100010000.

Con el nombre de la tarea, la direccion exacta, y el `ELR_EL1` apuntando a
la instruccion que se paso. La diferencia con antes no es que ahora falle:
es que antes **no** fallaba, y el sistema seguia andando con una tarea
corrompida.

El `STACK_MAGIC` sigue ahi, pero ha cambiado de papel: era el unico aviso y
ahora es la segunda red, para el caso de que alguien salte por encima de la
guarda de un brinco largo.

## fork, y la mentira util

Un proceso se duplica. El hijo sale con **el mismo estado** que el padre
-los mismos registros, la misma pila, la misma posicion en el codigo- y la
unica diferencia esta en `x0`: el padre recibe el pid del hijo y el hijo
recibe un cero. De ahi sale el `if (fork() == 0)` de toda la vida.

Y no se copia memoria. Los dos espacios apuntan a **las mismas paginas**,
marcadas de solo lectura en ambos; la primera escritura de cualquiera de
los dos es la que paga su copia.

Lo que mas cuesta ver es que al padre tambien se le quita el permiso. Si lo
conservara, escribiria en paginas que ya no son solo suyas y el hijo veria
cambios que no le corresponden. **El sistema les esta mintiendo a los dos**:
les dice que no pueden escribir cuando en realidad si pueden. La mentira se
deshace, pagina a pagina y solo cuando hace falta, en el manejador de
fallos.

Ahi hay dos casos, y la diferencia entre ellos es todo el ahorro:

    la usa uno solo   ->  no hay nada que copiar, se devuelve el permiso
    la usan varios    ->  ahora si, se hace la copia privada

El primero es el que hace que un `fork` seguido de un `exec` no copie casi
nada, y el que hace que si el padre muere, el hijo se quede con las paginas
originales sin pagar una sola copia.

### Lo que cuesta de verdad

    $ forkd
      paginas libres al empezar : 245391
      tras reservar y tocar 1 MB: 245134
      [padre] el hijo es el pid 17
      [padre] el fork ha costado 5 paginas
              (si copiara el mega, serian 256 y pico)
      [hijo]  la cambio a 222 y me voy
      [padre] mi global sigue valiendo 111   <- aislado
      [padre] paginas libres al final : 245134

**Cinco paginas**: las tablas de traduccion del hijo y su pila de kernel.
Bifurcar un proceso de 8 MB cuesta lo mismo que bifurcar uno de 8 KB,
porque lo unico que se recorre es la tabla. Y al final la memoria vuelve
entera: las copias que el hijo llego a pagar se liberaron al morir.

### Las piezas que ya estaban

Casi todo lo que hace falta para esto se construyo antes sin saber que era
para esto:

  - **ASIDs** (paso 10b), para que dos espacios convivan en la TLB
  - **El manejador que reintenta** (paso 21), que ya sabia que un fallo
    puede ser una peticion y no un error
  - **Distinguir traduccion de permisos** en el `ISS`, que en el paso 21
    servia para no confundir una pila que crece con una violacion, y aqui
    sirve para reconocer una pagina que toca copiar

Lo unico nuevo es un **contador de referencias por pagina** en el PMM.
`pmm_free()` deja de significar "devuelvela" y pasa a significar "yo ya no
la uso": solo vuelve al bitmap cuando no la usa nadie.

Y un bit del descriptor. Los bits 55 a 58 los ignora el hardware y estan
ahi para que el sistema operativo apunte lo que quiera; el 55 marca las
paginas COW. Sin esa marca no habria forma de distinguir "de solo lectura
porque es codigo" de "de solo lectura porque todavia no te he dado tu
copia", y la primera es una violacion mientras que la segunda es un
tramite.

## exec, y la falta de marcha atras

`fork` duplica; `exec` sustituye. Juntos son la forma clasica de arrancar
un programa, y cada uno hace **una** cosa sola — que es justo lo que
permite combinarlos: entre el `fork` y el `exec` cabe todo lo que un shell
quiera preparar para el hijo sin afectarse a si mismo.

Aqui hay una regla que no aparece en ninguna otra parte del kernel: **no
hay vuelta atras**. En cuanto se tira el espacio de direcciones viejo, un
fallo deja al proceso sin memoria, sin codigo y sin sitio al que volver.
Por eso todo lo que puede fallar se hace ANTES, y el cambio de sitio ocurre
cuando ya no queda nada que pueda ir mal.

Y el orden importa por un segundo motivo, menos evidente: **la imagen del
programa nuevo vive en la memoria del proceso viejo**. Si se tirara
primero, no quedaria nada que leer. Se construye el espacio nuevo leyendo
del viejo, que sigue siendo el activo, y se cambia al final — con un
`vmm_switch_to` explicito, porque TTBR0 todavia apunta al que esta a punto
de desaparecer.

Un detalle que encaja solo: `task_exec()` devuelve **argc**. El despachador
de llamadas hace `f->x[0] = ret` al terminar, y `x0` es exactamente donde
el programa nuevo espera encontrar su argc. No hace falta ningun caso
especial.

El MMIO concedido **no** se hereda: se le dio al programa que habia, y ese
programa ya no existe. Un driver que hace `exec` deja de ser un driver.

### Dos formas de arrancar algo

El proyecto tiene las dos, y la diferencia se ve mejor juntas:

    spawn(imagen, bytes, args)      un paso: el kernel crea el proceso
    fork() + exec(imagen, ...)      dos pasos: duplicarse y transformarse

`spawn` es mas directo y es lo que usa `run`. Pero con `fork` y `exec` por
separado, el hijo existe durante un rato **siendo todavia el padre**, y en
ese rato se le puede preparar el terreno: cambiarle lo que va a heredar,
cerrarle cosas, dejarle algo puesto. Con `spawn` ese hueco no existe. Es
la razon por la que Unix eligio el par en vez de la llamada unica, y `sh`
ya lo usa:

    int64_t pid = fork();
    if (pid == 0) {
        exec(imagen, bytes, linea);
        exit(1);                     /* si exec vuelve, es que fallo */
    }
    waitpid(pid);

El hijo lee la imagen de la memoria del shell, y puede hacerlo porque el
copy-on-write ya se la ha dado: en ese momento esos bytes son suyos.

## Senyales

Una senyal es **un bit**. Todo lo demas -cuando se mira, que se hace con
el, como se le cuenta al proceso- es politica que decide el kernel.

Y el momento en que se mira no es casual: **justo antes de volver a EL0**,
y en ningun otro sitio. Antes no se puede, porque el proceso esta a medias
de una llamada al sistema y su estado no es coherente; despues no hay
ocasion, porque ya se ha ido. Como todo lo que hace el kernel pasa por
`exception_dispatch` -llamadas, interrupciones, fallos de pagina- basta
mirar en un sitio.

### El kernel fabrica una llamada a funcion

Si hay manejador, el kernel guarda el contexto interrumpido **en la pila
del proceso** y reescribe el marco de excepcion:

    f->elr    = el manejador      el proceso "aparece" ahi
    f->x[0]   = el numero         con la senyal como argumento
    f->lr     = el trampolin      y vuelve por aqui
    f->sp_el0 = debajo del contexto guardado

Al hacer el `eret`, el proceso se encuentra dentro de una funcion que
nunca llamo. Cuando esa funcion retorna, cae en el trampolin, que llama a
`sigreturn`, que restaura el contexto y lo devuelve **exactamente** donde
estaba. Se ve en `trap`:

    [trap] sigo aqui, vuelta 6
    [trap] atrapada la senyal 2, van 1
    [trap] sigo aqui, vuelta 7

La vuelta 7 viene despues de la 6 aunque en medio se haya ejecutado codigo
que el programa no pidio. El proceso no puede notar la diferencia, y esa
es toda la idea.

### Por que hay un trampolin

La direccion de retorno tiene que apuntar a codigo que exista en el
programa. El kernel no puede inventarse tres instrucciones en la pila
porque la pila **no es ejecutable** — y mejor que siga sin serlo. Asi que
`user/signal.c` lleva una funcion minuscula que no hace mas que llamar a
`sigreturn`, y el programa se la pasa al kernel sin llegar a verla nunca.

### Ctrl-C es del driver, no del programa

El caracter 3 no se pone en la cola de entrada: se lo queda `uart_irq` y
lo convierte en una senyal. Por eso Ctrl-C funciona aunque el programa no
este leyendo del teclado, que es justo cuando hace falta.

Va al proceso de **primer plano**, definido de la forma mas simple que
funciona: el duenyo de la consola, o el hijo al que este esperando. Un Unix
de verdad lleva grupos de procesos; esto es la misma idea sin la
contabilidad.

### Lo que se hereda y lo que no

`fork` hereda los manejadores -el hijo es el mismo programa y sabe atrapar
lo mismo- pero **no** las senyales pendientes, que eran del padre. `exec`
los borra todos: apuntaban a codigo de un programa que ya no existe.

Y `SIGKILL` no se puede atrapar. Eso no es una limitacion, es su unico
motivo de existir: si un proceso pudiera ignorarla, no habria forma de
acabar con uno que se ha vuelto loco.

## Sueño interrumpible

Las senyales del paso anterior solo llegaban a quien estuviera corriendo.
Un proceso bloqueado esperando algo que no llega no se enteraba de nada, y
eso es justo cuando mas falta hace un Ctrl-C.

`wq_wait()` pasa a devolver un valor: **0 si lo desperto el aviso que
esperaba, -1 si lo desperto una senyal**. Y quien lo llama tiene que
mirarlo, porque volver con -1 significa que la condicion NO se cumplio y
la llamada al sistema debe abandonar.

Los cerrojos internos del kernel usan la version que no se deja
interrumpir: son cortos, no dependen de nadie de fuera, y dejarlos a
medias seria peor que esperar.

Para sacar a alguien de una cola hizo falta que la tarea supiera **en cual
esta durmiendo** (`t->wq`), porque una senyal tiene que desenlazarla a
mano. El efecto se ve asi:

    $ upper          <- se queda esperando una tecla
    ^C
      [kernel] upper termina por la senyal 2
      [salida -1]

Antes de este paso, ese Ctrl-C no habria hecho nada.

## Un zombi con duenyo

`waitpid` perdia el codigo de salida del hijo, porque el recolector se
llevaba los zombis en cuanto aparecian. Ahora un zombi **con padre vivo no
se toca**: existe precisamente para que ese padre pueda leer lo que
devolvio. Cuando el padre lo recoge -o se muere sin hacerlo- deja de tener
padre, y entonces es del recolector.

Eso es lo que significa de verdad un proceso zombi, y explica por que en
un Unix se acumulan cuando un padre no espera a sus hijos.

## Descriptores de fichero

Hasta aqui, un programa que escribia llamaba a `SYS_write` y el kernel lo
mandaba a la UART. Directo, y por eso mismo **imposible de redirigir**: no
habia ningun sitio donde decir "lo que escriba este, que vaya a otro lado".

Un descriptor es ese sitio. El programa escribe "en el 1" y **quien decide
que es el 1 es quien lo arranco**. De ahi sale todo lo demas.

    SYS_write(buf, n)        ->  SYS_write(fd, buf, n)
    SYS_read()  -> un char   ->  SYS_read(fd, buf, n)

Los descriptores **se heredan** al bifurcarse y **sobreviven al exec** —
esa es la pieza que hace util al par: el shell prepara la tuberia, se
bifurca, y el hijo ya la tiene puesta sin haber hecho nada, y sigue
puesta cuando se convierte en otro programa.

## Tuberias

Una cola de bytes con dos extremos y una regla: quien lee se para si no
hay nada, quien escribe se para si no cabe. Esa espera es todo el
mecanismo de sincronizacion que hace falta para encadenar programas.

    $ cat hola.txt | upper
      HOLA DESDE LA TARJETA SD.
      ESTE FICHERO LO HA PUESTO UN MAC Y LO VA A LEER TINYOS.

    $ ls | wc
      17 lineas, 461 bytes

Ni `cat` ni `ls` saben que hay alguien detras, y ni `upper` ni `wc` saben
de donde les llega. Esa ignorancia es lo que los hace combinables.

Lo que la cierra es el **final de fichero**: cuando se cierra el ultimo
extremo de escritura, quien lee deja de esperar y recibe un cero. Por eso
el shell cierra sus dos copias de la tuberia despues de bifurcarse —
mientras quede un solo descriptor de escritura abierto en cualquier
proceso, el lector espera para siempre.

### Dos interbloqueos que aparecieron por el camino

**Tocar memoria de usuario con el cerrojo cogido.** Las colas de la
tuberia se manejan con `sched_lock`, y leer del buffer del proceso ahi
dentro puede provocar un fallo de pagina -la pila crece, una pagina es
COW- que acabaria pidiendo ese mismo cerrojo. La solucion es copiar a un
buffer del kernel ANTES de entrar.

**Heredar los descriptores con el cerrojo cogido.** `task_fork` copiaba la
tabla dentro de la seccion critica, y `file_dup` lo pide por su cuenta.
Un spinlock contra uno mismo, y el sintoma fue un cuelgue seco en el
primer `|`.

## El teclado, tambien en EL0

La pantalla salio del kernel en el paso 8. El teclado tardo diecinueve
pasos mas, y no por falta de ganas: **un proceso de EL0 no puede recibir
interrupciones**. Las interrupciones entran por la VBAR, que es un registro
de EL1, y saltan a una pila de EL1 con privilegios de EL1. No hay forma de
que ese vector aterrice en espacio de usuario, y no la hay por diseño: si
la hubiera, cualquier proceso podria quedarse con el temporizador.

Asi que el driver de teclado parece imposible por definicion. La salida no
es dar la interrupcion, sino el **aviso**:

```
    llega la IRQ 57
      -> el kernel la enmascara y manda CMSG_IRQ al puerto del driver
      -> el driver despierta, vacia la FIFO, hace irq_ack()
      -> el kernel la vuelve a abrir
```

El kernel se queda solo con lo que de verdad exige privilegio: atender el
vector y tocar el controlador de interrupciones. Son nueve lineas. Todo lo
demas -saber que registro leer, cuantos bytes hay, que hacer con ellos- vive
en `user/conserver.c`, en EL0, sin privilegios.

**Por que hay que enmascararla.** Si se dejara abierta, volveria a saltar
inmediatamente: el periferico sigue pidiendo atencion y seguira pidiendola
hasta que alguien lea su FIFO, y quien va a leerla es un proceso que
todavia no ha tenido ocasion de ejecutarse. El sistema se quedaria dando
vueltas en el manejador, atendiendo una y otra vez la misma interrupcion,
sin llegar nunca a planificar al unico que podia callarla. Enmascarar es lo
que rompe ese circulo: la fuente se calla hasta que el driver dice que ya.

**Lo bonito es lo que ve el driver.** Para `conserver.c`, una interrupcion
no se distingue de cualquier otro mensaje: entra por el mismo `msg_recv` y
se atiende en el mismo bucle que un `CMSG_PRINT`. No hay contexto de
interrupcion, ni reentrada, ni carreras entre el manejador y el codigo
normal, ni la regla de "aqui no puedes bloquear". Esa uniformidad no es un
efecto secundario: es exactamente lo que se compra con un microkernel.

**Lo que se queda dentro, y por que.** Dos cosas.

El *buffer* de teclas sigue en el kernel. El driver lo llena con
`SYS_console_push` y `read(0, ...)` lo vacia como siempre. Se podria haber
hecho que cada `read` fuera un viaje de ida y vuelta al servidor, y seria
mas puro, pero entonces el kernel necesitaria un puerto propio para recibir
la respuesta y habria que inventar como se bloquea a un proceso esperando
una contestacion que llega a otro. Lo que se gana es coherencia; lo que se
paga son dos mecanismos nuevos. Aqui se eligio pagar menos: el driver esta
fuera, que era el objetivo, y la cola se queda donde ya estaba.

El *a quien interrumpe Ctrl-C* tambien. El driver ve el byte 3 y decide que
significa "interrumpe" -eso es politica de terminal, y el terminal es el-
pero no puede saber a quien: eso esta en la tabla de procesos. De ahi el
reparto: `console_int()` avisa, y el kernel elige la victima. Es la misma
frontera de siempre, dicha de otra forma: **el driver sabe de hardware, el
kernel sabe de procesos.**

**Un driver que se puede morir.** Decir "si tiene un bug muere el y el
sistema sigue" solo vale si es verdad. Matando el conserver con SIGKILL, la
interrupcion quedaria enmascarada esperando un `irq_ack` que no va a llegar
nunca, y el teclado dejaria de existir hasta el siguiente reinicio. Por eso
`ipc_release_ports()` llama a `irq_release_port()`: cuando muere el duenyo
de un puerto, el kernel recupera la fuente y la reabre. Se puede comprobar:

```
    f  s  z            arrancar ficheros, consola y shell
    kill 15 9          matar al driver de teclado
    ls                 sigue respondiendo: la lleva el kernel otra vez
```

Con esto el kernel ya no toca ningun periferico de entrada/salida salvo
para imprimir sus propios mensajes de arranque. La UART, la SD y el teclado
estan los tres fuera.

## Redireccion: < y >

`ls > lista.txt` parece cosa del shell, y casi todo lo es: partir la linea,
quedarse con el nombre, y no pasarle el ">" al programa, que no tiene por
que enterarse. Pero hay una pieza que el shell no puede poner. El programa
va a escribir "en el 1", y alguien tiene que hacer que el 1 sea un fichero.

Eso es un descriptor nuevo, `F_FICHERO`, y tiene una peculiaridad: **el
fichero no esta en el kernel**. Esta en el servidor de EL0, detras de un
puerto de mensajes. Un descriptor que apunta a algo que vive en otro
proceso.

**El kernel, de cliente.** Hasta aqui el kernel solo recibia peticiones. Un
proceso llamaba, el kernel contestaba, y la direccion era siempre la misma.
Para esto tiene que hacer lo contrario: pedirle algo a un proceso de EL0 y
esperar la respuesta.

No es tan raro como suena. El hilo que hace `read()` ya esta dentro del
kernel, con su pila y su entrada en la tabla de tareas; puede mandar un
mensaje y dormirse esperando la contestacion igual que se duerme esperando
una tecla. Lo unico que le faltaba era un puerto donde recibirla:

```c
    port_send(PORT_FILES,  &peticion);
    port_recv(PORT_KERNEL, &respuesta, 0);   /* duenyo: el pid 0 */
```

`PORT_KERNEL` es de duenyo 0, y el 0 es un pid que no existe -los procesos
empiezan en `CORES`-, asi que solo el kernel puede vaciarlo. Y la direccion
de la dependencia sigue siendo la buena: el kernel no *llama* al servidor,
le *escribe*. Si el servidor no esta, el mensaje no llega a ninguna parte y
`read()` devuelve error; no hay nada que se cuelgue esperando codigo que no
existe.

**Lo barato que sale un protocolo sin estado.** El servidor de ficheros no
tiene `open` ni `close`: cada peticion lleva el nombre y el desplazamiento.
Eso, que se decidio en el paso 15 para no tener descriptores que perder
cuando un cliente muere sin avisar, resulta que hace que un fichero abierto
sea exactamente esto:

```c
    char     nombre[16];
    uint64_t off;
```

No hay handshake, no hay handle que pedir, no hay nada que cerrar al otro
lado. `file_close` sobre un `F_FICHERO` es un `kfree` y ya. Una decision
tomada por un motivo (robustez) pagando un coste (buscar en el directorio
en cada peticion) resulta regalar otra cosa trece pasos despues. Pasa mas
de lo que parece.

**Donde va cada cosa en el shell.** La redireccion se aplica **en el hijo**,
entre el `fork` y el `exec`:

```c
    int64_t pid = fork();
    if (pid == 0) {
        aplicar(hay, ent, sal);        /* abre y dup2 sobre el 0 o el 1 */
        exec(img, bytes, orden);
    }
```

Si se hiciera en el padre, el shell se quedaria con el 1 apuntando al
fichero y no volveria a hablar con la consola nunca. Y en una tuberia va
**despues** del `dup2` del pipe, que es lo que hace que en `a > f | b` la
salida de `a` acabe en el fichero y `b` no vea nada: gana la ultima
redireccion, que es lo mismo que hace cualquier shell y sale solo de
respetar el orden en que se escribieron.

**Dos fallos que costaron lo mismo de encontrar y uno de arreglar.**

El primero: `openf` devolvia -1 siempre, con el mensaje "no puedo escribir
OUT.TXT". `PORT_KERNEL` es el 2, y el shell pide un puerto con
`port_create(-1)` -"el que sea"- y se llevaba justo ese. Ahora la busqueda
lo salta. Que el mensaje dijera QUE fichero y en QUE direccion fallaba
llevo derecho al sitio; si hubiera dicho "error de redireccion" habria
habido que ir a buscarlo.

El segundo: los ficheros salian con tamanyos multiplos de 96 y el texto
cortado. `message.len` son los bytes utiles de `data[]`, y yo le mandaba el
`sizeof` de la peticion entera, asi que el servidor escribia los 96 bytes
del buffer, ceros incluidos. Un campo mal interpretado, no un error de
logica, y por eso funcionaba *casi*: el sintoma no era "no escribe", era
"escribe de mas".

Y una comprobacion que conviene hacer siempre con `>`: que **vacia** un
fichero que ya existia.

```
    cat hola.txt > x.txt        ls -> X.TXT 116 bytes
    upper < hola.txt > x.txt    ls -> X.TXT  82 bytes
```

Si el segundo hubiera dejado 116 bytes, los ultimos 34 serian basura del
anterior, y el fichero pareceria correcto hasta que alguien llegara al
final. Por eso `O_ESCRIBIR` manda `FS_CREATE` antes de nada: crear y vaciar
son la misma operacion, y eso es exactamente lo que significa `>`.

**Un tercer fallo, que aparecio despues.** `cat hola.txt | upper > u.txt`
daba 116 bytes unas veces y 110 otras. La misma orden, dos resultados.

`fichero_write` escribia como mucho `FS_CHUNK` -96 bytes, lo que cabe en un
mensaje- y devolvia eso. Es **legal**: `write()` puede escribir menos de lo
que se le pide y el que llama esta obligado a repetir. Pero `upper` hacia
un solo `write` y se quedaba tan ancho, y con una tuberia detras eso nunca
habia fallado porque la tuberia se lo tragaba todo. Al ponerle un fichero
detras, empezo a perder el trozo que no cupo. Y lo peor: el principio y el
final del fichero estaban bien, asi que parecia correcto.

Se arreglo por los dos lados, que es lo que corresponde. `fichero_write` da
ahora tantas vueltas como haga falta, porque que una interfaz PERMITA
devolver menos no quiere decir que convenga hacerlo cuando se puede evitar.
Y `upper` repite el `write`, porque esa obligacion es suya y algun dia se
encontrara con alguien que si devuelva menos.

La moraleja repite una de antes: **una prueba que pasa con una tuberia no
te dice nada sobre un fichero.** El camino corto funcionaba por una
propiedad del otro extremo, no por ser correcto.

## Una libc: crt0, printf y libc.a

Hasta aqui cada programa empezaba asi:

```c
    void _start(int argc, char **argv) __attribute__((section(".text.start")));

    void _start(int argc, char **argv)
    {
        ...
        exit(0);
    }
```

y para imprimir un numero hacia esto, copiado de fichero en fichero:

```c
    static void num(const char *antes, uint64_t v, const char *despues)
    {
        char b[24];
        uint64_t n = udec(b, v);
        b[n] = 0;
        kprint(antes); kprint(b); kprint(despues);
    }
```

Estaba en **diez** de los dieciocho programas, con el mismo nombre y el
mismo cuerpo. Eso ya no es reutilizar poco: es una biblioteca escrita a
mano y sin darse cuenta.

**crt0, o por que main no es el principio.** Todo programa en C empieza en
`main`, y esa frase esconde una mentira util: el principio es `_start`. El
kernel entra por el punto de entrada del ELF con argc en `x0` y argv en
`x1`, que es justo donde el convenio de AArch64 espera los dos primeros
argumentos, asi que no hay nada que colocar. Lo que si hay que hacer es lo
de despues:

```asm
    _start:
        mov     x29, xzr        /* cortar la cadena de marcos de pila */
        mov     x30, xzr
        bl      main            /* argc y argv ya estan donde toca */
        bl      exit            /* main devolvio en w0; exit lo quiere ahi */
    1:  b       1b
```

Son cinco instrucciones, y son las que convierten `return 0` en un proceso
que termina bien. Sin ellas, un `main` que devuelve se va por el final de
la funcion y salta a una direccion basura.

**printf.** No es magia: es un bucle sobre una cadena que, al encontrar un
`%`, saca el siguiente argumento y lo convierte a texto. Lo unico que no se
puede escribir en C normal es "saca el siguiente argumento", y para eso
estan los `va_` de `<stdarg.h>`, que **si** da el compilador aunque no haya
libc: son parte del lenguaje, no de la biblioteca.

Hay un detalle que no es opcional en esta maquina: el modificador `l`. Un
`int` son 32 bits y un puntero o un `uint64_t` son 64. Sacar de la pila el
tamanyo equivocado no estropea *ese* numero, desplaza **todos los
siguientes**. De ahi que `%lu` y `%d` sean cosas distintas de verdad.

Y lo mismo por dentro: `printf` y `snprintf` comparten todo el codigo. Lo
unico que cambia es a donde van los caracteres, a un descriptor o a un
buffer:

```c
    struct destino { int fd; char *buf; size_t cap, n, total; };
```

`total` cuenta lo producido **quepa o no**, que es lo que permite a
`snprintf` decirte cuanto sitio habria hecho falta.

La salida a descriptor se acumula en 128 bytes y se vacia al llenarse. Sin
eso, cada caracter seria una llamada al sistema, y una llamada al sistema
cuesta una excepcion, un cambio de nivel y un viaje por la tabla de
vectores. Una linea de ochenta caracteres costaria ochenta.

**El atributo que encuentra fallos.** La declaracion lleva esto:

```c
    int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
```

y no es decoracion: hace que GCC compruebe que los `%` cuadran con los
argumentos. Nada mas convertir los programas salto un error real en
`hello.c` -un `unsigned int` impreso con `%lu`- que el `udec(b, v)` de
antes se tragaba sin rechistar, porque la conversion implicita a
`uint64_t` lo arreglaba por accidente. Una herramienta que solo sirve para
"no equivocarse al escribir" acaba encontrando cosas.

**libc.a, y por que el orden del enlazador importa.** La biblioteca se
archiva con `ar` y se pasa **al final**:

```
    $(CC) ... $(CRT0) user/prog.c $(LIBC) -o prog.elf
```

El enlazador recorre los ficheros **una vez**, y de un archivo solo saca
los objetos que ya sabe que le faltan. Puesto antes que quien lo usa, no
aporta nada. El crt0 va suelto y delante porque tiene que estar siempre.

El efecto se mide:

| programa | antes | ahora |
|----------|-------|-------|
| `upper`  | 4528  | 4576  |
| `ls`     | 5512  | 7384  |

`upper` no usa `printf` y crece 48 bytes; `ls` si, y se lleva los ~1,9 KB.
Eso es el enlazado selectivo funcionando: cada programa paga lo que usa.

**Una precaucion contada bien.** `lib/string.c` se compila con
`-fno-tree-loop-distribute-patterns`. Esa optimizacion reconoce un bucle de
copia byte a byte y lo sustituye por una llamada a `memcpy`; dentro de
`memcpy` eso es recursion infinita. Es un fallo famoso.

Aqui **no pasa**, y conviene saber por que antes de repetir la leyenda:
`-ffreestanding` le dice a GCC que no de por hecha la biblioteca estandar,
y eso ya desactiva la transformacion. Se comprueba desensamblando sin el
flag: `memcpy`, `memset` y `strcpy` salen como bucles, sin un solo `bl`.
El flag se pone igualmente, no porque haga falta hoy, sino para que la
correccion de `memcpy` no dependa de un efecto secundario de otro flag.

**Lo que queda del viejo syscall.h.** Al mudar `kprint`, `kgetc`, `exit`,
`malloc`, `ustrlen`, `ucopy` y `udec` a `<stdio.h>`, `<stdlib.h>` y
`<string.h>`, el fichero vuelve a ser lo que decia su nombre: la frontera
con el kernel, y nada mas. Esa es la otra mitad de tener una libc; no solo
gana el que escribe programas, gana tambien lo que queda debajo.

## Coma flotante, y el fallo como aviso

Desde el paso 1 todo el proyecto compila con `-mgeneral-regs-only`, que
prohibe usar los registros de coma flotante. No era un capricho: el kernel
no los salva, asi que si un hilo los usara, el siguiente encontraria los
suyos pisados. Prohibirlos era mas barato que salvarlos.

Salvarlos cuesta. Son 32 registros de 128 bits mas FPSR y FPCR: **528
bytes**. El contexto entero de un hilo -lo que salva `switch.S`- son 104.
Hacerlo en cada cambio de contexto multiplicaria por cinco el coste de
cambiar de hilo, y para nada en la mayoria de los casos: el planificador,
el recolector, el servidor de ficheros y el shell no hacen una sola
operacion en coma flotante en toda su vida.

**Asi que no se salvan. Se apaga la FPU y se espera a que alguien la
quiera.**

```
    CPACR_EL1.FPEN = 0b00        atrapar cualquier uso, en EL0 y en EL1
      -> un hilo toca un registro de FP
      -> excepcion sincrona con EC = 0x07
      -> se le reserva su area de 528 bytes (kmalloc, la primera vez)
      -> se le restaura lo que tuviera y se enciende la FPU
      -> se vuelve SIN tocar elr: la CPU reintenta la misma instruccion
```

Es el mismo patron que ya aparecio tres veces -la pila que crece, las
paginas bajo demanda, copy-on-write-: **el fallo no es un error, es el
aviso de que ha llegado el momento de hacer el trabajo.** Lo distinto es
que aqui lo que se difiere no es memoria, es estado de registros. Un hilo
que nunca hace una multiplicacion en coma flotante no paga ni un ciclo ni
un byte: ni area reservada, ni save, ni restore, nunca.

**Por que NO es perezosa del todo, que es la parte interesante.** La
version de libro es perezosa por los dos lados: al cambiar de hilo tampoco
se salva, se deja el estado en los registros, y si el hilo vuelve sin que
nadie los haya tocado se ahorra hasta la restauracion. Funciona perfecto en
una maquina de un solo nucleo.

Aqui hay cuatro. Si el hilo A deja su estado en los registros del nucleo 0
y luego lo planifican en el 1, el nucleo 1 **no tiene forma de traerselo**:
esta en unos registros que no son suyos. Sacarlo de ahi exige interrumpir
al nucleo 0 y pedirle por IPI que lo escriba en memoria, y para cuando has
montado eso la optimizacion ha dejado de ser barata. Los kernels de verdad
lo hacen; aqui se eligio lo otro.

**Se salva al salir y se restaura perezosamente al entrar.** Se conserva lo
que de verdad importa -quien no la usa no paga nada- y no queda estado vivo
fuera de su duenyo cuando un hilo cambia de nucleo. Lo que se pierde es una
excepcion por hilo y por turno entre los que si la usan.

**Y ahora la medida, que es lo que decide si la historia era cierta.** La
tecla `q` del menu lo cuenta. Tras arrancar los dos servidores y correr
`ls`, `wc` y `cat` desde el shell:

```
      veces que se ha encendido : 307
      hilos que la han pedido   : 5
      la tienen reservada ahora : 2
      hilos creados en total    : 16
```

Cinco de dieciseis. No es "casi nadie", que es lo que uno esperaria: **ni
`ls` ni `wc` ni `cat` hacen una sola cuenta con decimales**. Y sin embargo
piden la FPU. El motivo sale desensamblando `printf`:

```
    400e38:  stp  q0, q1, [sp, #256]
    400e3c:  stp  q2, q3, [sp, #288]
    400e40:  stp  q4, q5, [sp, #320]
    400e44:  stp  q6, q7, [sp, #352]
```

Es el prologo de una funcion variadica. El ABI de AArch64 obliga a salvar
v0-v7 al entrar en una funcion con `...`, **por si el formato lleva un
`%f`**. No se sabe hasta leer la cadena, y para entonces ya es tarde. Asi
que cualquier programa que llame a `printf`, aunque solo imprima enteros,
toca la FPU en su primera llamada.

La prueba en negativo la da `upper`, que filtra con `read`/`write` y no
llama a `printf` en ningun sitio: **cero instrucciones de FP en todo el
binario**, cero trampas, cero bytes reservados. Y los hilos del kernel
tampoco, porque el kernel sigue compilando con `-mgeneral-regs-only`.

**Y una prediccion, que es lo que convierte la historia en comprobacion.**
Arrancando solo los servidores y `fp`, la cuenta da cuatro: el servidor de
ficheros, el shell, `fp` y el hijo de `fp`. Falta el `conserver`, y hay que
explicar por que falta o el argumento no vale.

Falta porque su camino normal **no pasa por la libc**: imprime con
`hw_puts`, que escribe en el registro de la PL011 y no es variadica. Sus
dos `printf` son rutas de error que no se ejecutan, y su `snprintf` solo
salta cuando un cliente le manda un `CMSG_PRINT`. Si eso es cierto, basta
arrancar un cliente con `n` para que aparezca. Y aparece:

| escenario                | hilos que la han pedido |
|--------------------------|-------------------------|
| `f` `s` `z` + `fp`       | 4                       |
| lo mismo, mas `n`        | 6                       |

Los dos nuevos son el `conserver`, que ya ejecuta su `snprintf`, y el
cliente, que tambien es variadico.

Lo que separa ese experimento es una distincion que se confunde con
facilidad: **tener instrucciones de coma flotante en el binario no es lo
mismo que llegar a ejecutarlas.** El `conserver` tiene 28 y no paga ni un
byte mientras nadie le hable. Es exactamente de lo que vive este mecanismo:
no adivina quien va a usar la FPU, espera a que la use.

O sea: el ahorro es real, pero no por donde dice el libro. No lo decide
"quien hace cuentas con decimales", lo decide **quien llama a una funcion
variadica**. Es exactamente el tipo de cosa que no se sabe hasta medirla.

**Un contador que dice "hilos" tiene que contar hilos.** La primera version
de esa tabla mentia por dos sitios a la vez. Los hijos que heredan el area
en el `fork` no pasan por la trampa, asi que no se contaban; y `exec`
soltaba el area sin descontarla. En el escenario de arriba los dos errores
**se cancelaban** y el numero salia bien, que es la peor forma de estar
mal: no hay sintoma que te avise.

Se vio al correr otra secuencia -solo `fp`, sin `ls` ni `cat`- y encontrar
tres donde el razonamiento decia cinco. La cura fue poner toda la
contabilidad en un solo sitio, `fp_area_alloc()`, por donde pasan los dos
caminos, y marcar el hilo con `fp_pedida` para no contarlo dos veces cuando
hereda el area y luego la pierde en un `exec`.

**Dos detalles del hierro.**

El `isb` despues de escribir `CPACR_EL1` no es opcional. Cambiar ese
registro afecta a como se **decodifican** las instrucciones siguientes, y
la CPU ya tiene varias en vuelo: sin la barrera, la primera instruccion de
FP de despues puede haber pasado por el decodificador cuando FPEN todavia
valia cero, y atrapar otra vez. Un bucle infinito de trampas.

Y uno pequenyo que costo un error de ensamblado: `stp x1, x2, [x0, #512]`
no existe. El desplazamiento de `stp` se codifica en 7 bits con signo por
8, o sea hasta 504; el de `str` en 12 bits sin signo por 8, y llega de
sobra. Dos instrucciones que parecen la misma con limites distintos.

**Lo que hay que probar de verdad.** Que `2.5 * 4` da `10` no prueba nada:
un `fp_restore` roto da numeros correctos mientras no se cuele nadie entre
dos instrucciones. Por eso `user/fp.c` no comprueba cuentas, comprueba
supervivencia: deja un valor en la FPU, llama a `yield()` cincuenta veces
-cada una pasa por el planificador, que salva y apaga- y mira si sigue
ahi. Y luego hace un `fork` con un numero a medias, para probar que el
hijo lo hereda y que los dos siguen por su cuenta.

```
    50 sumas de 0.1     : 5.0000  ok
    [hijo]  heredado 3.75, x2 = 7.50  ok
    [padre] el mio sigue en 3.75  ok
```

El `fork` tiene su miga: antes de copiar el area del padre hay que
**bajarla** a memoria. Si el padre tiene la FPU encendida, sus registros
son la copia buena y lo que hay en su area es de hace un rato; copiar sin
bajarla le daria al hijo un estado viejo. Cuesta que el padre vuelva a
atrapar la proxima vez que la use, y no cuesta nada mas.

**Y de paso, `%f`.** La libc ya tiene decimales, con precision (`%.10f`) y
anchura (`%10.3f`). El redondeo se hace **antes** de partir el numero en
entero y fraccion, que es lo que hace que `0.9999` con dos decimales salga
`1.00` y no `0.99`: truncar no es redondear. `lib/` se compila sin
`-mgeneral-regs-only` precisamente por esto; el kernel, que si lo lleva, no
podria tener un `printf` con decimales aunque quisiera.

## Subdirectorios, y el directorio actual

El sistema de ficheros llevaba dieciseis pasos siendo el enano de la casa:
procesos que se bifurcan, se comunican, se senyalan y redirigen, y una
tarjeta plana con dieciocho ficheros en un solo monton.

**El raiz de FAT16 no es un directorio.** Esa es la primera sorpresa. Un
subdirectorio es un fichero normal cuyo contenido son entradas de 32 bytes:
una cadena de clusters, que crece como cualquier otra. El raiz, en cambio,
es una **region de sectores fija**, puesta detras de las FAT, con un numero
de entradas decidido al formatear y que no se puede cambiar nunca. No tiene
entrada de directorio en ninguna parte, porque no es hijo de nadie.

Son dos cosas distintas de verdad, y la unica forma de no escribir dos
veces cada recorrido es esconder la diferencia detras de una pregunta:

```c
    static int dir_sector(uint32_t dir, uint32_t n, uint32_t *lba);
```

"Dame el sector numero N de este directorio". El cluster 0 quiere decir el
raiz, y sirve de marca porque no existe como cluster de datos: los validos
empiezan en el 2. Con esa funcion, buscar, crear y listar son el mismo
codigo para los dos casos.

(De ahi viene, por cierto, que en FAT16 se pueda llenar el directorio raiz
teniendo el disco medio vacio. El raiz tiene tope; los demas no.)

**Resolver una ruta devuelve DOS cosas.** `/DOCS/NOTAS/A.TXT` se parte en
componentes y se baja una a una, parando **antes** de la ultima:

```c
    static int resolver(const char *ruta, uint32_t *dir, char *ultimo);
```

Lo que sale es el directorio que CONTIENE lo que se pedia, mas el nombre
suelto. Esa separacion no es un detalle de implementacion: crear, borrar y
renombrar necesitan las dos mitades por separado, porque lo que se toca es
la entrada DENTRO del directorio padre. Un "abrir" que solo devolviera el
fichero no valdria para ninguna de las tres.

## El directorio actual, o estado que no es memoria

Aqui esta lo que hace este paso distinto de los anteriores. Un proceso
tiene memoria, descriptores, senyales, registros de FPU. El directorio
actual **no es ninguna de esas cosas**: son sesenta y cuatro bytes de texto
que no significan nada para el hardware y todo para quien abre un fichero.

```c
    char cwd[FS_PATH_MAX];      /* en struct task */
```

Lo interesante es como se comporta en los tres momentos que ya conoces:

| suceso | senyales | FPU | cwd |
|--------|----------|-----|-----|
| `fork` | se heredan | se hereda | **se hereda** |
| `exec` | se borran  | se borra  | **sobrevive** |
| morir  | se pierde  | se libera | se pierde |

Esa casilla de `exec` es la unica de su columna, y es la que importa: el
programa cambia, el sitio donde estabas no. Sin ella, `cd docs` seguido de
`cat notas.txt` no funcionaria.

**Y de ahi sale que `cd` tenga que ser una orden interna del shell.** No
por comodidad ni por velocidad: si fuera un programa, el shell se
bifurcaria, el hijo cambiaria SU directorio -que se hereda, pero hacia
abajo- y al morir se lo llevaria consigo. El shell seguiria donde estaba.
Es la unica orden de este interprete que no puede ser un fichero en la
tarjeta, y es exactamente el mismo motivo por el que en cualquier Unix
tampoco lo es.

**Una sola implementacion de la normalizacion.** El servidor de ficheros
solo entiende rutas absolutas, y es a proposito: no tiene estado, y el
directorio actual es estado. Asi que alguien tiene que unir las dos cosas,
y ese alguien resulta ser el kernel dos veces: para la redireccion (`>`
sobre una ruta relativa) y para los programas, que hablan con el servidor
por su cuenta.

Dos implementaciones -una en el kernel, otra en la libc- acabarian
discrepando en algun caso raro, `/a/../..` o `a//b`, y el sintoma seria que
el shell y el programa no ven el mismo fichero. De ahi `SYS_realpath`: hay
una funcion, `path_resolve()`, y los programas usan la del kernel.

Lo que hace es **puramente textual**: no toca el disco, `.` se tira y `..`
quita la componente anterior. Conviene saber que eso solo es correcto
porque FAT no tiene enlaces simbolicos: con ellos, `/a/b/..` no tiene por
que ser `/a`.

**Un PATH de dos sitios.** En cuanto hay subdirectorios, `cd docs` te deja
sin ordenes: `ls` ya no esta donde estas. El shell busca ahora el
ejecutable primero en el directorio actual y luego en el raiz. Es la
version mas pequenya posible de una idea que en Unix es una variable de
entorno con ocho sitios, y esta aqui por el mismo motivo exacto.

## Lo que el disco dijo cuando le pregunte

Dos cosas salieron mal, y las dos merecen contarse.

**El raiz no sabia describirse a si mismo.** `cd ..` desde el primer nivel
fallaba con "no puedo entrar en ..". La causa estaba lejos del sintoma:
`chdir` comprueba que el destino existe y es un directorio, para eso
pregunta al servidor por `/`, y `resolver("/")` no encontraba ninguna
ultima componente que buscar. El raiz no tiene entrada de directorio en
ningun sitio, asi que no se puede **buscar**: hay que saberlo. Cuatro
lineas de caso especial, en el unico sitio donde el caso especial existe de
verdad.

**Y una hipotesis que resulto falsa.** Al listar, el `/prueba` que habia
creado TinyOS ensenyaba `.` y `..`, y el `/docs` que habia creado el Mac
no. Supuse que macOS no las escribia. Lo comprobe con un volcado del
sector, que es lo que habia que hacer antes de suponer nada:

```
     0  name='.          '  attr=0x12
     1  name='..         '  attr=0x12
     2  name='NOTAS      '  attr=0x10
     ...
     4  name='_NOTA~3    '  attr=0x22      <- el "._NOTAS" de macOS
```

macOS **si** las escribe. Lo que hace es marcarlas con 0x12: directorio
**mas oculto**. Y este servidor descarta las ocultas, porque macOS deja un
`._loquesea` al lado de cada fichero y sin ese filtro el listado es la
mitad basura. Asi que la MISMA entrada aparecia o no segun quien hubiera
creado el directorio.

Se arregla mirando el **nombre** y no el atributo, que es lo unico que da
el mismo resultado en los dos casos. Y deja una leccion sobre FAT que vale
mas que el arreglo: **los atributos son una sugerencia que cada sistema
rellena a su gusto**, y apoyarse en ellos para decidir QUE es algo sale
caro.

## Nombres largos, y el truco de 1995

Con subdirectorios funcionando, un `ls` de la tarjeta de verdad devolvio
esto:

```
    ENSA~209.ELF   101240 bytes
```

El nombre estaba ahi, entero, en el disco. Lo que pasaba es que este
servidor solo leia la entrada de 8.3 y tiraba las otras.

FAT guarda los nombres en 8.3 y punto. Los largos se anyadieron despues, y
el truco con el que se hizo es de los mas elegantes que hay: delante de la
entrada corta de toda la vida se ponen entradas EXTRA con el atributo
**0x0F**, que es solo-lectura + oculto + sistema + etiqueta-de-volumen a la
vez. Una combinacion que no tiene ningun sentido.

Y ahi esta la gracia. MS-DOS, que no sabia nada de esto, las descartaba
por absurdas y seguia viendo el disco entero con sus nombres cortos. Un
formato ampliado **sin romper a quien no entiende la ampliacion**, y sin
un bit de version en ninguna parte.

Cada entrada extra lleva 13 caracteres UTF-16 repartidos en tres huecos
(bytes 1-10, 14-25 y 28-31), porque tuvieron que colarse entre los campos
que ya existian. Van en orden **inverso**: la primera que aparece en el
disco es el ultimo trozo del nombre, y lleva el bit 0x40 para decir "por
aqui empieza".

Y una **suma de comprobacion del nombre corto**, repetida en cada trozo.
No es paranoia: si un sistema antiguo renombra el fichero, toca la entrada
corta y deja las largas huerfanas, apuntando a un nombre que ya no existe.
La suma detecta ese desacuerdo, y entonces se usa el corto, que siempre
esta. Es una decision de disenyo que asume que **otro sistema va a tocar
tus datos sin entenderlos**, y sigue funcionando cuando pasa.

El acumulador vive fuera del bucle de sectores, y no es un detalle: una
cadena de entradas largas puede empezar al final de un sector y acabar en
el siguiente, que es justo lo que pasa con los nombres mas largos.

Hay una tension que conviene ver: una componente puede medir 63
caracteres, pero la RUTA entera sigue midiendo 64. Un nombre largo cabe en
el raiz y no cabe tres niveles abajo. Subir el limite obligaria a bajar
`FS_CHUNK`, porque los dos salen del mismo mensaje de 256 bytes.

## Una interrupcion perdida, y una conclusion que estaba mal

Probando los nombres largos, el shell se quedaba a medias de una linea:
veintitantos caracteres y nunca mas. Lo di por un artefacto de las
pruebas -mi guion tecleaba mas rapido de lo que QEMU consume- y lo escribi
asi en las limitaciones.

**Era un fallo de verdad, y bueno.**

El driver de consola vaciaba la FIFO y **despues** reconocia la
interrupcion en el chip:

```c
    while (!(*fr & FR_RXFE)) { ...sacar bytes... }
    *icr = INT_RX | INT_RT;              /* <- tarde */
```

Si llega un byte entre el "ya esta vacia" y el reconocimiento, el
reconocimiento borra el aviso que ese byte acaba de levantar. Y el byte se
queda **dentro** de la FIFO. Como esta por debajo del umbral que dispara la
interrupcion, nadie vuelve a avisar nunca: la UART tiene datos, el driver
no lo sabe, y el teclado se muere en silencio a mitad de una linea.

La cura es reconocer **antes** y volver a mirar despues:

```c
    do {
        *icr = INT_RX | INT_RT;          /* primero */
        while (!(*fr & FR_RXFE)) { ...sacar bytes... }
    } while (!(*fr & FR_RXFE));          /* ¿entro algo mientras? */
```

Reconociendo antes, cualquier byte que llegue durante el vaciado deja su
aviso en pie. Y el bucle de fuera cubre el caso simetrico: si entro algo
justo despues del ultimo `FR_RXFE`, se ve ahi mismo en vez de esperar un
aviso que quiza no llegue.

**Lo que hay que aprender de esto no es el arreglo.** El arreglo son cuatro
lineas. Lo que costo caro fue haber tenido la explicacion correcta
delante -"se pierde entrada cuando tecleo deprisa"- y haberla archivado
como problema de la prueba en vez de seguir tirando. Una prueba que falla
de forma reproducible no es ruido aunque el fallo sea incomodo.

Tres cosas mas salieron de ahi, todas del mismo estilo:

- **`uart_push` tiraba caracteres en silencio.** El anillo son 64 bytes,
  quien lee va a su ritmo, y lo que sobra se pierde: es inevitable sin
  control de flujo en el cable. Lo que no tiene por que ser inevitable es
  no enterarse. Ahora dice `entrada demasiado rapida: 23 caracteres
  perdidos (179 en total)`, y con eso el sintoma deja de ser un misterio.

- **Enmascarar y no avisar mataba la consola.** En `irq_handle`, la
  interrupcion se cierra y luego se manda el mensaje al driver. Si el
  mensaje no cabe en la cola, la fuente quedaba cerrada esperando un
  `irq_ack` que nadie iba a hacer. Son dos pasos que tienen que pasar los
  dos o ninguno; ahora, si el aviso falla, se reabre.

- **El driver detecta el overrun del hardware.** Al leer `DR`, los bits de
  arriba no son datos: el 11 dice que llego un byte y no cabia. QEMU no
  parece modelarlo -nunca se encendio-, pero en silicio si pasa, y estaba
  sin mirar.

## mmap, o el kernel esperando a un programa sin privilegios

`mmap` no lee nada. Reserva un tramo de direcciones, apunta de que fichero
viene, y se va. La primera vez que el proceso toca una de esas paginas
salta un fallo de traduccion, y ahi se trae el trozo que hace falta.

Eso ya lo sabiamos hacer: es la pila que crece y es copy-on-write, por
cuarta vez. Lo nuevo, y es gordo, es **de donde sale el contenido**.

Hasta hoy, cuando el kernel necesitaba una pagina se la pedia al gestor de
paginas: codigo suyo, en su mismo nivel de privilegio, que contesta
siempre y enseguida. Ahora se la pide a un **proceso de EL0**. Manda un
mensaje al servidor de ficheros y se duerme hasta que conteste. El kernel,
a mitad de una instruccion que todavia no ha terminado de ejecutarse,
esperando a un programa sin privilegios.

**Y funciona porque "el kernel" no es nadie.** El hilo que falla es un hilo
normal, con su pila y su entrada en la tabla de tareas, que resulta estar
en modo privilegiado. Puede dormirse como cualquier otro, y mientras tanto
los otros tres nucleos siguen corriendo. Si el servidor no esta, la
peticion falla, el proceso muere, y el sistema sigue. Esa frase -"el kernel
no es un proceso, es un modo"- llevaba treinta y dos pasos siendo una
definicion; aqui es lo que hace que la cosa no se caiga.

**La medida, que es lo que hay que mirar.** `map` imprime las paginas
libres del sistema en tres momentos:

```
    / $ map ls.elf
      ls.elf mapeado en 0x30000000, 8720 bytes
      paginas libres: 245364 antes -> 245364 despues de mapear
      (mapear no gasta memoria: todavia no se ha leido nada)

      leo el primer byte ('') -> quedan 245362 paginas
      lo recorro entero      -> quedan 245360 paginas
```

Mapear un fichero de 8.720 bytes cuesta **cero paginas**. Tocar el primer
byte cuesta dos: una es la pagina de datos y la otra es un nivel de tabla,
porque la zona de mapeos esta en 0x30000000 y ahi no habia todavia ninguna
tabla de nivel 3. Recorrerlo entero trae las dos que faltaban. Con
`hola.txt`, que son 82 bytes, la ultima linea no gasta nada: cabia entero
en la primera pagina.

Es el mismo perfil que la pila que crece, pero ahora medido contra un
fichero de verdad y con el disco de por medio.

**Lo que cuesta, dicho claro.** Un mensaje lleva `FS_CHUNK` = 176 bytes
utiles, asi que llenar una pagina de 4 KB son **24 viajes** de ida y vuelta
al servidor. Es lento, y es el precio de que el sistema de ficheros no este
en el kernel. Se podria arreglar con un protocolo que devuelva una pagina
entera, o con memoria compartida entre el servidor y el kernel; las dos
cosas son mas mecanismo, y ninguna cambia la idea.

**El peligro de invertir la dependencia.** Si el que falla es el PROPIO
servidor de ficheros, se esta esperando a si mismo: no queda nadie para
contestarle, y con el se cuelga todo el que quiera leer algo. No hay forma
elegante de evitarlo -es de fondo, viene de que el kernel dependa de un
proceso- y la unica manera honesta de tratarlo es nombrarlo y cortarlo en
seco:

```c
    if (t->pid == port_owner(PORT_FILES)) return -1;
```

Mejor un `mmap` que devuelve -1 que un sistema que se para sin decir por
que.

**De solo lectura, y a proposito.** No hay nada que devuelva los cambios al
disco. Un mapeo que se deja escribir y luego pierde lo escrito al morir el
proceso es peor que uno que no deja: el fallo aparece tarde y en otro
sitio. `map -w` lo comprueba, y muere como debe:

```
    Causa   : EC=0x24  Data Abort desde EL inferior
    Detalle : Fallo de permisos (escritura)
    Direccion (FAR_EL1) : 0x0000000030000000
```

**En el fork, pereza doble.** Los mapeos se heredan. Las paginas que ya
estaban dentro las copia `vmm_fork` como cualquier otra, en
copy-on-write; las que no, volveran a pedirse al servidor cuando el hijo
las toque. Una pereza sobre la memoria y otra sobre el disco, encajadas
sin que ninguna sepa de la otra.

**Y un tropiezo que ya es viejo conocido.** `t->mapeos[i] = padre->mapeos[i]`
son 80 bytes, y gcc convierte eso en una llamada a `memcpy` que en el
kernel no existe. Tercera vez en este proyecto, y siempre se ve igual: no
falla el compilador, falla el enlazador, y el mensaje no menciona ninguna
estructura.

## exec por mmap, o comprobar pasa a ser conseguir

El shell cargaba un programa asi: preguntar el tamanyo, reservar ese
tamanyo con `malloc`, y dar vueltas pidiendole trozos al servidor de
ficheros hasta llenarlo. Cuarenta lineas. Luego le pasaba ese buffer a
`exec`, y el kernel copiaba de ahi a las paginas del programa nuevo.

Con `mmap` son dos lineas. Pero lo interesante no es el codigo que
desaparece del shell: es **lo que hubo que cambiar en el kernel** para que
pudiera desaparecer.

**El kernel daba por hecho que la memoria de usuario estaba.** Antes de
leer un puntero que viene de EL0, comprobaba:

```c
    uint64_t ok = for_write ? user_touch_w(p) : vmm_translate_user(p);
```

`vmm_translate_user` es `at s1e0r`: le pregunta a la MMU si EL0 puede leer
ahi. Si dice que no, error. Eso valia mientras "estar mapeada" fuera una
propiedad **estable**: o estaba o no estaba, y preguntar no cambiaba nada.

Con ficheros mapeados deja de serlo. La pagina no esta, **pero puede
estarlo** si alguien la pide. Preguntar antes de tocar devuelve un "no" que
en realidad era un "todavia no", y `exec` sobre un ELF mapeado fallaba sin
haber intentado nada.

**Asi que se cambia comprobar por conseguir:**

```c
    int user_touch_r(uint64_t va)
    {
        if (vmm_translate_user(va)) return 1;                       /* ya esta */
        if (task_mmap_fault(va) && vmm_translate_user(va)) return 1;/* traela  */
        if (task_grow_stack(va, va) && vmm_translate_user(va)) return 1;
        return 0;                                                   /* nada que hacer */
    }
```

El kernel ya no pregunta si puede leer: **hace lo que haga falta para
poder**, y solo falla cuando no queda nada que intentar. Es el gemelo de
`user_touch_w`, que llevaba desde el paso 23 haciendo lo mismo para
escribir -COW y crecer la pila- sin que se viera que era un patron.

Esa funcion es el paso entero. Todo lo demas sale gratis: `read`, `write`,
`msg_send` y `exec` pasan por el mismo sitio, asi que todos aceptan ya
memoria que aun no existe.

**Y trae una regla nueva que hay que respetar.** `user_touch_r` puede
DORMIR: traer una pagina mapeada es un viaje al servidor de ficheros. Antes
comprobar era una instruccion (`at`) y no podia bloquear; ahora puede. Por
eso `file.c` copia a un buffer intermedio ANTES de entrar en sus secciones
criticas, y no al reves. Esa decision se tomo en el paso 26 por otro motivo
-los fallos de pagina y `sched_lock`- y resulta que ya protegia de esto.

**munmap, que hacia falta de verdad.** Un proceso tiene cuatro ranuras de
mapeo y el shell mapea un fichero por cada orden: sin soltarlos, a la
quinta orden se queda sin sitio. `task_munmap` quita del mapa las paginas
que se llegaron a traer y devuelve la ranura. Las que nunca se tocaron no
estan mapeadas, asi que `vmm_unmap_in` dice -1 y no pasa nada; y las que el
`fork` dejo compartidas solo bajan un contador.

Se comprueba pidiendo las paginas libres antes y despues de seis
ejecuciones:

```
    / $ map hola.txt         paginas libres: 245369
    / $ ls   (x6)
    / $ map hola.txt         paginas libres: 245369
```

Ni una. Conviene medirlo asi y no mirando la memoria total al final: la
primera vez que lo intente salieron nueve paginas de diferencia, y no eran
una fuga sino el propio shell todavia sin recoger por el recolector. Una
medida que incluye lo que no querias medir no vale.

**Lo que se ahorra es una copia, no memoria.** El fichero ya no pasa por un
`malloc` del shell para que el kernel lo copie de ahi: va del servidor a
las paginas del programa nuevo. `run` pierde ademas un array estatico de
32 KB que reservaba siempre, se usara o no.

## Dos particiones, dos sistemas de ficheros

La tarjeta de la Pi tiene dos particiones: `boot` en FAT16, que es la que
lee el firmware, y `DATA` en FAT32. Hasta aqui el servidor montaba la
primera que encontrase y hacia como que la otra no existia. Ahora `DATA`
es `/` y `boot` cuelga de `/boot`.

```
    [fs] /      FAT32  8 sectores/cluster  115218 clusters
    [fs] /boot  FAT16  4 sectores/cluster   31181 clusters
```

**FAT16 y FAT32 son el mismo formato con dos diferencias que importan.**

La primera: en FAT16 el directorio raiz es una **region fija** detras de
las FAT, con un numero de entradas decidido al formatear. En FAT32 esa
region no existe: el raiz es una cadena de clusters como cualquier
directorio, y el BPB dice por cual empieza. Es mejor en todo -crece, no
tiene tope- y la unica razon de que FAT16 no lo hiciera asi es que en 1983
habia que poder encontrar el raiz sin leer la FAT.

Para este servidor el cambio fue de dos lineas, y solo porque el paso 31
ya habia escondido la diferencia entre el raiz y los demas detras de
`dir_sector()`. Sin aquello, habria habido que tocar cada recorrido.

La segunda: las casillas de la tabla miden 16 o 32 bits. De ahi los
nombres. Y hay un detalle que se olvida siempre: **los cuatro bits de
arriba de una casilla FAT32 estan reservados y no son parte del numero**.
Hay que enmascarar al leer y conservarlos al escribir; si no, un volumen
que los traiga a uno da clusters astronomicos y la cadena se va a paseo.

**Y una cosa que NO esta en el BPB: de que tipo es.** No hay ningun campo
que lo diga. El `"FAT16   "` que se ve en el sector es una etiqueta que
nadie garantiza y que ningun sistema serio mira. La forma oficial de
averiguarlo es **contar los clusters**: menos de 4085 es FAT12, menos de
65525 es FAT16, y el resto FAT32.

O sea que el tipo de un volumen FAT es una **consecuencia de su tamanyo**,
no un dato. Eso tiene un efecto practico inmediato: la imagen de pruebas
tuvo que pasar de 64 MB a 512, porque por debajo de cierto tamanyo
`diskutil` formatea FAT16 aunque le pidas FAT32. No puede hacer otra cosa.

**La tabla de montajes.** Diez variables globales con la geometria pasaron
a ser una `struct volumen` que se pasa como argumento a casi todo. Se
podria haber dejado un puntero global al "volumen actual" y ahorrarse el
refactor -el servidor atiende una peticion cada vez, asi que seria
correcto-, pero un estado global que hay que acordarse de poner antes de
cada operacion es justo la clase de cosa que funciona hasta el dia que
alguien anyade un camino nuevo y se olvida.

Elegir volumen es quedarse con el punto de montaje **mas largo** que case,
porque `/` casa con todo:

```
    /boot/config.txt  ->  volumen de arranque, "/config.txt"
    /hola.txt         ->  volumen de datos,    "/hola.txt"
```

Y el punto tiene que terminar en barra o en fin de cadena, o
`/bootcode.bin` se lo quedaria `/boot` y buscaria un `code.bin` que no
existe. Es el mismo error que comparar prefijos de URL sin mirar el
separador.

**Un directorio que no esta en ningun disco.** Al listar `/`, el servidor
ensenya `boot` como `<dir>` aunque ahi no hay ninguna entrada: sale de la
tabla de montajes. Es la primera vez que este servidor ensenya algo que no
ha leido de un sector.

En Unix esto se hace al reves: el punto de montaje tiene que existir como
directorio de verdad en el volumen de abajo, y al montar queda **tapado**.
Tiene sus ventajas -no hay que inventar entradas- y una consecuencia
famosa: si montas sobre un directorio que tenia cosas, dejan de verse sin
haberse borrado. Aqui no hay tal directorio, asi que se anyade. Mas
simple, y se ve mejor lo que es un montaje: un trozo de nombre que lleva a
otro sitio.

**El fallo, que fue de los de mirar donde no era.** `ls /boot` se listaba
a si mismo: ensenyaba `boot <dir>` dentro de `/boot`. La causa no estaba
en la tabla de montajes sino en que le pasaba la ruta **ya recortada**:
`volumen_de()` le quita el punto de montaje, asi que la funcion que busca
"que montajes cuelgan de aqui" recibia `/` y se encontraba a si misma. Una
funcion correcta con el argumento equivocado.

**La comprobacion que de verdad vale.** Que TinyOS lea lo que TinyOS
escribe no prueba gran cosa: un sistema de ficheros mal escrito es
perfectamente capaz de entenderse consigo mismo. Lo que hay que comprobar
es que **lo entienda otro**:

```
    /nuevo $ cat /boot/aviso.txt > copia.txt     (lee FAT16, escribe FAT32)

    $ ls -l /Volumes/DATA/NUEVO/
    -rwx------  77  COPIA.TXT
    $ cat /Volumes/DATA/NUEVO/COPIA.TXT
      --- /boot/aviso.txt ---
    Soy el de la particion de arranque.
      --- fin ---
```

macOS lee el directorio que creo TinyOS y el fichero que escribio, byte
por byte. Eso es lo que dice que las estructuras estan bien y no solo son
consistentes con quien las puso.

## /usr/bin, y un PATH de verdad

Con la tarjeta organizada, los ejecutables dejan de estar tirados en el
raiz y se van a `/usr/bin`. El shell los busca ahi:

```c
    static const char *PATH[] = { 0, "/usr/bin" };   /* el 0 es "donde estoy" */
```

Dos sitios: el directorio actual y `/usr/bin`. Es una lista escrita en el
codigo y no una variable de entorno porque este sistema todavia no tiene
entorno, pero la idea es la de siempre: **un programa se llama por su
nombre y alguien decide donde se busca**.

Que el directorio actual vaya primero es comodo, y en Unix **no** se hace:
ahi `.` no esta en el PATH por defecto, porque entrar en un directorio
ajeno y escribir `ls` podria ejecutar el `ls` que haya dejado el duenyo del
directorio. Aqui no hay varios usuarios, asi que no hay a quien enganyar.

Y cuando no lo encuentra, lo dice entero:

```
    /docs $ noexiste
      NOEXISTE.ELF: no lo encuentro. He mirado en:
        /docs
        /usr/bin
```

Un "no encuentro" a secas manda a pensar que el fichero no esta; ensenyar
la lista dice que quiza esta, pero en otro sitio.

## Comillas, y quien parte una linea

`cat "un nombre bastante largo.txt"` no funcionaba: el shell partia por
espacios y el programa recibia `"un` como nombre.

Arreglarlo tiene una pega de disenyo que conviene ver antes que el codigo.
**En Unix el que parte la linea es el shell**, que le pasa al kernel un
array de cadenas ya hecho; el kernel no sabe lo que es una comilla ni
falta que le hace. Aqui el convenio es otro -`exec` recibe UNA cadena y la
parte `build_args`- asi que el que tiene que entender las comillas es el
kernel.

No es lo ideal: mete politica de interfaz de usuario en un sitio donde no
pinta nada. La alternativa es cambiar el convenio de `exec` y `spawn` para
pasar un array, y eso es otro paso. Queda dicho en el codigo.

Y hay una segunda pega, esta visible: la misma regla acaba escrita en
**cuatro sitios**. El kernel para los argumentos, y el shell tres veces
-el nombre del programa, el destino de `<` y el de `>`-. Cuatro sitios
para una sola regla es de esas cosas que funcionan hasta el dia que
alguien anyade un quinto y se olvida.

**El fallo, que fue mio y de hoy.** La primera version compactaba la
cadena **sobre si misma**: copiaba cada palabra sin comillas unos bytes
mas atras, en el mismo buffer. Parecia seguro porque el que escribe nunca
adelanta al que lee... salvo en un sitio. Al cerrar una palabra se escribe
un cero, y en la primera palabra ese cero cae **exactamente encima del
espacio que se iba a leer a continuacion**. A partir de ahi todo se
descuadra en uno.

El sintoma no se parecia en nada a la causa:

```
    / $ hello "dos palabras" tres
      >> me han llamado con 2 argumento(s): [hello] []
```

La cura fue leer de un sitio y escribir en otro: la fuente es la cadena
que llega, el destino es la pila del proceso nuevo. Dos buffers, y el
problema desaparece en vez de esquivarse.

```
    / $ hello "dos palabras" tres
      >> me han llamado con 3 argumento(s): [hello] [dos palabras] [tres]
```

**Y negarse en vez de apanyarselo.** Con comillas ya se podia escribir
`cat hola.txt > "con espacios.txt"`, y el servidor lo creaba: `a_8_3()`
convertia el nombre en `CON ESPATXT`, que ningun sistema sabe volver a
escribir igual y que al listarlo sale como `CON.TXT`, porque `de_8_3()`
para en el primer espacio. Un fichero que se crea con un nombre y aparece
con otro.

Los nombres largos se **leen** pero no se escriben -eso exigiria generar
la cadena VFAT y, peor, inventar un nombre corto que no choque-, asi que
lo correcto mientras tanto es un error:

```
    / $ cat hola.txt > "con espacios.txt"
      no puedo escribir CON ESPACIOS.TXT
      (los nombres nuevos tienen que caber en 8.3: sin espacios)
```

## El fallo mas caro de este paso no estaba en el codigo

Estaba en el Makefile, y conviene contarlo porque es de los que no se ven
en una revision.

`make sdtest` crea la imagen de dos particiones y escribe en ellas. Las
particiones se llaman BOOT y DATA, asi que el script escribia en
`/Volumes/BOOT` y `/Volumes/DATA`. Parece razonable.

Lo que pasa es que si hay **una tarjeta de verdad puesta en el Mac** -que
es exactamente lo que pasa cuando estas trabajando en esto- ya hay un
`/Volumes/DATA`, y es el de la tarjeta. macOS entonces monta la imagen
como `/Volumes/DATA 1`. El script no se entera y escribe en la **tarjeta**.

El sintoma fue que TinyOS arrancaba y no encontraba nada: la imagen estaba
vacia porque todo habia ido a otro sitio. El sintoma no dijo nada del
problema real, que era que una herramienta de construccion estaba
escribiendo en un dispositivo que no era el suyo.

La cura es no adivinar nunca un punto de montaje:

```sh
    B=$(diskutil info -plist ${DEV}s1 | plutil -extract MountPoint raw -)
    D=$(diskutil info -plist ${DEV}s2 | plutil -extract MountPoint raw -)
    case "$B$D" in /Volumes/*) ;; *) echo "puntos raros"; exit 1;; esac
```

Se pregunta por el **dispositivo**, que es lo unico que se sabe con
certeza, y se comprueba la respuesta antes de usarla. Un nombre de volumen
es una etiqueta que cualquiera puede repetir; el numero de disco no.

## exec con argv[], o devolver cada cosa a su sitio

El paso anterior dejo una costura fea y a la vista. `exec` recibia UNA
cadena y la partia el kernel, asi que el kernel tenia que entender
comillas -politica de interfaz de usuario en un sitio donde no pinta
nada- y la misma regla acababa escrita en cuatro sitios: el kernel, y el
shell tres veces (el nombre del programa, el destino de `<` y el de `>`).

Ahora `exec` y `spawn` reciben un **array de punteros terminado en cero**,
igual que el que recibe `main`. La diferencia es exactamente la que hay
entre *"aqui tienes una linea, apanyate"* y *"aqui tienes los
argumentos"*.

**Lo que adelgaza en el kernel.** `build_args` era treinta lineas de
comillas y estados; ahora son dos bucles que copian. Y lo que el kernel
gana no es brevedad, es **no saber**: `grep comilla src/` ya solo encuentra
comentarios.

**Lo que aparece en el kernel.** Traerse un `argv[]` son dos niveles de
indireccion en memoria ajena: el array de punteros, y detras de cada
puntero una cadena. Cada uno hay que comprobarlo por separado, porque el
array puede salirse de lo mapeado a mitad y cada cadena tambien. Por eso
`copiar_args` mira pagina a pagina mientras copia, en vez de fiarse de un
tamanyo que el proceso no ha dicho.

**Y lo que desaparece en `run`.** Tenia una funcion que juntaba
`argv[1..]` en una cadena separada por espacios... para que el kernel la
volviera a separar. Ahora es esto:

```c
    int64_t pid = spawn(imagen, total, argv + 1);
```

El propio `argv` ya viene terminado en cero, asi que saltarse el nombre de
`run` deja exactamente los argumentos del hijo. Cuando un cambio hace
DESAPARECER codigo en vez de moverlo, suele ser senyal de que el problema
estaba mal planteado.

## Un troceador, y solo uno

Devolver el troceo al shell no bastaba: alli seguia habiendo tres trozos
de codigo que tenian que saber lo que es una comilla, y tres reglas
iguales escritas aparte son tres reglas que pueden discrepar.

La cura fue que el troceador emita `<` y `>` **como fichas sueltas**,
aunque vayan pegadas a una palabra:

```
    cat hola.txt>pegado.txt   ->   [cat] [hola.txt] [>] [pegado.txt]
```

Con eso, la redireccion se resuelve mirando el **array** en vez de
cortando la cadena: se busca la ficha, se coge la siguiente como nombre, y
las dos salen de `argv`. El programa no llega a ver ni el `>` ni el
fichero, que es lo correcto.

De cuatro sitios que sabian de comillas se pasa a uno. Y salen gratis dos
cosas que antes no funcionaban: la redireccion pegada sin espacios, y un
error decente cuando falta el nombre.

```
    / $ cat hola.txt >
      falta el fichero despues de >
```

**Una estructura por orden, con su texto dentro.** Cada orden preparada
lleva su propio almacen:

```c
    struct orden {
        char *argv[MAX_ARGV + 1];
        char  texto[MAX_LINEA];
        char  ent[FS_PATH_MAX], sal[FS_PATH_MAX];
        int   hay;
    };
```

No es por comodidad: una tuberia son **dos ordenes vivas a la vez**, y con
un buffer compartido la segunda pisaria a la primera. Es el mismo error de
aliasing de ayer -escribir donde se lee- visto desde otro angulo, y esta
vez evitado por construccion en vez de por cuidado.

## Las senyales guardan la coma flotante

Esto llevaba siete pasos en la lista de limitaciones, con esta coletilla:
*"real, y de los que no se notan hasta que se notan"*. Era la unica cosa
del documento que estaba **mal** en vez de **sin hacer**.

Cuando llega una senyal, el kernel guarda el contexto del proceso en su
pila y lo hace "aparecer" dentro del manejador. Al volver, restaura. Pero
solo restauraba los registros **enteros**: si el manejador usaba decimales,
le pisaba la coma flotante al programa interrumpido, que volvia a su bucle
con otros numeros y sin manera de saber por que.

La cura es meter los 528 bytes de la FPU en el marco de senyal, detras del
contexto de siempre:

```
    sp                          -> los registros enteros
    sp + sizeof(trap_frame)     -> la FPU, si este proceso la usa
```

**Solo si la usa**, y eso no es tacanyeria. La mayoria de los procesos no
toca la coma flotante en su vida; hacerles pagar 528 bytes de pila en cada
senyal seria cobrarles por algo que no tienen. Es la misma pereza del paso
30, ahora aplicada a la pila en vez de a los registros.

Hay un detalle de orden que importa: antes de copiar hay que **bajar a
memoria** lo que este vivo en los registros. Si el proceso tiene la FPU
encendida, la copia buena esta en el silicio y la de `t->fp_state` es de
la ultima vez que lo desalojaron. Copiar sin bajarla guardaria un estado
viejo, y el sintoma seria que la senyal a veces se traga unas decimas.

Y un caso pequenyo al volver: si el proceso **no** habia tocado la FPU y el
manejador **si**, no hay nada que restaurar, pero dejarle los numeros del
manejador seria que el programa se encontrara luego basura con la que no
contaba. Se suelta el area, y la proxima vez nace a cero.

## Una prueba que falla cuando debe

`user/trap.c` lleva ahora una cuenta con decimales en su bucle principal y
ensucia la FPU a proposito dentro del manejador:

```
    [trap] vuelta 12, mi cuenta va por 1.500  (12 x 0.125)
    [trap] atrapada la senyal 2, van 1
    [trap] y de paso ensucio la FPU: 4987.385
    [trap] vuelta 13, mi cuenta va por 1.625  (13 x 0.125)
```

La columna de la derecha es la comprobacion: la cuenta tiene que valer
exactamente `i x 0.125` aunque hayas pulsado Ctrl-C en medio.

**Pero que una prueba pase no dice nada hasta saber que falla cuando debe.**
Aqui habia una duda razonable: `cuenta` es una variable local, y el
compilador podria haberla dejado en la pila entre llamadas, en cuyo caso
ninguna senyal la tocaria y la prueba pasaria igual sin arreglar nada.

Asi que se comprobo al reves: desactivando el guardado y volviendo a
correrla.

```
    [trap] vuelta 12, mi cuenta va por 1.500  (12 x 0.125)
    [trap] atrapada la senyal 2, van 1
    [trap] vuelta 13, mi cuenta va por 0.000  (13 x 0.125)
    [trap] vuelta 14, mi cuenta va por 0.000  (14 x 0.125)
```

Se va a cero **y ya no vuelve**: la cuenta vivia en un registro de coma
flotante de los que el ABI obliga a conservar entre llamadas (d8-d15), y
el manejador se lo llevaba por delante. La prueba mide lo que dice medir.

Es la tercera vez en este proyecto que hace falta este paso -romper el
codigo a proposito para ver si la prueba se entera- y las tres veces ha
cambiado lo que sabiamos.

## rmdir y mv, o deshacer lo que se hizo

El sistema sabia crear directorios y no borrarlos. Una asimetria fea, y de
las que se notan: un sistema de ficheros que solo va hacia adelante no
esta terminado.

**Lo primero fue comprobar si ademas corrompia.** Estaba apuntado que `rm`
sobre un directorio "no lo comprueba y puede dejar una entrada sin sus
datos". Resulto ser falso: `dir_lookup` solo busca ficheros, asi que el
directorio no aparecia y `rm` decia *"no esta en la tarjeta"*. No
destruia; **mentia**. Y mandaba a buscar donde no era, que es el mismo
pecado de siempre: un error que junta causas distintas.

```
    / $ rm docs
      docs es un directorio: usa rmdir
```

**Que quiere decir "vacio" en FAT.** No "sin entradas": un directorio
recien hecho ya trae `.` y `..`, que las pone quien lo crea. Vacio es "sin
nada MAS que esas dos".

Y las ocultas **si** cuentan. macOS deja un `._loquesea` al lado de cada
fichero; al listar se descartan, pero ocupan sitio de verdad, y borrar el
directorio se los llevaria por delante sin avisar. Mejor negarse.

**El orden al borrar.** Primero la entrada, despues la cadena de clusters.
Al reves, un corte de corriente a mitad dejaria una entrada apuntando a
clusters que ya estan en el monton de libres, y eso no pierde un
directorio: pierde **lo que venga luego**, cuando alguien reutilice esos
clusters creyendo que son suyos.

## mv: los datos no se mueven

Mover y renombrar son la misma operacion, y ninguna de las dos toca los
datos. Se escribe la entrada de directorio en otro sitio y se quita la de
antes; los clusters se quedan donde estaban.

De ahi salen dos cosas que sorprenden hasta que se ve por que:

- renombrar un fichero de un giga cuesta **lo mismo** que uno de cero
  bytes;
- y mover entre particiones **no se puede**, porque ahi ya no vale cambiar
  un nombre de sitio: hay que copiar los bytes. Eso lo hacen `cp` y `rm`,
  y cuesta lo que pesa.

**El detalle bonito: el `..` hay que ir a corregirlo.** En FAT no existe un
indice de padres en ninguna parte. Cada directorio lleva escrito dentro,
en su entrada `..`, quien es el suyo. Asi que mover un directorio no es
mover su entrada: es mover su entrada **y entrar dentro** a decirle quien
es su padre ahora.

```c
    if (es_dir && dir_o != dir_d && cluster) {
        ...buscar ".." dentro del propio directorio y reescribir su cluster...
    }
```

Y por lo mismo hay que impedir meter un directorio dentro de si mismo: el
arbol dejaria de ser un arbol, y cualquiera que lo recorriera no pararia
nunca. Se comprueba subiendo desde el destino por los `..` a ver si
aparece el origen.

**El orden al renombrar, otra vez.** La entrada nueva primero y la vieja
despues. Si se corta la luz en medio queda el mismo fichero con dos
nombres: feo, y lo arregla un `fsck`. Al reves se habria perdido. Entre
dos formas de romperse se elige siempre la que deja los datos
alcanzables.

**Y una comodidad que NO va en el servidor.** `mv fichero directorio`
quiere decir "metelo dentro", que es lo que espera cualquiera. Eso se
resuelve en `mv.c`, preguntando primero si el destino es un directorio y
componiendo la ruta. Abajo solo hay *"renombra esto asi"*. Es la frontera
de siempre: el servidor da el mecanismo, la orden pone la costumbre.

## Que lo diga otro: fsck_msdos

Comprobar que TinyOS lee lo que TinyOS escribe no vale para nada,
especialmente aqui: el `cd ..` de este sistema es **textual**, lo resuelve
el kernel sobre la cadena, y funcionaria igual aunque el `..` del disco
estuviera mal.

Asi que lo dice otro:

```
    $ fsck_msdos -n /dev/rdisk5s2
    ** Phase 1 - Preparing FAT
    ** Phase 2 - Checking Directories        <- aqui se validan los ".."
    ** Phase 3 - Checking for Orphan Clusters
```

Limpio despues de crear directorios, moverlos de sitio, borrarlos y sacar
ficheros de dentro. Y de paso encontro algo que no habriamos visto:

```
    Warning: Free space in FSInfo block (115117) not correct (115116)
```

FAT32 guarda en un sector aparte -el **FSInfo**- cuantos clusters quedan
libres y por donde seguir buscando. Son un **atajo**, no la verdad: la
verdad esta en la FAT. Nosotros no llevamos esa cuenta, asi que en cuanto
tocamos la tabla el numero de ahi deja de valer.

Dejarlo como estaba seria peor que no tenerlo: **un numero que parece
bueno y no lo es**. El estandar preve exactamente este caso y permite
ponerlo a `0xFFFFFFFF`, que quiere decir "no lo se, cuentalo tu". Eso se
hace ahora, una vez, la primera vez que se escribe algo. Y fsck lo nota:

```
    Warning: Free space in FSInfo block is unset (should be 115116)
```

De "esta mal" a "no esta". Es peor informacion y es mejor dato.

**Cuatro errores donde habia uno.** `FS_ES_DIRECTORIO`, `FS_NO_VACIO` y
`FS_EXISTE` se suman a `FS_ERROR`. No es burocracia: "no existe", "es un
directorio", "no esta vacio" y "ya existe" mandan a sitios distintos, y
juntarlos obliga a quien pregunta a adivinar.

## Escribir nombres largos, y de donde sale el ~1

Desde el paso 32 se **leian** nombres largos y no se escribian. Podias
abrir `un nombre bastante largo.txt` y no podias crear ninguno parecido:
el servidor se negaba, porque `a_8_3()` habria hecho un destrozo.

Escribir la cadena VFAT es mecanico -trece caracteres por entrada, en
orden inverso, con la suma repetida- y ya estaba medio hecho de leerla. Lo
que tiene miga de verdad es otra cosa.

**Todo fichero con nombre largo tiene TAMBIEN un nombre 8.3**, y hay que
inventarselo. No es decoracion: es el que ve un sistema que no entienda
VFAT, y es el que lleva la suma de comprobacion que ata la cadena a su
duenyo. La receta es coger las primeras letras que valgan, tirar espacios
y signos raros, subir a mayusculas y pegar `~1`. Si ya existe, `~2`.

```
    "con espacios.txt"         ->  CONESP~1.TXT
    "un directorio largo"      ->  UNDIRE~1
    "saludo de prueba.txt"     ->  SALUDO~1.TXT
```

Y con tres que colisionen, lo esperable:

```
    "nombre muy largo uno.txt"   ->  NOMBRE~1.TXT
    "nombre muy largo dos.txt"   ->  NOMBRE~2.TXT
    "nombre muy largo tres.txt"  ->  NOMBRE~3.TXT
```

**De aqui salio el `ENSA~209.ELF`** que aparecio en la tarjeta de verdad
hace unos pasos. Cuando hay muchas colisiones el numero crece y se come
las letras: primero `ENSAMB~1`, luego `ENSAM~10`, luego `ENSA~209`. Un
nombre generado no es un nombre elegido, y se nota.

Lo caro es comprobar que esta libre: hay que mirar el directorio entero
por cada intento. Con directorios de decenas de entradas da igual; en un
sistema de verdad esto se resuelve con un hash del nombre largo.

**Huecos seguidos, no huecos sueltos.** Las entradas de nombre largo
tienen que ir pegadas justo delante de la corta -asi es como se sabe
cuales son suyas- asi que hace falta un hueco de N+1 entradas
CONSECUTIVAS. Un sitio aqui y otro alla no vale, aunque sumen.

**Y borrar tiene que llevarse la cadena.** Si se borrara solo la entrada
corta, los trozos de delante se quedarian huerfanos, apuntando por su suma
a un nombre que ya no existe. Se reconocen porque van pegados y repiten la
suma; en cuanto una no cuadra, se para, porque lo de mas atras es de otro.

Un detalle que costo un rato: `cached()` guarda **un** sector, asi que hay
que leer la suma de comprobacion ANTES de empezar a recorrer hacia atras.
La primera vuelta del bucle se lleva por delante el sector que tenias.

## Que lo diga otro, otra vez

La comprobacion util no es que TinyOS lea lo que TinyOS escribe. Aqui
menos que nunca: escribir mal una cadena VFAT da un sistema de ficheros
que se entiende perfectamente consigo mismo y que nadie mas sabe leer.

```
    $ fsck_msdos -n /dev/rdisk5s2
    ** Phase 2 - Checking Directories        <- aqui se cazan las huerfanas
    (limpio)

    $ ls -1 /Volumes/DATA
    'carpeta larga'
    'otra vez muy largo.txt'
```

macOS lee los nombres que escribio TinyOS, y fsck no encuentra ni una
entrada de nombre largo suelta despues de crear, renombrar de largo a
corto, de corto a largo, mover a otro directorio y borrar.

Y se cae un aviso que llevaba desde el paso 35: ya no hay que explicarle a
nadie que los nombres nuevos tienen que caber en 8.3.

## Variables de entorno, y sacar el PATH del codigo

El `PATH` estaba escrito dentro del shell:

```c
    static const char *PATH[] = { 0, "/usr/bin" };
```

Cambiarlo queria decir **recompilar el sistema operativo**. Eso no es una
limitacion de un shell pequenyo: es que faltaba una idea.

**El entorno es un segundo array de cadenas, igual que argv.** La forma es
identica: `"NOMBRE=valor"`, terminado en cero. La diferencia no esta en la
forma, esta en como viaja:

| |argv|entorno|
|-|-|-|
|quien lo pone|quien te arranca, una vez|se hereda, sin que nadie lo reescriba|
|donde llega|`main(argc, argv)`|`environ`, una variable global|

Que el entorno acabe en una global y no en un parametro tiene su motivo:
casi nadie escribe `main(argc, argv, envp)`. Lo normal es pedirle una
variable suelta a `getenv()` desde cualquier sitio, y para eso tiene que
estar en un sitio fijo. Lo rellena **crt0**, antes de `main`, con lo que
dejo el kernel en `x2`:

```asm
    adrp    x9, environ
    add     x9, x9, :lo12:environ
    str     x2, [x9]
    bl      main
```

**Y alguien tiene que poner el primero.** Una herencia necesita un
antepasado. En un Unix de verdad lo pone `init` leyendo ficheros de
configuracion; aqui es una linea en `task_create_user_str`:

```c
    args_de_cadena(&e, "PATH=.:/usr/bin HOME=/ TERM=serie");
```

**export tiene que ser interna, por lo mismo que cd.** `setenv` cambia el
entorno del proceso que llama, y de nadie mas. Si `export` fuera un
programa, el shell se bifurcaria, el hijo cambiaria SU entorno y al morir
se lo llevaria consigo. El entorno se hereda hacia abajo, nunca hacia
arriba.

**Y `env` es un programa, tambien a proposito.** Siendo un programa,
demuestra lo que ensenya: si imprime `PATH` es porque el shell se bifurco,
el hijo heredo, hizo `exec`, y el kernel se lo volvio a entregar al
programa nuevo. Una orden interna no probaria nada.

```
    / $ export SALUDO=hola
    / $ env
      PATH=.:/usr/bin
      HOME=/
      TERM=serie
      SALUDO=hola          <- puesto por el shell, leido por su hijo
```

**La expansion, y por que ' y " no son lo mismo.** `$NOMBRE` se sustituye
en el mismo troceador que ya sabia de comillas, asi que no hay una cuarta
copia de nada. Y entre comillas **simples** no se expande:

```
    / $ echo 'sin expandir: $HOME'
    sin expandir: $HOME
    / $ echo "con comillas: $HOME"
    con comillas: /
```

Esa distincion parece caprichosa hasta que se dice en voz alta: una es
"esto tal cual" y la otra "esto, pero mirandolo".

**La prueba de que el PATH se usa de verdad** no es que `ls` funcione:
funcionaba antes. Es que deje de funcionar cuando no debe.

```
    / $ export PATH=/noexiste
    / $ ls
      LS.ELF: no lo encuentro. PATH=/noexiste
```

(Y un aviso sobre mirar los registros de una prueba: esa linea no aparecia
en mi primera lectura porque el filtro con el que estaba leyendo el log
descartaba todo lo que llevara `.ELF`. La prueba estaba bien; el que
miraba, no.)

## El kernel aprende a fallar y recuperarse

Este proyecto lleva cuarenta pasos convirtiendo fallos en mecanismos: la
pila que crece, copy-on-write, las paginas bajo demanda, la FPU perezosa,
`mmap`. Todos eran fallos de **otro** -de un proceso- que el kernel
atendia desde fuera.

Faltaba el propio. Cuando el kernel toca memoria de un proceso y ahi no
hay nada, hasta ahora se moria: *PANIC, excepcion no manejada*. Por eso
`user_range_ok` traia las paginas **por adelantado**, y por eso `exec` se
tragaba el ELF entero para leer una cabecera de sesenta y cuatro bytes.
Era pagar por miedo.

**Dos ideas, y las dos las pone el hardware.**

La primera es `ldtr` y `sttr`, *load/store unprivileged*: ejecutandose en
EL1, hacen el acceso con los permisos de **EL0**. Si la direccion no es
del proceso -porque apunta al kernel, o a nada- la MMU lo rechaza igual
que se lo rechazaria a el.

Eso cambia de sitio una responsabilidad. El kernel comprobaba a mano que
el puntero cayera dentro del espacio de usuario antes de tocarlo; ahora lo
comprueba el silicio, en cada acceso, sin que se pueda olvidar. **Un
kernel que valida punteros con ifs acaba teniendo un if que falta.**

La segunda es la tabla de arreglos. Si el acceso falla, el manejador busca
la direccion de la instruccion en una tabla y, si la encuentra, cambia el
punto de retorno:

```asm
1:  ldtrb   w3, [x1]            /* la que puede fallar */
    ...
3:  mov     x0, x2              /* la salida de error  */
    ret
    ARREGLO 1b, 3b              /* y el par, en .ex_table */
```

La construye el **enlazador**, no el codigo: no hay que registrar nada al
arrancar ni mantener una lista, las entradas aparecen porque alguien
escribio la instruccion. Es el `__ex_table` de Linux con dos entradas en
vez de miles.

**El orden del manejador es lo que lo hace util.** Ante un fallo del
kernel sobre una direccion de usuario, primero se INTENTA arreglar -la
pagina puede ser de un fichero mapeado que aun no se ha traido, o COW- y
solo si no hay manera se salta al arreglo. El primer caso es el que
permite que `exec` cargue un ELF mapeado sin traerselo entero; el segundo
es el que evita el panic.

Con eso, la comprobacion de `exec` pasa de materializar doce KB a mirar
que el rango este dentro del espacio de usuario. Lo demas se defiende
solo.

**Las cabeceras se copian antes de mirarlas**, y no solo por los fallos:
leerlas directamente de la memoria del proceso deja la puerta abierta a
que las cambie EN MEDIO, entre la comprobacion y el uso. Con cuatro
nucleos eso no es teorico. Se comprueba lo que se va a usar, y se usa lo
que se comprobo.

## Una funcion que servia a dos amos

El cambio rompio el arranque entero, y de la peor manera: once
`[kernel] no he podido crearlo` seguidos, sin decir por que.

`load_elf` sirve a dos amos y es facil no darse cuenta. Carga los
programas **empotrados en el kernel** -los que arranca el menu- y tambien
los que trae un proceso. Para los segundos hay que usar `copy_from_user`;
para los primeros eso no vale, porque son memoria del kernel, EL0 no
puede verla, y `ldtr` falla siempre.

Se distingue por la direccion, que es lo unico que hay y lo unico que no
se puede falsear: por encima de `KERNEL_VA_BASE` es nuestra.

Y de paso salio un fallo pequenyo que llevaba ahi desde el principio: el
bit que dice si un fallo fue de escritura es el 6 del ISS, y el codigo que
lo miraba habia enmascarado antes con `0x3F`. Un `0x3F` de mas y todas las
escrituras parecen lecturas.

## Que falle cuando debe

`user/malo.c` le da al kernel cuatro punteros que no valen:

```
    / $ malo
      exec con un puntero al kernel : -1
      exec con memoria sin mapear   : -1
      exec con un puntero nulo      : -1
      spawn con memoria sin mapear  : -1
      --- y sigo vivo para contarlo ---
```

Solo **dos** de esos cuatro llegan a la tabla: el puntero al kernel y el
nulo los para la comprobacion de rango antes de tocar nada. Los otros dos
-una direccion de usuario sin mapear- pasan el rango, fallan al leerse, y
es el arreglo el que devuelve el control.

Como saberlo con seguridad: quitando la tabla.

```
    / $ malo
      exec con un puntero al kernel : -1        <- el rango lo para

    ############## EXCEPCION ##############
      Causa   : EC=0x25  Data Abort en el mismo EL
      Direccion (FAR_EL1) : 0x000000000E000000
    *** PANIC: excepcion no manejada ***
```

Es la cuarta vez en este proyecto que hace falta romper algo a proposito
para saber si la prueba mide lo que dice. Sigue mereciendo la pena todas
las veces.

## init, o el kernel dejando de saber que es un shell

Hasta aqui el arranque estaba en un `switch` del menu del kernel: `f` para
el servidor de ficheros, `s` para la consola, `z` para el interprete. Y el
entorno inicial era una cadena literal dentro de `sched.c`. O sea que el
kernel sabia **que es un shell**, en que orden van los drivers y que
variables hereda un proceso. Nada de eso es asunto suyo.

Ahora el kernel arranca **uno** y se olvida:

```c
    int pid = task_bootstrap("init", &a, &e, DEV_NINGUNO);
    task_set_init_pid((uint64_t)pid);
```

Y el sistema arranca solo, sin tocar una tecla.

**El problema del huevo y la gallina.** El servidor de ficheros es un
programa, y para leer un programa de la tarjeta hace falta el servidor de
ficheros. Alguien tiene que traer los primeros dentro, y ese alguien es el
kernel: lleva empotrados el `conserver`, el `fs`, el `sh` y el propio
`init`. Es lo mismo que hace un initramfs, con cuatro entradas en vez de
un sistema de ficheros entero.

`init` los pide por nombre con `SYS_bootstrap`, y a partir del shell todo
lo demas se lee de `/usr/bin`.

**Pedir el dispositivo por nombre, no por direccion.** Los drivers
necesitan MMIO, y dejar que un proceso diga "mapeame la pagina
0x3F201000" seria regalar la maquina. Asi que dice `DEV_UART`, y el kernel
decide si eso significa algo y que direccion es:

```c
    case DEV_UART: return UART0_PHYS;
    case DEV_EMMC: return EMMC_PHYS;
    default:       return 0;
```

La lista de lo concedible esta en el kernel y no la elige quien pregunta.
Esa es toda la diferencia entre conceder y obedecer.

**Y una frontera de privilegio, una sola.** `SYS_bootstrap` y
`SYS_consola` solo las atiende el kernel si quien llama es init:

```c
    if (current->pid != task_init_pid()) { ret = -1; break; }
```

Es una linea, y conviene decir cuanto es: no hay usuarios, ni grupos, ni
capacidades. Hay **el primero** y hay los demas. Basta porque init es el
unico que puede existir antes de que exista nadie mas; en cuanto hiciera
falta un segundo proceso de confianza, esto se quedaria corto y habria que
inventar algo de verdad.

**El entorno sale de un fichero.** `/etc/rc` son lineas `NOMBRE=valor` que
init lee con `mmap` y mete en su entorno, de donde se heredan hacia abajo.
La prueba de que ya no esta en el codigo es una variable que el kernel no
ha visto nunca:

```
    / $ echo soy $SISTEMA
    soy TinyOS
```

Cambiar el `PATH` ha dejado de ser recompilar el sistema operativo.

**Y el interprete vuelve si se va.** `init` lo arranca en un bucle: cuando
escribes `salir`, sale otro. Eso es lo que hace que un shell que se muere
no deje la maquina muda, y es literalmente el motivo por el que init
existe en cualquier Unix.

## Dos fallos, y los dos de compartir

**Un puerto reservado que no estaba reservado.** `init` pide un puerto con
`port_create(-1)` -"el que sea"- y se llevo el **1**, que es el del
servidor de ficheros, porque lo pidio antes de que el servidor arrancara.
El servidor se encontro su sitio ocupado y se murio:

```
    [fs] servidor de ficheros vivo en EL0
    [fs] ya hay un servidor de ficheros: me voy
```

Es la **segunda vez** que pasa: en el paso 28 el kernel se quedo sin su
propio puerto por lo mismo. Entonces se parcheo saltando uno; ahora se
arregla de verdad, con `PORT_PRIMERO_LIBRE`. Un numero reservado que se
puede repartir por sorteo no es un numero reservado.

**Dos lectores para un teclado.** El menu del kernel seguia leyendo del
anillo de teclas mientras el shell leia tambien. No salia desordenado:
salia **repartido**. Escribir `env` daba `nvs` en el shell y una `e` en el
menu, cada uno convencido de haber leido bien:

```
    / $ slir
      SLIR.ELF: no lo encuentro. PATH=.:/usr/bin
```

Ahora el menu se aparta mientras la consola sea de otro. Es la diferencia
entre ser el camino y ser una herramienta: el menu sigue ahi para depurar,
pero manda quien tenga la consola.

## Una sola forma de tocar memoria de usuario

El paso anterior dejo el kernel con **dos** caminos para lo mismo:

```c
    user_readable(va, len) + leer el puntero a pelo   /* el viejo */
    copy_from_user(dst, va, len)                      /* el nuevo */
```

Los dos funcionaban. El problema de tener dos es que el viejo dependia de
que nadie se olvidara de llamarlo, y de que el rango que comprobaba fuera
**el mismo** que luego se leia. Dos sitios que tienen que decir lo mismo
acaban diciendo cosas distintas; es solo cuestion de cuantos pasos mas.

Ahora solo queda el segundo, en todas las llamadas: mensajes, rutas,
argumentos, `getcwd`, `realpath`, las teclas que entrega el driver de
consola, las tuberias y el marco de senyal. La comprobacion la hace la MMU
en cada acceso, y lo que falta llega por el camino del fallo de pagina.

**Y de paso se aclaro que eran `user_touch_r` y `user_touch_w`.** Parecian
verbos -"comprueba si puedo leer aqui"- y el codigo tenia que acordarse de
cual tocaba. Ya no los llama nadie para acceder: son lo que hay **al otro
lado del fallo**, y por eso ahora son uno solo con otro nombre:

```c
    int user_fault_fix(uint64_t va, int escritura);
```

Tres cosas arregla, en orden: un fichero mapeado que aun no se ha traido,
una pagina compartida por un `fork` que toca copiar antes de escribir, y
la pila que crece. No es un verbo que se llama: es el manejador.

**Un detalle que salio bien.** Copiar una cadena con tope se resuelve
copiando de golpe lo que quepa y buscando el cero dentro. Si el bloque se
corta antes -porque la cadena estaba al final de lo mapeado- lo copiado
sigue valiendo: solo hay que encontrar el cero ahi. Un bucle byte a byte
seria mas obvio y mucho mas lento, y la version rapida sale de que
`copy_from_user` diga **cuantos bytes faltaron** en vez de solo "fallo".

## Y fuera las demostraciones

`src/kernel.c` tenia 963 lineas. Ahora tiene 226.

Se han ido nueve hilos contando cosas, un menu de veinticinco teclas, dos
carreras de datos a proposito, un banco de memoria con y sin caches, y las
demostraciones de aliasing, de paginas de solo lectura y de desbordar la
pila del kernel.

Eso **fue el proyecto** durante mucho tiempo, y merece decirse: mientras
se aprendia como funciona cada pieza, la unica forma de ver una carrera de
datos era provocarla y la unica forma de ver que la MMU traduce era
imprimir la traduccion. Un menu de demostraciones era exactamente la
herramienta correcta.

Deja de serlo cuando hay un sistema. Ahora esas mismas cosas se hacen
desde dentro, con programas: `mem` mide el monton, `fp` la coma flotante,
`deep` desborda la pila, `malo` le da punteros falsos al kernel, `forkd`
mide lo que no cuesta un `fork`. Y se pueden encadenar, redirigir y
ejecutar desde un fichero, que es lo que no podia hacer un menu.

Lo que queda en el kernel es lo que no puede estar en otro sitio:
encender la UART, preguntar la RAM a la GPU, montar las tablas de
paginas, despertar los otros tres nucleos, enrutar los pines de la SD
-porque la pagina de GPIO lleva la UART al lado y darla entera seria
regalar la consola- y arrancar init.

```
    [init] soy el pid 5, y arranco el sistema
```

Pid 5 y no 14: los nueve hilos de demostracion ya no estan.

## Rutas largas y un reloj inventado

Los dos primeros peldanyos de una escalera que lleva a compilar el sistema
desde el propio sistema. Ninguno es espectacular; los dos hacian falta
antes de nada.

**Las rutas.** `FS_PATH_MAX` eran 64 bytes, y un arbol de fuentes tiene
rutas de 150 sin despeinarse. El problema no era el numero: era que la
ruta y el trozo de fichero salen **del mismo mensaje**, asi que alargar
una encogia el otro.

```
    mensaje = puerto(8) + posicion(8) + ruta + datos
```

Se sube el mensaje de 256 a 512 bytes, y entonces caben 256 de ruta y 240
de datos -que ademas hace las lecturas un 30% mas baratas-. Cuesta 16 KB
mas de monton del kernel en las colas de los puertos.

Que dos cosas sin relacion compitan por el mismo espacio es el sintoma de
un protocolo que lo mete todo en un mensaje de tamanyo fijo. Lo limpio
seria separarlas; mientras tanto, es un numero que se sube.

## Un reloj en una maquina que no tiene ninguno

La Pi **no tiene reloj de tiempo real**. No hay pila, no hay nada que siga
contando con la maquina apagada: al arrancar no sabe que dia es y no hay
forma de que lo averigue sola.

Y sin fechas no hay `make`. Comparar marcas de tiempo *es* lo que hace
make; sin ellas, o recompila todo siempre o hace cosas peores.

Lo que la maquina si sabe es **cuanto lleva encendida**. Asi que el reloj
es una suma:

```c
    hora = base + tiempo desde el arranque
```

y la base la pone alguien de fuera. Por defecto es la fecha en que se
compilo el kernel, inyectada por el Makefile. Es una mentira util: no es
la hora, pero cumple lo unico que hace falta -que un fichero escrito
despues tenga una marca mayor que uno escrito antes- y no queda por detras
de los fuentes, porque los fuentes los copio a la tarjeta esa misma
maquina. `fecha AAAA-MM-DD hh:mm` la corrige, y eso solo lo puede hacer
init: la hora es de la maquina entera.

**Un reloj que solo sabe que el tiempo avanza, no que hora es.**

**El formato de FAT cuenta su propia historia.** La fecha son dos palabras
de 16 bits:

```
    fecha: anyo desde 1980 (7 bits) | mes (4) | dia (5)
    hora:  hora (5) | minuto (6) | segundo PARTIDO POR DOS (5)
```

Los segundos van de dos en dos porque no cabian: con 5 bits llegas a 31,
no a 59. Se decidio que dos segundos de precision bastaban, y ahi sigue
cuarenta anyos despues. El anyo en 7 bits llega a 2107, que suena lejos y
sin embargo alguien lo vera.

**Y una trampa que solo se ve comparando con otro.** La primera version
puso el reloj en UTC, y salio esto:

```
    2026-09-23 13:57        82  HOLA.TXT      <- lo escribio el Mac
    2026-09-23 11:54       117  NUEVO.TXT     <- lo escribio TinyOS
```

Dos horas de menos. **FAT no tiene zona horaria**: guarda la hora del
sitio donde se escribio el fichero, y punto. Con el reloj en UTC, todo lo
que escribiera TinyOS apareceria mas viejo que lo que acababa de escribir
el Mac... y make, que compara fechas, haria justo lo contrario de lo que
se le pide.

Un reloj mal puesto no es un detalle cosmetico cuando alguien ordena cosas
con el. La epoca se inyecta ahora en hora local.

**Y la comprobacion de que sirve para lo que se hizo:**

```
    2026-09-23 13:57       117  UNO.TXT
    2026-09-23 13:58       117  DOS.TXT
```

Escritos con un minuto de diferencia, y el segundo es mas nuevo. Eso es
todo lo que make necesita saber. macOS lee las mismas fechas, y
`fsck_msdos` sigue limpia.

## Un ls de hace quince pasos

En la Pi, `ls` dentro de `/boot` daba esto:

```
    ÍÚ³j             9080 bytes
    ÍÚ³j             8784 bytes
```

Parecia un fallo de FAT16, porque en FAT32 iba bien. No lo era, y los
cuatro bytes lo decian todo: `0x6AB3DACD` al reves es un timestamp leido
como texto. Lo que se estaba ensenyando en el sitio del nombre **era la
fecha**.

Es decir: un `ls` que leia `struct fs_info` con el reparto de campos
ANTERIOR. Un binario viejo.

**Tres decisiones mias se juntaron para que eso pasara**, y ninguna era
obviamente mala por separado.

**1. Meti el campo nuevo en medio.** `mtime` quedaba bonito entre `flags`
y `name`. Puesto al final, un programa viejo habria seguido leyendo bien
todo lo que ya conocia y simplemente no habria visto la fecha. La regla es
vieja y la salte igual: **campos nuevos, al final; siempre.** No es
compatibilidad de verdad -para eso hace falta una version en el protocolo-
pero convierte "basura silenciosa" en "una cosa de menos".

**2. `make sd` solo copiaba.** Los ejecutables que vivian en la particion
de arranque antes del paso 35 se quedaron ahi para siempre, porque nadie
los borro nunca. Una herramienta de despliegue que no borra deja el
destino contando **la historia entera** en vez del estado actual. Ahora
quita los `.ELF` que no pintan nada ahi, y lo dice.

**3. El directorio actual iba primero en el PATH.** Y aqui esta lo bueno:
este mismo documento explicaba, dos pasos atras, por que Unix **no** pone
`.` en el PATH — entrar en un directorio ajeno y escribir `ls` podria
ejecutar el `ls` que haya dejado su duenyo. Y yo lo habia descartado
diciendo que aqui no hay varios usuarios, asi que no hay a quien enganyar.

Era cierto y daba igual. `cd /boot` seguido de `ls` ejecutaba el viejo. No
hacia falta un atacante: bastaba con una copia vieja.

Ahora `.` va el ultimo, y se anyade la otra mitad de la regla de Unix: **si
el nombre lleva una barra, no se busca en ningun sitio.** `./prog` y
`/usr/bin/prog` dicen exactamente donde estan, y ponerse a buscar seria
desobedecer.

## La superficie que espera una libc

Hasta aqui, un programa que quisiera borrar un fichero tenia que saber
COMO se habla con el servidor: crear un puerto, componer un
`struct fs_request`, mandar el mensaje, esperar la respuesta. Cincuenta
lineas para un `unlink`.

Eso esta bien mientras el que escribe el programa esta aprendiendo como
funciona un servidor de ficheros. Deja de estarlo en cuanto quieres portar
codigo que ya existe: **newlib no sabe nada de puertos**, sabe de
`unlink()`, `stat()` y `lseek()`.

El kernel ya hacia de intermediario para `open`, `read` y `write`. Ahora
tambien para `stat`, `lseek`, `unlink`, `mkdir`, `rmdir`, `rename`,
`opendir` y `readdir`. La IPC sigue ahi debajo, intacta; lo que cambia es
que ya no hay que conocerla.

Se nota al pesarlo:

| programa | antes | ahora |
|----------|-------|-------|
| `rm`     | 55    | 35    |
| `mkdir`  | 51    | 18    |
| `rmdir`  | 51    | 25    |
| `mv`     | 94    | 46    |
| `ls`     | 101   | 82    |

**Un directorio abierto es un descriptor con un indice dentro.** El
protocolo del servidor pide las entradas de una en una por numero, asi que
el descriptor solo tiene que acordarse de por cual iba. Que eso quepa en
la misma `struct fichero` que un fichero normal no es casualidad: *"lo que
un proceso tiene abierto"* es un concepto, y los tipos son variaciones
suyas.

**Y `lseek` solo vale en un fichero.** Una tuberia no se puede rebobinar
-los bytes ya no estan- y la consola tampoco. Devolver un error ahi no es
una carencia: es la verdad.

`struct estado` tiene tres campos: tamanyo, fecha y si es un directorio.
FAT no guarda duenyo, ni permisos, ni enlaces, e inventar campos que
siempre valen lo mismo seria fingir que este sistema tiene cosas que no
tiene.

## La pila del kernel se quedo pequenya

El paso anterior multiplico por cuatro el tamanyo de las rutas (64 a 256)
y por dos el de los mensajes (256 a 512). Las llamadas nuevas empezaron a
estrellarse:

```
    *** PANIC: desbordamiento de pila de kernel ***
```

Nadie habia escrito codigo mas profundo. Lo que paso es que **los datos
que maneja el kernel crecieron y la pila no**. Un marco con dos rutas y un
mensaje se come 1 KB largo, y hay tres anidados: el despachador, la
operacion y la transaccion con el servidor.

Se arreglo por dos sitios, y los dos hacian falta:

- **Menos sitio por marco.** El mensaje de `fs_transaccion` pasa a ser
  `static`, y es seguro porque ya lo era: `fs_mtx` garantiza una
  transaccion a la vez, asi que no hay con quien compartirlo mal. Las
  operaciones por nombre se unifican en una funcion -no por ahorrar
  lineas, sino porque el compilador puede reservar a la vez el sitio de
  todas las ramas de un `switch`-. El marco del despachador baja de 1168 a
  928 bytes.

- **Mas pila.** De una pagina a dos. Micro-optimizar marcos es pelear con
  el sintoma: si los datos son cuatro veces mas grandes, la pila que los
  maneja tiene que crecer. Cuesta 4 KB por hilo en una maquina de 960 MB.

**Y lo encontro la pagina de guarda**, que lleva ahi desde el paso 22 sin
hacer nada. Sin ella esto habria sido una escritura silenciosa encima de
la tarea de al lado, y el fallo habria aparecido mucho despues, en otro
sitio y sin relacion aparente. Una red que no se usa en veintitres pasos y
sirve una vez ya ha pagado su coste.

## Memoria anonima

`sbrk` mueve **un** tope: la memoria de un proceso es un bloque contiguo
que crece y encoge por arriba. Basta para un `malloc` pequenyo y se queda
corto en cuanto alguien quiere un arena grande y poder soltarlo entero sin
esperar a que se vacie lo que hay encima.

`mmap` anonimo da tramos independientes: cada uno se pide, se usa y se
suelta por su cuenta. Es como reserva memoria cualquier compilador, y por
eso hace falta.

Por dentro es el mismo mecanismo que mapear un fichero, con la pagina
saliendo del gestor de paginas en vez del servidor. Y eso lo hace **mucho
mas barato**: no hay mensaje, no hay espera, no hay nadie de quien
depender.

```
    / $ map -m
      100 MB reservados en 0x30000000
      paginas libres: 245423 antes -> 245423 despues de reservar
      (reservar no gasta memoria: no se ha tocado nada)

      escribo en 256 paginas repartidas -> quedan 245117
      gastadas: 306 paginas para 256 tocadas
      lo escrito se relee y el resto esta a cero: ok
```

Reservar cien megas cuesta **cero paginas**. Tocar 256 repartidas cuesta
306: las 50 de mas son tablas de nivel 3, una por cada 2 MB de direcciones
tocadas. Ese es el precio de escribir salteado en vez de seguido, y se ve
aqui porque las paginas se cuentan.

**Y un cliff que habria aparecido tarde.** La primera version ponia cada
tramo detras del ultimo y nunca miraba atras. Mas simple, y funciona
hasta que alguien reserva y suelta muchas veces: cada vuelta consume
direcciones que ya no vuelven, y a los 128 MB de la zona se acaba el sitio
**aunque no haya nada mapeado**. Un compilador hace exactamente eso miles
de veces.

Ahora se busca el primer hueco que valga, y la prueba lo comprueba:

```
      veinte vueltas de reservar y soltar:
        siempre en 0x30000000: ok
```

## El ultimo programa sin IPC

Con `cat`, `cp` y `write` pasados a `open`/`read`/`write`, ya no queda
ningun programa que sepa componer un `fs_request`. Los que hablan por
puerto son los que tienen que hacerlo: el servidor de ficheros, el de
consola y sus clientes.

| programa | antes | ahora |
|----------|-------|-------|
| `cat`    | 60    | 43    |
| `cp`     | 94    | 51    |
| `write`  | 89    | 38    |

Y `cat` gana algo que antes no podia tener: **funciona con lo que sea que
haya detras del descriptor**. Si le dan una tuberia en vez de un fichero,
no se entera. Eso no es que se haya anyadido: es lo que aparece cuando
dejas de hablar con un servidor concreto y empiezas a hablar con un
descriptor.

## Una funcion que devuelve dos veces

`setjmp` es lo mas raro que hay en una libc: devuelve **dos veces**. La
primera cuando la llamas, con cero; la segunda cuando alguien hace
`longjmp`, con lo que le pasaran.

No es magia. Se guarda los registros que el ABI obliga a conservar entre
llamadas -x19-x28, el marco, la direccion de retorno, el puntero de pila y
d8-d15- y `longjmp` los vuelve a poner. Al restaurar x30 y sp, el `ret` de
`longjmp` **aterriza dentro de setjmp**, justo detras de su propio `ret`, y
setjmp vuelve por segunda vez.

Y por eso no se puede escribir en C: en C no hay forma de decir *"el
puntero de pila vale esto otro"*.

Es la misma lista que salva `switch.S` al cambiar de hilo, mas los ocho
registros de coma flotante. No es casualidad: las dos cosas son lo mismo
-congelar un punto de ejecucion para volver a el- con la diferencia de que
un cambio de contexto vuelve **una** vez y esto puede volver muchas.

**Una linea que parece un capricho del estandar.** `longjmp(buf, 0)` tiene
que hacer que setjmp devuelva **1**, no 0:

```asm
    cmp     w1, #0
    csinc   w0, w1, wzr, ne
```

Quitandola, el programa de prueba entra en un bucle infinito:

```
    primera vez: setjmp devuelve 0
    tras el salto: setjmp devuelve 7, desde tres niveles
    primera vez: setjmp devuelve 0          <- otra vez
    tras el salto: setjmp devuelve 7...
```

Si `longjmp(buf, 0)` devolviera cero, el que llamo a setjmp no podria
distinguir la vuelta de verdad de la primera, y un bucle de reintentos no
terminaria nunca. El estandar lo exige por eso.

**Y los d8-d15**, que en este sistema tienen gracia: guardarlos toca la
FPU, y la FPU esta apagada hasta que alguien la usa (paso 30). O sea que
el primer `setjmp` de un proceso provoca una excepcion, le reserva sus 528
bytes y sigue. Cuesta una trampa y es correcto.

## Una prueba que no medía nada

La primera version de la prueba de `d8-d15` era esta:

```c
    volatile double antes = 0.0;
    ...
    antes = sumar(1.0, 8);
```

Y pasaba. Tambien pasaba **quitando el guardado de d8-d15 de setjmp**, o
sea que no medía nada.

La culpa es de ese `volatile`. Se puso por un motivo correcto -tras un
longjmp, lo que no este en memoria vuelve atras- y tiene un efecto
secundario que arruina esta prueba en concreto: obliga a la variable a
vivir **en memoria**, que es justo donde no le pasa nada a un registro que
no se guarda.

En C no hay forma de decir "esto tiene que vivir en d8". Asi que se dice
en ensamblador:

```c
    static inline void poner_d8(double v) { asm volatile("fmov d8, %d0" :: "w"(v)); }
```

Se pone 3.25 antes de `setjmp`, se cambia a 99.5 despues, se salta, y se
mira. Con el guardado: `3.25 ok, restaurado`. Sin el: `0.00 MAL`.

Es la quinta vez en este proyecto que romper el codigo a proposito cambia
lo que sabiamos, y la primera en que descubre que **la prueba** estaba mal
en vez del codigo.

## Lo demas que ha crecido

`qsort` con mediana de tres, y no por elegancia: el quicksort de libro
-pivote el primero- se vuelve **cuadratico** justo con lo que mas aparece
en la vida real, que son datos ya ordenados o casi. Mirar tres y quedarse
con el de en medio cuesta dos comparaciones y quita ese caso. Por debajo
de ocho elementos usa insercion, que es O(n²) y **mas rapida** ahi: sin
ceremonia de recursion y con los datos en cache.

`bsearch` con `bajo + (alto-bajo)/2` y no `(bajo+alto)/2`, porque la suma
se desborda con arrays enormes. Es el fallo que estuvo veinte anyos en la
busqueda binaria de la biblioteca de Java.

`strtol` es lo que `atoi` deberia haber sido: dice donde se paro, entiende
bases, y permite saber **si leyo algo**. `atoi` no puede distinguir `"0"`
de `"hola"`, y por eso sigue habiendo programas que tratan una entrada
mala como un cero.

`calloc` comprueba la multiplicacion antes de hacerla: `calloc(2, enorme)`
tiene que negarse, no dar un bloque de dos bytes al que desborda.
`realloc` devuelve la direccion nueva porque puede mover el bloque, y si
no puede, **el viejo sigue valiendo** -perderlo ahi es un fallo clasico-.

## errno, o hacer que el kernel diga por que

Hasta aqui, **todo** fallo era `-1`. "No existe", "es un directorio", "ya
existe", "no cabe" y "ese descriptor no es tuyo" se contaban igual, y
quien preguntaba tenia que adivinar.

Es exactamente la leccion del paso 38 -cuatro errores donde habia uno- un
nivel mas arriba: entonces fue el protocolo del servidor, ahora las
llamadas al sistema. La informacion **ya existia** y se perdia por el
camino: el servidor distinguia cuatro casos desde hace diez pasos, y el
kernel los aplastaba todos en un `-1`.

**El truco de devolver el error en el valor.** Una llamada devuelve UN
numero, y el kernel no tiene donde poner un segundo que sea del que llama:
escribir en su memoria exige un puntero que quiza no ha dado. Asi que se
aprovecha que ningun resultado legitimo -un tamanyo, un descriptor, una
posicion- es negativo, y en el rango de -4095 a -1 se meten los codigos.

La libc lo deshace:

```c
    static inline int64_t revisar(int64_t r)
    {
        if (r < 0 && r >= -4095) { errno = (int)-r; return -1; }
        return r;
    }
```

O sea que `errno`, que hoy suena a error de disenyo -informar de un fallo
escribiendo en una variable global-, **no es como se transporta el error**
en este sistema: es como se le entrega a un programa que espera un Unix.
El transporte va dentro del valor, que es donde tiene que ir.

## Siete mensajes donde habia uno

```
    / $ rm docs
      docs es un directorio: usa rmdir
    / $ rm noexiste.txt
      noexiste.txt: no existe
    / $ rmdir docs
      no he podido borrar docs: el directorio tiene cosas dentro
    / $ rmdir hola.txt
      no he podido borrar hola.txt: no es un directorio
    / $ mkdir docs
      no he podido crear docs: ya existe
    / $ ls noexiste
      /noexiste: no existe
    / $ ls hola.txt
      /hola.txt: no es un directorio
```

Y dos de esos siete costaron una transaccion de mas, porque el servidor no
los distinguia:

- `mkdir` sobre algo que ya esta le sale como "no pude". Se mira si existe,
  y entonces es `EEXIST`.
- `rmdir` sobre un FICHERO le sale como "no esta", porque busca solo entre
  los directorios. Y eso es falso: esta, lo que pasa es que no es un
  directorio.

Las dos comprobaciones estan **en el camino del error**, que es justo donde
da igual lo que cuesten.

**Y una que se cae sola.** `rm` preguntaba dos veces -un `stat` para ver si
era un directorio y luego el `unlink`- porque el fallo no decia por que.
Eso no era solo feo: entre las dos preguntas el fichero podia cambiar.
Ahora es una llamada y el motivo viene con ella.

**Un codigo que merece existir por si solo:** `ESPIPE`. Rebobinar una
tuberia no es un argumento invalido, es una operacion que no tiene sentido
sobre esa cosa: los bytes ya no estan. Que haya un codigo aparte para eso
-y no un `EINVAL` generico- es una decision de hace cincuenta anyos que
sigue siendo util.

## Un cubo delante de cada fichero

`printf` no escribia en la pantalla: escribia en un `write()`, y cada
`write()` es una excepcion, un cambio de privilegio, un viaje por la tabla
de vectores y la vuelta. Dos mil caracteres eran dos mil viajes.

```
    / $ libc
      --- el cubo ---
      stdout habla con un terminal
      2000 caracteres con cubo:  4 llamadas a write()
      2000 caracteres sin cubo:  2000 llamadas a write()
      o sea 500 veces menos viajes al kernel
```

Un `FILE` es un descriptor con un cubo delante. Eso es todo lo que es. Lo
que tiene miga es **cuando** se vacia, y de esa decision salen tres cosas
que parecen no tener nada que ver entre si.

**Primera: el mismo binario se comporta distinto segun a donde escriba.**

```
    / $ libc
      stdout habla con un terminal
    / $ libc > /sal.txt
    / $ cat /sal.txt
      stdout habla con un fichero
```

Con un terminal delante hay alguien **mirando**, y una salida que aparece a
trozos de 512 bytes no sirve para seguir un programa: se vacia en cada
salto de linea. Hacia un fichero no mira nadie mientras se escribe, asi que
se llena el cubo entero. El criterio no es "que es mas rapido", es "que le
sirve a quien lee".

Y la pregunta se responde sin syscall nueva: se intenta `lseek` sobre el
descriptor. Un fichero te deja moverte; la consola no, porque no tiene
posicion. La respuesta llega **por el camino de atras**, como efecto
secundario de una operacion que iba a otra cosa. (`errno` se guarda y se
repone: esto es una pregunta, no un fallo.)

**Segunda: la mentira mas vieja de depurar con printf.**

```
    / $ libc
      --- lo que se pierde ---
      salgo por exit()  y esto se lee
      arriba hay UNA linea, no dos
```

Dos hijos escriben lo mismo, sin salto de linea al final. Uno sale por
`exit()`, que vacia los cubos; el otro por `_exit()`, que es lo que pasa de
verdad cuando un programa se muere de golpe. Solo se lee uno.

Por eso **el ultimo `printf` que ves en pantalla no es el ultimo que se
ejecuto**. Llevas cuarenta anyos de programadores buscando el fallo entre
la linea que se imprimio y la siguiente, cuando el fallo estaba tres
funciones mas adelante y lo que falta es lo que se quedo en el cubo.

Quitar el `fflush(0)` de `exit()` hace desaparecer tambien la primera
linea. La prueba mide algo.

**Tercera: dos colas para la misma puerta.**

`cat` imprimia sus cabeceras con `printf` y el contenido con `write(1,...)`
a pelo. Funcionaba, y funcionaba **de milagro**: mientras stdout hablaba con
la consola se vaciaba en cada salto de linea y el orden cuadraba por
casualidad. En cuanto la salida fue a un fichero:

```
      ...contenido del fichero...
      --- /sal.txt ---      <-- la cabecera, al final
```

Las cabeceras se quedaron en el cubo hasta el `exit` y salieron **detras**
del contenido que anunciaban. Mezclar la libc y el descriptor a pelo en la
misma salida es tener dos colas para la misma puerta.

### Leer del terminal sin cubo, y por que

Al leer, el cubo tiene un problema que no tiene al escribir: **el terminal
no es tuyo**. Lo comparten todos los procesos por turnos. Si el shell se
guarda 512 bytes "por si acaso", se esta quedando con lo que el usuario
tecleo para el programa que viene despues, y ese programa espera algo que
ya no va a llegar.

De un fichero se lee de golpe porque el fichero es tuyo. Del terminal, de
uno en uno.

### Atar stdin a stdout

Un programa que pregunta `nombre: ` sin salto de linea se quedaba mudo:
la pregunta en el cubo, el programa esperando la respuesta. Le paso al
shell en cuanto printf empezo a tener cubo -el prompt aparecia **un
comando tarde**- y le habria pasado a todo lo que pregunte algo.

La solucion no es acordarse de poner `fflush(stdout)` antes de cada
lectura. Es que **leer vacie lo que haya pendiente de escribir**, una vez,
en `fgetc`. Por eso en C nadie escribe un `fflush` antes de un `scanf`: no
es que no haga falta, es que ya lo hace la lectura.

### Lo que hay

`fopen` `fclose` `fflush` `fread` `fwrite` `fgetc` `fputc` `ungetc`
`fgets` `fputs` `fseek` `ftell` `rewind` `feof` `ferror` `clearerr`
`fprintf` `vfprintf` `isatty`, y `printf`/`puts`/`putchar`/`getchar`
reescritos encima de ellos para que haya **un solo cubo por fichero** y el
orden de lo que sale sea el orden en que se escribio.

Dos detalles que no son descuidos:

- **Solo `"r"` o `"w"`,** no `"r+"`. Un fichero abierto para las dos cosas
  necesita saber en que direccion se uso el cubo la ultima vez y vaciarlo
  al cambiar de sentido; es la parte de stdio donde mas se equivoca todo
  el mundo, y aqui no hace falta.
- **`stderr` sin cubo,** a proposito. Es para lo que tienes que ver aunque
  el programa se este cayendo.

Y uno que si es un error facil: `ftell` **no** es la posicion del
descriptor. Hay que descontar lo que queda por consumir en el cubo al leer,
o sumar lo que queda por escribir. Sin eso, un `ftell` tras leer un solo
caracter contesta 512.

## Anyadir al final, o quien decide donde acaba un fichero

Anyadir al final de un fichero parecen dos lineas: averiguas donde acaba y
escribes ahi.

    long fin = lseek(fd, 0, DESDE_FINAL);
    lseek(fd, fin, DESDE_INICIO);
    write(fd, linea, n);

Eso funciona perfectamente, y sigue funcionando mientras seas el unico. En
cuanto hay otro escribiendo en el mismo fichero, el numero que devolvio el
primer `lseek` describe un fichero que ya no existe: entre averiguarlo y
usarlo, el final se ha movido. Los dos escriben en el mismo sitio, el
segundo tapa al primero, y nadie se entera de nada porque las dos
escrituras devolvieron el numero de bytes que se les pidio.

**Preguntar y actuar son dos cosas, y entre dos cosas siempre cabe una
tercera.** No hay forma de arreglarlo escribiendo mejor las dos lineas. La
unica salida es que dejen de ser dos.

### Donde tiene que vivir la indivisibilidad

Que sean una obliga a decidir quien las hace, y ahi esta lo interesante:
no puede ser quien escribe. El que escribe es un cliente, y lo unico que
sabe del final del fichero es lo que le hayan contestado hace un rato.

El final de un fichero esta en su entrada de directorio, y la entrada de
directorio es del servidor. Asi que la decision baja ahi, y el cliente
deja de tomarla: manda un desplazamiento que no es un desplazamiento.

    #define FS_AL_FINAL   0xFFFFFFFFul

`FS_AL_FINAL` en el `arg` de un `FS_WRITE` no quiere decir "escribe en el
byte 4294967295". Quiere decir *donde acabe, y dime donde fue*.

En el servidor el cambio son dos lineas dentro de `fichero_escribir`, y lo
que importa de ellas es donde estan colocadas:

    dir_read(dlba, doff, &primero, &tam);

    if (offset == (uint32_t)FS_AL_FINAL) offset = tam;
    if (offset > tam) return -1;

El tamanyo se lee y se usa sin soltar el control en medio. **Y no hay
ningun cerrojo, ni hace falta ninguno:** el servidor atiende un mensaje
entero antes de mirar el siguiente, asi que dentro de una peticion no hay
nadie mas. La indivisibilidad no se ha construido, se ha *colocado* donde
ya estaba.

Esa es la leccion del paso, y no es de sistemas de ficheros: es de
microkernels. Un servidor con estado puede regalar garantias que un
cliente no puede fabricar por mucho cuidado que ponga, porque las
garantias no salen del cuidado, salen de quien es duenyo del dato.

### Una peticion que decide algo tiene que contar que decidio

Con `FS_AL_FINAL` el cliente manda la escritura sin saber donde va a caer,
y despues tampoco lo sabe. Eso no vale: un descriptor tiene que poder
decir por donde va, porque `ftell` pregunta. Asi que la respuesta a
`FS_WRITE`, que hasta ahora era un `FS_OK` pelado, lleva algo dentro:

    struct fs_escrito {
        unsigned long off;               /* primer byte que se escribio */
    };

Y el kernel la usa para poner el descriptor al dia:

    uint64_t donde = f->anyadir ? FS_AL_FINAL : f->off;
    ...
    if (f->anyadir) {
        struct fs_escrito *e = (struct fs_escrito *)resp.data;
        f->off = e->off + hay;
    } else {
        f->off += hay;
    }

### O_ANYADIR no es una posicion, es una propiedad

`O_ANYADIR` no quiere decir "abrelo y ponme al final". Eso lo puede hacer
el programa solo con un `lseek`, y es justo lo que no sirve. Es una
propiedad del **descriptor**: mientras este abierto asi, *cada* escritura
se coloca al final en el momento de escribir, y el desplazamiento que
guarda la estructura deja de mandar.

De ahi sale un detalle que parece un descuido y es lo contrario: un
`lseek` sobre un descriptor abierto para anyadir mueve el numero, pero no
mueve donde se escribe. La siguiente escritura seguira yendo al final. Lo
dice POSIX, y es la unica manera de que la garantia siga en pie: si un
`lseek` pudiera desactivarla, no seria una garantia, seria una costumbre.

Tres aperturas, tres contratos con lo que ya hubiera:

| modo         | si no existe | si existe      | es       |
|--------------|--------------|----------------|----------|
| `O_LEER`     | falla        | lo lee         | abrir    |
| `O_ESCRIBIR` | lo crea      | **lo vacia**   | `>`      |
| `O_ANYADIR`  | lo crea      | **no lo toca** | `>>`     |

La diferencia entre los dos ultimos es toda la diferencia entre "esto
sustituye a lo que habia" y "esto se suma a lo que habia", que es la que
separa un fichero de salida de un diario.

En la libc es la tercera letra de `fopen`, y en el shell es una ficha mas
en el troceador. `>>` tiene que mirarse **antes** que `>` o saldrian dos
fichas seguidas y la segunda se comeria el nombre del fichero; en un
lenguaje de verdad la regla se llama *maximal munch* y aqui son cuatro
lineas.

## El otro problema, que se parece y no es el mismo

Hay una segunda forma de que dos escrituras se pisen, y tiene una
respuesta completamente distinta. Merece la pena verlas juntas porque
confundirlas es facil.

Un `write` de 500 bytes no es un mensaje: son tres, porque en uno solo
caben 240 utiles. Y entre mensaje y mensaje, quien **comparta ese mismo
descriptor** -un hijo de un `fork`, el otro extremo de un `dup2`- puede
colar los suyos. Aqui el desplazamiento es comun, asi que no se pierde
nada; lo que sale son dos lineas trenzadas, que en un fichero de texto es
lo mismo que haberlas perdido.

La respuesta es un cerrojo, y lo unico que hay que acertar es donde
ponerlo:

    struct fichero {
        ...
        uint64_t     off;
        int          anyadir;
        struct mutex mtx;      /* una escritura entera, indivisible */
    };

No es del kernel, ni del proceso, ni de la tarjeta: es de **la descripcion
de fichero abierta**, porque lo que protege es ese `off` de ahi arriba. El
cerrojo vive donde vive el dato que defiende. Y por eso `fork` lo comparte
sin hacer nada especial: comparte la estructura entera, que es exactamente
lo que Unix llama *open file description* y exactamente lo que hay que
compartir.

Lo que **no** arregla ese cerrojo es el caso de antes: dos procesos que
abren el fichero cada uno por su lado tienen dos descripciones distintas y
dos cerrojos distintos, y un cerrojo que no comparten no sincroniza nada.
Para ese caso esta `O_ANYADIR`, que no necesita cerrojo ninguno porque no
hay nada compartido que proteger.

**Dos problemas que se parecen y tienen respuestas que no se parecen en
nada.** Uno es "esto lo comparten dos, hay que serializarlo" y el otro es
"esto no lo comparte nadie, hay que bajarlo a quien sabe la respuesta".

## Una prueba que falla cuando debe, otra vez

`anyadir` hace las dos mitades, y la segunda es la que hace util a la
primera. Tres hijos escriben 20 lineas de 16 bytes cada uno en el mismo
fichero: 960 bytes si no se pierde nada. Las lineas llevan dentro la letra
del hijo repetida, asi que una linea con dos letras distintas es una
escritura partida por la mitad y se ve sin contar nada.

    con O_ANYADIR (cada hijo abre el suyo):
      esperados 960 bytes, hay 960
      por hijo: 20 20 20  (tendrian que ser 20 cada uno)
      ok: no se ha perdido nada
    con lseek al final y luego write:
      esperados 960 bytes, hay 656
      por hijo: 13 14 14  (tendrian que ser 20 cada uno)
      y eso es lo que tenia que pasar: se han perdido lineas

19 lineas de 60 desaparecidas, y ningun `write` devolvio un error.

La contraprueba lleva un `sleep(1)` entre el `lseek` y el `write`, y eso
hay que decirlo: **no fabrica el fallo, ensancha su ventana.** Sin el, el
fallo sigue estando y aparece cuando le apetece, que en una prueba es peor
que no aparecer, porque convierte un error en un misterio.

### Lo mismo en la Pi, que no da lo mismo

|                        | QEMU        | Pi 3B       |
|------------------------|-------------|-------------|
| `O_ANYADIR`            | 960 bytes, 20 20 20 | **960 bytes, 20 20 20** |
| `lseek` + `write`      | 656 bytes, 13 14 14 | **848 bytes, 18 17 18** |

La primera fila es identica, y tiene que serlo: colocar la escritura al
final lo resuelve el servidor dentro de la peticion, y eso no depende de
lo que tarde nadie.

La segunda no. En la Pi se pierden **7 lineas de 60** donde en QEMU se
perdian 19, con la misma ventana de 10 ms. El motivo es que ahi las
escrituras cuestan de verdad -traerse el sector de una tarjeta lenta,
tocarlo, devolverlo, y la FAT ademas- asi que cada proceso pasa mucho mas
tiempo DENTRO de la escritura y quedan menos ocasiones de colarse entre el
`lseek` y el `write`.

Sigue fallando, que es lo que tiene que hacer, pero con menos margen del
que parecia. Y eso es justo la forma en que una prueba deja de medir sin
avisar: si la ventana se estrechara un poco mas -un tick mas corto, una
tarjeta distinta- esta contraprueba empezaria a pasar, y parecerian buenas
noticias. No lo serian: el `lseek` seguiria estando igual de mal, y solo
habriamos dejado de verlo.

Y fijate en lo que *no* es el problema en esa segunda mitad: los hijos
comparten un descriptor heredado, y si escribieran sin el `lseek` saldria
bien, porque compartirian el desplazamiento. Es la pregunta -el `lseek`,
que habiamos puesto para ir sobre seguro- lo que parte la escritura en
dos.

## Tres sitios donde estaba escrita la misma lista

La lista de programas estaba en el `Makefile` tres veces: `UPROGS`, y a
mano otras dos dentro de `sdtest` y de `sdcard`. Estaba anotada como
limitacion desde hacia pasos, con un "ya se han desincronizado una vez".
Se habian desincronizado dos: `malo` aparecia dos veces en las dos copias
e `init` en ninguna.

Ahora la lista esta una vez y la otra se deduce:

    BINPROGS := $(filter-out init sh fs conserver client,$(UPROGS))

Los cinco que se quitan son los que el kernel lleva dentro, asi que no
tienen nada que hacer en la tarjeta. Lo que hacia peligroso el duplicado
no era el trabajo de escribirlo tres veces: es que olvidarse de uno **no
da ningun error**. El programa compila, el sistema arranca, y lo unico que
pasa es que ese programa no esta en la tarjeta y el shell dice que no
existe, que es lo mismo que dice cuando te equivocas al teclear.

## El trabajo, y no el proceso

Hasta este paso, Ctrl-C se repartia asi:

    uint64_t destino = consola_pid;                    /* el shell */
    struct task *t = by_pid(destino);
    if (t && t->waiting_for) destino = t->waiting_for; /* ...o su hijo */
    task_signal(destino, SIGINT);

Una cadena de un solo eslabon, y el comentario que tenia encima ya decia
lo que le faltaba: *"un Unix de verdad lleva grupos de procesos; esto es
la misma idea sin la contabilidad"*. Resulta que la contabilidad era la
idea.

Falla en los dos casos que importan. En `a | b` el shell espera primero a
`a`, asi que `waiting_for` vale `a` y `b` no se entera de nada. Y a un
nieto -cualquier programa que se bifurque- no llega nunca, porque la
cadena tiene un eslabon y hacen falta dos.

Esto no es un razonamiento, es lo que hace el kernel del paso 50 con el
mismo `lento 20 a | lento 20 b`:

    [a] 7 de 20
    [b] 7 de 20
    ^C
    [kernel] lento termina por la senyal 2
    [b] 8 de 20
    [b] 9 de 20
    ...
    [b] 20 de 20
    [b] terminado

`b` se queda ahi contando tan tranquilo mientras tu ya has pulsado
Ctrl-C, has recuperado el prompt y crees que has parado lo que pediste.

**Los dos fallos son el mismo, y no se arreglan haciendo la cadena mas
larga.** Se estaba buscando UN proceso cuando lo que el usuario quiere
parar es un TRABAJO. Lo que una persona escribe en una linea no es un
proceso: `cat x | wc` son dos procesos y una sola cosa.

### Un grupo es un nombre para "esto de aqui"

Un grupo de procesos es exactamente eso, y su nombre es el pid del
primero que lo formo. En la tabla de tareas es un campo:

    uint64_t pgid;

Se **hereda** en el `fork` y **sobrevive** al `exec`, igual que el
directorio actual: el programa cambia, pero de que trabajo forma parte
no. Por eso alcanza a un nieto sin que nadie lleve un arbol de
parentescos: el nieto nacio dentro del grupo y ahi sigue.

Y el reparto del Ctrl-C deja de seguir una cadena y pasa a preguntar
quien pertenece al grupo de primer plano:

    if (grupo)
        for (int i = CORES; i < MAX_TASKS && n < MAX_TASKS; i++)
            if (... && tasks[i].pgid == grupo)
                destinos[n++] = tasks[i].pid;

    sched_unlock_irqrestore(flags);
    for (int i = 0; i < n; i++) task_signal(destinos[i], SIGINT);

Dos pasadas y no una: primero se apuntan los pid con el cerrojo cogido y
luego se senyalan sin el, porque `task_signal` lo vuelve a pedir.
Recorrer la tabla llamandolo desde dentro seria un interbloqueo contra
uno mismo.

### El testigo de la consola

Poner un grupo en primer plano era "solo init". Eso es una frontera de
privilegio, pero no la correcta: quien tiene que ceder la consola al
trabajo que acaba de arrancar es el shell, y el shell no es init.

La regla nueva no habla de quien eres sino de que llevas, como el testigo
de una carrera de relevos:

1. **init siempre**, porque es quien la reparte cuando no queda nadie.
2. **Si tu grupo la tiene ahora**, puedes pasarla.
3. **Si la tiene un grupo que formaron hijos tuyos**, puedes recuperarla.
   Es lo que hace el shell cuando el trabajo que puso delante termina: se
   la habia prestado.
4. **Si no la tiene nadie vivo**, que se la quede quien la pida.

Lo que la regla impide es lo que tiene que impedir: que un proceso de
segundo plano se ponga delante por su cuenta y se quede con el teclado de
quien esta sentado ahi.

### La carrera del setpgid, y por que se escribe dos veces

En el shell, `setpgid` aparece dos veces por cada hijo: una en el hijo
antes del `exec`, y otra en el padre justo despues del `fork`.

    int64_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);              /* el hijo se coloca */
        ...
        exec(...);
    }
    setpgid((uint64_t)pid, (uint64_t)pid);   /* y el padre lo coloca */

Parece lo mismo escrito dos veces y no lo es: **son dos carreras
distintas, y cada linea tapa una.**

- Si solo lo hiciera el hijo, el padre podria ceder la consola a ese
  grupo antes de que el hijo se hubiera colocado, y el Ctrl-C iria a un
  grupo vacio.
- Si solo lo hiciera el padre, el hijo podria llegar al `exec` -y hasta
  terminar- antes de que el padre lo moviera, y `setpgid` sobre alguien
  que ya no esta devuelve `-ESRCH`.

Escrito en los dos sitios, gane quien gane la carrera el resultado es el
mismo. Es de las pocas veces en que repetir una llamada es lo correcto y
no un descuido, y esta en el manual de POSIX por esto exactamente.

## Segundo plano

Con los grupos puestos, el `&` sale casi de balde: el trabajo se monta
igual, pero no se le cede la consola y no se le espera. De ahi salen las
dos propiedades que uno espera de un proceso de fondo, y las dos son la
misma decision vista de dos lados: **no recibe el Ctrl-C porque no esta
en primer plano, y no te bloquea porque no lo esperas.**

El `&` se mira ANTES de partir la linea por la tuberia, porque
`a | b &` manda al fondo el trabajo entero. El `&` no es de una orden, es
de la linea.

### Un hijo al que nadie espera

Lo que no sale de balde es recogerlo. Un hijo cuyo padre nunca lo espera
se queda de zombi hasta que el padre muere, y **un shell no muere nunca**.

Para eso hace falta preguntar sin quedarse esperando, que es
`WNOHANG`, y una forma de decir "todavia no" que no se confunda con un
codigo de salida. Con el convenio del paso 48 eso ya estaba resuelto:

    if (t->state != TASK_ZOMBIE && (banderas & WNOHANG)) {
        ret = -EAGAIN;                   /* sigue vivo; vuelve luego */
        break;
    }

El shell pregunta justo antes de cada prompt, que es el unico momento en
que no esta haciendo otra cosa:

    / $ lento 5 fondo &
      [1] 11
    / $ jobs
      [1] 11  lento 5 fondo
    / $
      [1] hecho    lento 5 fondo

Un Unix de verdad se entera en el acto, por `SIGCHLD`. Esto se entera un
poco tarde, y para lo que hay que ensenyar da igual.

## Dos cosas que devolvian el mismo -1

Poner el Ctrl-C donde tiene que estar destapo un fallo que llevaba ahi
desde siempre y que no podia verse antes.

Ahora la senyal va al grupo de primer plano, y mientras escribes el grupo
de primer plano es el del shell: **el Ctrl-C te lo comes tu**. Asi que el
shell tiene que atraparlo, y atraparlo es facil -un manejador vacio, que
es lo que hace cualquier shell-. Pero al atraparlo pasaba esto:

    / $ hol^C
      se acabo la entrada, me voy

La senyal no lo mataba: **lo convencia de irse.** El manejador hacia su
trabajo, pero la lectura volvia con -1, y -1 era a la vez "una senyal
corto esto" y "se acabo la entrada". El shell leia lo segundo y se
despedia educadamente.

Que dos cosas distintas devuelvan el mismo numero no da guerra hasta que
una de las dos empieza a pasar de verdad. La leccion del paso 48 -que el
kernel diga POR QUE- se habia quedado corta justo en este camino, porque
hasta hoy nadie preguntaba:

    int c = uart_getc_blocking();
    if (c < 0) return -EINTR;               /* antes: return -1 */

Y de paso aparecio que `read` y `write` eran **las dos unicas llamadas
que no pasaban por `revisar()`**, que es quien convierte el errno negativo
del kernel en el `-1` y el `errno` que espera cualquier programa escrito
para un Unix. El kernel decia `-EINTR` y arriba llegaba tal cual, asi que
quien mirara `errno` veia lo que hubiera de antes.

## El fallo mas caro de este paso tampoco estaba en el codigo

Con las dos correcciones puestas, el shell seguia despidiendose. Y el
codigo estaba bien.

`lib/file.o` se estaba compilando con la version VIEJA de `user/syscall.h`.
La regla de la libc listaba sus dependencias a mano:

    $(BUILD)/lib/%.o: lib/%.c lib/stdio.h lib/string.h lib/stdlib.h

`user/syscall.h` no esta en esa lista, asi que cambiarlo no recompilaba
nada de la libc. El kernel llevaba `-MMD -MP` desde el principio; los
programas de usuario y la libc, no.

Una lista de cabeceras escrita a mano se queda corta en cuanto alguien
anyade un `#include`, y lo peor es como se presenta: **no parece un fallo
de construccion, parece un fallo de codigo.** Se lee el fuente, se ve
correcto, se vuelve a leer, y lo que se esta ejecutando es otra cosa. Es
la misma forma que tenia el fallo del paso 44, donde un `ls` de quince
pasos atras seguia en la tarjeta.

Ahora `-MMD -MP` esta tambien en `UCFLAGS`, y las tres familias de objetos
entran en `DEPS`. Los `.elf` necesitan un `-MF` explicito porque su regla
compila y enlaza de una vez, y `-MMD` deduce el nombre del fichero de
dependencias del `-o`, que aqui es un ejecutable.

## Ni vivo ni muerto

Hasta aqui un proceso estaba vivo o estaba muerto. Este paso trae el
tercer estado, que es el unico que no se le ocurre a nadie hasta que hace
falta: **detenido**.

    TASK_STOPPED,

Ni corre ni quiere correr, y -esto es lo que lo distingue de todo lo
demas- **no esta en ninguna cola de espera**. Una tarea bloqueada aguarda
un suceso: una tecla, un hueco en una tuberia, que muera un hijo. Una
tarea detenida no aguarda un suceso, aguarda un **permiso**. Por eso es un
estado y no una cola mas.

Y el planificador no se entero:

    if (t->state == TASK_READY)
        return t;

`pick_next` solo mira `TASK_READY`, asi que un estado nuevo se queda fuera
sin tocar una linea. Eso no es suerte: es lo que se gana cuando la
condicion se escribe en positivo. Una escrita al reves -"todo menos
zombi"- habria puesto a ejecutar procesos detenidos desde el primer
minuto, y el fallo habria aparecido en el sitio equivocado.

### Detenerse no tiene nada que guardar

    int task_parar(void)
    {
        uint64_t flags = sched_lock_irqsave();
        current->state = TASK_STOPPED;
        wq_wake_all(&exit_wq);          /* que el padre se entere ANTES */
        schedule_locked();
        int abortar = (current->sig_pending & (1u << SIGKILL)) != 0;
        sched_unlock_irqrestore(flags);
        return abortar ? -1 : 0;
    }

No hay contexto que salvar. El proceso esta a mitad de una llamada al
sistema, con su pila de kernel y su marco de excepcion tal cual: **si no
se ejecuta, tampoco se mueve**. Al reanudarlo sigue por la linea de abajo
y vuelve por donde entro. Comparado con lo que costo `fork` o una senyal,
detener un proceso es casi no hacer nada, y esa es la gracia.

El `wq_wake_all` de en medio si es necesario, y va **antes** de
`schedule_locked()`. Sin el, un shell que esta en `waitpid` esperando a
este proceso se queda ahi para siempre: el hijo no ha muerto -asi que no
despierta a nadie- y tampoco va a volver a correr. Los dos esperando al
otro, y nadie con motivo para moverse.

### Un proceso parado tiene que poder matarse

    if (t->state == TASK_STOPPED && (sig == SIGCONT || sig == SIGKILL)) {
        t->state = TASK_READY;
        ...
    }

Un proceso detenido no vuelve a EL0, asi que **no pasa por
`signal_deliver`**: apuntarle un bit de senyal ahi no sirve de nada.
Devolverlo a la vida es trabajo de `task_signal`, y `SIGCONT` existe
justo para eso.

Lo que es facil de olvidar es que `SIGKILL` tambien tiene que hacerlo.
Sin esa mitad de la condicion, un proceso parado seria **inmatable**: la
senyal fulminante se quedaria apuntada en la libreta de alguien que no va
a leerla nunca, y para matarlo habria que continuarlo primero. Asi que se
le devuelve la CPU lo justo para que se muera:

    / $ lento 40 zz
      ^Z
      [1] parado   lento 40 zz
    / $ kill 9 9
      [kernel] lento termina por la senyal 9

### Parar y seguir se anulan

    if (sig == SIGCONT) {
        t->sig_pending &= ~((1u << SIGSTOP) | (1u << SIGTSTP) | ...);
    } else if (ES_PARADA(sig)) {
        t->sig_pending &= ~(1u << SIGCONT);
    }

Y se hace al APUNTARLAS, no al entregarlas. Si un Ctrl-Z y un `SIGCONT`
llegan casi a la vez y las dos se quedan pendientes, el proceso arranca y
se vuelve a parar, o al reves, segun el orden en que se recorran los bits.
Ninguno de los dos resultados es el que pidio nadie: **vale lo que dijo el
ultimo en hablar**, y para eso la que llega borra a su contraria.

### Ni SIGKILL ni SIGSTOP se atrapan

    uint64_t h = (s == SIGKILL || s == SIGSTOP) ? 0 : t->sig_handler[s];

Son las dos unicas garantias que le quedan a quien esta fuera. Una es
"esto se puede matar" y la otra "esto se puede parar", y las dos dejan de
valer en cuanto el programa puede opinar. Ctrl-Z manda `SIGTSTP`, que si
se atrapa, porque ahi lo que se quiere no es una garantia sino una
costumbre: un editor que quiere dejar el terminal como lo encontro antes
de irse.

## El robo de teclas, y por que SIGTTIN no es un castigo

El paso anterior dejo esto anotado como limitacion: un proceso de segundo
plano que lea del teclado **roba las teclas** que ibas a escribirle al
shell, por turnos y sin avisar.

Y no fallaba nada. No hay error que devolver, porque leer un caracter que
existe es perfectamente legal. Por eso era tan desagradable: el shell
perdia una letra de cada dos y no habia nada a lo que culpar.

    while (!task_en_primer_plano()) {
        if (current->sig_handler[SIGTTIN]) {
            task_signal(current->pid, SIGTTIN);
            return -EINTR;
        }
        if (task_parar() < 0) return -EINTR;
    }

Leer el teclado desde el fondo no es un error: es una **pregunta a
destiempo**. El teclado lo tiene uno solo, y quien esta sentado ahi le
escribe al trabajo que ve delante. La respuesta de Unix es que el que
pregunta antes de tiempo espere su turno, y eso es lo que convierte
"compites por las teclas y pierdes la mitad" en "esperas". De paso es lo
que le da sentido a `fg`, porque ya hay algo parado a lo que volver:

    / $ wc &
      [1] en el fondo  wc
    / $ jobs
      [1] parado   wc

### Detenerse no es fallar

La primera version se paraba bien y no servia para nada:

    / $ fg
      wc
      0 lineas, 0 bytes

`wc` volvia del `read` con -1, decidia que se habia acabado la entrada y
se iba sin leer nada. La lectura que lo habia parado ya habia fracasado.

Por eso el `while` de arriba es un bucle y no un `if`: al continuar, **la
lectura se reintenta**. Detenerse no es fallar; es no hacerlo todavia, y
hacerlo despues. Es la misma idea que `SA_RESTART`, y es lo que hace que
un programa que no sabe nada de todo esto funcione igual estando en
primer plano que habiendo pasado por el fondo.

Con `SIGTTOU` -escribir desde el fondo- no se hace nada, y tampoco lo hace
Unix salvo que se lo pidas (`stty tostop`). Un proceso de fondo que
imprime es molesto, no peligroso: ensucia la pantalla y ya. Uno que lee te
quita algo.

## waitpid tiene que contestar dos cosas

    #define W_SALIDA   0   /* termino solo; el valor es su codigo */
    #define W_PARADO   1   /* NO ha terminado: esta detenido      */

Un `waitpid` que no distinguiera las dos seria peor que uno que no
contesta: el que espera creeria que su hijo ha acabado y seguiria adelante
dejando atras un proceso que sigue existiendo, con sus ficheros abiertos y
su memoria.

Unix mete las dos en el mismo entero y reparte bits, que es por lo que hay
que desmontarlo con `WIFEXITED` y companyia y por lo que nadie se acuerda
de como va. Aqui van por separado: cuesta un puntero y se entiende
leyendolo.

Que `WUNTRACED` haya que **pedirlo** -y no venga de serie- es de las pocas
decisiones de Unix que se explican solas: quien no sabe que existen los
procesos detenidos no sabria que hacer con uno.

## fg, bg, y el orden que sale caro

    consola(t->pgid);                    /* primero el testigo */
    kill(-(int64_t)t->pgid, SIGCONT);    /* luego el pistoletazo */

Equivocarse de orden sale caro y el sintoma no lleva a la causa: si el
`SIGCONT` va primero, el proceso arranca, intenta leer del teclado, ve que
no es el de primer plano, se gana un `SIGTTIN` y se vuelve a parar en el
acto. `fg` parpadearia y no haria nada, y estarias buscando el fallo en
`fg`.

`bg` es lo mismo **sin la primera linea**. Toda la diferencia entre traer
algo al frente y soltarlo en el fondo es esa: quien se queda el teclado.

El `kill(-pgid, ...)` es el convenio de Unix y no es un truco sucio: un
pid y un pgid viven en el mismo espacio de numeros -un grupo se llama como
su primer proceso- asi que para decir cual de los dos es hace falta algo
que no sea el numero. El signo estaba libre porque no hay pids negativos.

Ctrl-Z va al grupo, asi que para una tuberia entera, y `fg` la reanuda
entera:

    / $ lento 30 a | lento 30 b
      [a] 7 de 30      [b] 7 de 30
      ^Z
      [1] parado   lento 30 a | lento 30 b
    / $ fg
      [a] 8 de 30      [b] 8 de 30

### Una lista de trabajos que no sabe distinguir parado de corriendo

Dos fallos de contabilidad, los dos en el mismo sitio y los dos por
preguntar de menos.

El primero: `recoger()` preguntaba con `WNOHANG` a secas, asi que un
trabajo del fondo que se detenia -porque intento leer, que es lo normal-
contestaba "sigue vivo" y la lista decia **corriendo**. Peor que un error
de contabilidad: el usuario se queda esperando a que avance algo que no va
a avanzar.

El segundo aparecio al arreglar el primero. Los trabajos ya marcados como
parados se saltaban, porque volver a preguntar solo servia para
anunciarlos en cada prompt. Pero a un proceso parado se le puede mandar un
`SIGKILL` -es lo unico aparte de `SIGCONT` que lo saca de ahi- y entonces
la lista seguia diciendo "parado" de algo que ya no existia. Lo que no hay
que repetir no es la pregunta, es el **anuncio**: se dice al cambiar de
estado, no cada vez que se mira.

## La otra mitad del mismo fallo de despliegue

Dos pasos mas tarde, `make sd` volvio a morder, y por lo contrario de la
vez anterior.

El sintoma en la Pi era este:

    / $ ls
      LS.ELF: no lo encuentro. PATH=/usr/bin:.
    / $ cd usr
    /usr $ cd bin
      no puedo entrar en bin

`/usr` estaba y `/usr/bin` no. El codigo no tenia nada que ver: con una
tarjeta recien generada, los mismos binarios listaban y entraban
perfectamente. Lo que habia pasado estaba en la receta:

    rm -rf "$(DATA)/USR/BIN";
    cp -R $(BUILD)/sddata/. "$(DATA)/";
    echo "Copiado a $(DATA) (el raiz).";

**Borra primero y copia despues, sin comprobar ninguna de las dos cosas.**
Los `;` no son `&&`, asi que si el `cp` falla la receta sigue; y el ultimo
comando es un `echo`, que devuelve cero, asi que **make dice que todo fue
bien**. El resultado es un destino peor que antes de empezar y un mensaje
diciendo que esta al dia.

La vez anterior la leccion fue que una herramienta que nunca borra deja el
destino contando la historia entera. Esta es el reverso exacto: **una que
borra sin comprobar deja el destino sin nada, y encima te felicita.** Las
dos mitades son la misma regla vista de los dos lados — un despliegue
tiene que dejar el destino en un estado conocido, y *saber* que lo ha
dejado ahi.

Ahora la receta hace tres cosas que antes no hacia:

- **Mira el origen antes de tocar el destino.** Si `build/sddata/USR/BIN`
  esta vacio, se niega y no borra nada. Lo primero que hay que proteger no
  es la copia, es lo que ya estaba.
- **Encadena con `&&`** y aborta si el `cp` falla, en vez de seguir.
- **Cuenta lo que ha llegado** y lo compara con lo que salio, y comprueba
  que el `kernel8.img` de la tarjeta es byte a byte el recien compilado.
  Verificar despues de copiar cuesta una linea y es la unica forma de que
  "Copiado" quiera decir algo.

Y dice en que dispositivo escribe (`/Volumes/DATA -> disk4s2`), porque
cuando hay una tarjeta de verdad y una imagen de pruebas montadas a la
vez, macOS llama a la segunda `DATA 1` y el nombre que uno teclea de
memoria puede apuntar a cualquiera de las dos. Es el mismo peligro del
que ya se protege `sdtest`, un escalon mas abajo.

### Y un descuido mio, mas tonto y mas facil de repetir

El paso anterior terminaba con un `make clean` para comprobar que todo
compilaba desde cero. Compilaba. Lo que no hice fue volver a generar
`build/sd.img`, que es lo que `make run` usa:

    SDOPT = $(if $(wildcard $(SDIMG)),-drive file=$(SDIMG)...,)

Sin imagen, QEMU arranca **sin tarjeta** y en silencio: el servidor de
ficheros no encuentra la SD, init se queda sin `/etc/rc` y el shell
arranca sobre un sistema sin disco. Un `make clean` no es inofensivo
cuando parte del estado de pruebas vive dentro de `build/`.

## La ronda en la Pi, y lo que ensenya el retardo

Los pasos 50, 51 y 52 se escribieron y se comprobaron en QEMU. Esto es lo
que dijo el silicio, con la tarjeta de verdad y los cuatro nucleos de
verdad:

- **`anyadir`**: las dos mitades, con los numeros de la tabla de arriba.
- **Ctrl-Z, `jobs`, `fg`**: para en la 5 y **reanuda en la 6**. El estado
  del proceso sobrevive a la parada sin que nadie lo guarde, que es
  exactamente lo que se afirmaba: si no se ejecuta, tampoco se mueve.
- **Ctrl-C sobre `lento | lento`**: mueren los dos.
- **`wc &`**: se para solo, por `SIGTTIN`.
- **`libc`**: las veinte comprobaciones, y 4 llamadas a `write()` para
  2000 caracteres.

Nada se comporto distinto. Lo unico que cambio fue **cuando** pasan las
cosas, y una de esas diferencias merece contarse porque se leyo como un
fallo antes de entenderse:

    / $ wc &
      [4] en el fondo  wc
    / $ jobs
      [4] corriendo 22  wc
      [4] parado   wc

Dos lineas que se contradicen, y las dos son ciertas. La primera la
imprime `jobs` en el momento en que se teclea; la segunda la imprime
`recoger()` en el prompt siguiente. Entre una y otra, `wc` intento leer
del teclado por primera vez y se detuvo.

En QEMU no se veia asi: el programa llegaba a leer antes de que el shell
sacara el primer prompt, y la lista ya lo daba por parado. En la Pi,
cargar un ELF de 13 KB desde la tarjeta son unas setenta idas y venidas
por el IPC, y eso se nota.

Lo interesante es que la diferencia no es un retardo cualquiera: **un
programa no se para al nacer, se para la primera vez que pregunta por el
teclado.** Eso no se deduce mirando la lista de trabajos, y en QEMU los
tiempos lo tapaban.

## Entre la tecla y el programa

Los pasos 51 y 52 hicieron la mitad del terminal que reparte **senyales**:
quien recibe el Ctrl-C, quien esta delante, quien esta detenido. Falta la
otra mitad, que es la que procesa **lineas**.

Hasta aqui, el eco y el borrado los hacia el shell:

    int k = getchar();
    if (c == 8 || c == 127) { if (n) { n--; printf("\b \b"); } continue; }
    if (c >= ' ') { linea[n++] = c; printf("%s", eco); }

Funcionaba, y estaba en el sitio equivocado. Se ve en cuanto se intenta
escribir un programa que pida una contrasenya: **no hay forma de apagar el
eco**, porque el eco no existe en ningun sitio concreto. Lo hace cada
programa por su cuenta, asi que apagarlo exigiria que todos supieran
hacerlo, y bastaria uno que no lo supiera para que la contrasenya se
viera en pantalla.

Y hay un segundo sintoma, mas callado: el shell tenia edicion de linea y
**ningun otro programa la tenia**. Escribir un backspace en un `wc` metia
un byte 127 en el texto.

### La disciplina de linea

Lo que hay entre la tecla y el programa es algo mas que un cable, y en
Unix se llama *line discipline*. Vive en el kernel -ni en el driver del
hardware ni en el programa- porque es lo unico que esta a la vez entre
todos los teclados posibles y todos los programas posibles.

    static void disciplina(char c)

Un solo sitio donde se decide que significa cada tecla:

| tecla | que es |
|-------|--------|
| Enter | cierra la linea y la entrega |
| Backspace / DEL | deshace el ultimo caracter, y lo despinta |
| Ctrl-U | tira la linea entera |
| Ctrl-D | **entrega ya lo que tengas** |

De esa tabla sale sola una cosa que todo el mundo usa y casi nadie sabe
explicar: **Ctrl-D no es un caracter, es "entrega ya".** Si no tienes nada
que entregar, lo que se entrega es el final de la entrada. Por eso a mitad
de linea no cierra nada y dos seguidos si.

Y el borrado deja de ser un caracter que alguien tenga que entender: el
programa recibe la linea **ya corregida** y no llega a enterarse de que
hubo correcciones, igual que no se entera de que el usuario se lo penso
dos veces.

### El eco, y por que no puede ser del programa

    #define T_ECO        1
    #define T_CANONICO   2

Dos banderas, porque son dos decisiones distintas. `T_ECO` es si se pinta
lo que se teclea; `T_CANONICO` es si se entrega por lineas dejando
corregir antes. Un `stty` de verdad lleva treinta banderas; estas dos son
las que cambian lo que un programa **puede hacer**, y las demas cambian
detalles.

`clave` es el programa que lo demuestra, y lo interesante es lo que **no**
tiene dentro: no hace eco, no entiende un backspace y no ha oido hablar de
Ctrl-D. Y aun asi puedes corregir lo que escribes, y aun asi la
contrasenya no se ve:

    / $ clave
      nombre: david vidal
      clave (no se vera):
      has dicho que eres "david vidal"
      y la clave tiene 10 caracteres

Diez caracteres que nunca aparecieron. Lo unico que hace el programa es:

    int antes = termios(-1);
    termios(antes & ~T_ECO);
    fgets(...);
    termios(antes);                  /* pase lo que pase */

Esa ultima linea no es cortesia. El terminal es uno y lo comparten todos,
asi que un programa que se va con el eco apagado deja el shell escribiendo
a ciegas. Es la misma clase de obligacion que cerrar un descriptor.

Y solo lo puede tocar quien esta en primer plano, con la misma regla que
el Ctrl-C: si un proceso de segundo plano pudiera cambiarlo, podria
apagarte el eco y marcharse.

### El modo crudo, que es la otra mitad

Apagar `T_CANONICO` es lo que necesita un editor de pantalla: cada tecla
en cuanto se pulsa, sin esperar al Enter y sin que nadie corrija por ti.
Se nota en que el backspace deja de borrar y pasa a ser un numero:

      ahora en crudo: pulsa teclas, y 'q' para salir
      ab[97] [98] ^?[127] q

Ahi esta la frontera, y se ve de un vistazo: en canonico el 127 **hace**
algo, en crudo el 127 **es** algo.

## Una linea como mucho, y el cubo que dejo de robar

El paso 49 dejo una decision incomoda: del terminal se lee **sin cubo**,
de uno en uno. El motivo no era el rendimiento, era de quien son los
caracteres — si el shell se guardara 512 bytes "por si acaso", se estaria
quedando con lo que el usuario escribio para el programa que viene
despues.

Ese peligro desaparece con una linea de mas en la lectura:

    if ((term_modo & T_CANONICO) && c == '\n') break;

**Una linea como mucho, aunque quepan mas y aunque las pidan.** Con la
linea como frontera, el cubo no puede robar nada, porque lo que se lleva
es exactamente lo que se escribio para el. La regla de siempre no ha
cambiado -de un fichero puedes leer de golpe porque el fichero es tuyo- y
lo que ha pasado es que ahora **una linea del terminal tambien lo es**.

Comprobado de la unica forma que vale, con dos lectores seguidos:

    / $ wc
    hola que tal
    ^D
      1 lineas, 13 bytes

El shell leyo `wc` y ni toco `hola que tal`, aunque las dos llegaron
juntas y aunque ahora lee con un cubo de 512 bytes.

Y de propina, el numero:

      nombre: david vidal
      (esa linea son 12 caracteres y ha costado 1 viaje al kernel)

Doce antes, uno ahora. La mejora de velocidad es un **efecto secundario**
de una decision sobre de quien son los caracteres, y es la segunda vez que
pasa lo mismo en este proyecto: la primera fue el cubo de escritura del
paso 49.

## Dos cosas que se me olvidaron al mover el eco de sitio

Las dos aparecieron en la misma prueba y las dos son de lo mismo: cuando
una responsabilidad cambia de casa, se lleva consigo obligaciones que
antes se cumplian solas.

**La primera fue un `-1`.** `uart_leer` devolvia -1 al interrumpirla una
senyal, donde el codigo viejo convertia eso en `-EINTR`. Resultado: cada
Ctrl-C en el prompt convencia al shell de que se habia acabado la entrada,
y se despedia educadamente. Es **exactamente** el fallo del paso 51, dos
pasos despues, en la funcion que lo sustituyo.

**La segunda no tiene precedente y es mas interesante:**

    / $ hol^C
    / $ pwd
      HOLPWD.ELF: no lo encuentro.

La media orden sobrevivio al Ctrl-C y se pego a la siguiente. Antes esto
no podia pasar: la linea a medias vivia en el shell, y el shell la perdia
al volver a empezar **sin que nadie tuviera que hacer nada**. Ahora vive
en el kernel, y ahi no se pierde sola.

En Unix, una senyal del terminal tira tambien la entrada pendiente. Aqui
faltaba, y el sintoma no apunta a la causa: el shell no tiene forma de
saber que habia algo que tirar, porque eso ya no es suyo.

## El kernel avisa, o la primera senyal que nadie pidio

Habia un sintoma apuntado en las limitaciones desde el paso 52, y de los
que solo se ven en la Pi porque hacen falta milisegundos de verdad:

```
/ $ wc &
  [1] en el fondo  wc
/ $ jobs
  [1] corriendo  9  wc        <- miente: wc esta parado
/ $ jobs
  [1] parado     9  wc        <- ahora si
```

`wc &` arranca al fondo, intenta leer el teclado, se gana un `SIGTTIN` y se
detiene. Pero el shell se enteraba de los cambios de estado de sus hijos
**preguntando**, y solo justo antes de sacar un prompt. El `jobs` inmediato
salia con la foto anterior, porque entre el "en el fondo" y el prompt aun no
se habia parado nadie.

Y el que miente no es `jobs`: es el modelo entero. Preguntar antes del
prompt significa que el shell se entera cuando le apetece mirar, no cuando
ocurren las cosas, y de ahi salen las tres cosas que estaban mal: la lista
desfasada, los zombis que esperan a que vuelvas del cafe, y una recogida que
solo funciona porque el shell se acuerda de los pid de todos sus hijos.

Lo que falta es que el kernel hable. Es `SIGCHLD`, y trae consigo un
problema nuevo que las once senyales anteriores no tenian.

### Una senyal que nadie pidio

Repasemos de donde salian las que ya habia. `SIGINT` y `SIGTSTP`, de una
tecla. `SIGTERM` y `SIGKILL`, de un `kill` que alguien escribio. `SIGTTIN`,
de una lectura que el propio proceso intento hacer. `SIGSEGV` no existe
aqui, pero seria de una direccion que el propio proceso toco.

Todas las pidio alguien, y en casi todas ese alguien esta mirando.

`SIGCHLD` no. Llega porque OTRO proceso -un hijo- cambio de estado, en un
instante que el padre no eligio y no puede prever. Y de esa diferencia, que
parece filosofica, salen sus dos rarezas, que parecen arbitrarias:

**Su accion por defecto es no hacer nada.** Todas las demas matan al que no
las atrapa. Esta no puede: la accion por defecto de una senyal tiene que ser
razonable para quien no sabe que esa senyal existe, y "morirte porque tu hijo
acabo" no lo es. Si matara, `fork()` seria inutilizable sin saber de
senyales.

**Y hace falta poder decir que no interrumpa.** Una senyal saca al proceso de
la llamada al sistema en la que estuviera bloqueado, y esa llamada vuelve con
`-EINTR`. Con Ctrl-C eso es justo lo que se busca: romper la lectura ES el
objetivo. Aqui no tiene nada que ver: que un hijo termine no le incumbe a la
lectura que su padre tenga a medias, y romperla es contarle un fallo que no
ha ocurrido.

Las dos respuestas son correctas, para senyales distintas. Por eso en Unix
es una bandera POR SENYAL y no una politica del sistema, y por eso existe
`sigaction`: `signal()` a secas no tenia donde ponerla. Aqui la bandera es
`SIG_REANUDAR` -el `SA_RESTART` de siempre- y se pide con
`signal_banderas()`. `signal()` sigue significando lo de antes, que es lo
que Ctrl-C necesita.

### Lo que se va a ignorar no puede llegar a molestar

Este es el fallo que habria tirado el sistema entero, y lo bonito es que no
se ve mirando SIGCHLD: se ve mirando quien mas tiene hijos.

Apuntar una senyal hace dos cosas. Una es dejar el bit. La otra es sacar al
proceso de donde estuviera dormido, porque una senyal tiene que poder
interrumpir una espera que no iba a acabar sola. Y esa segunda parte ocurria
aunque la senyal acabara sin hacer nada.

`init` espera a su interprete con un `waitpid` bloqueante y no atrapa
`SIGCHLD`. Si cada proceso que muere en el sistema lo sacara de ahi, su
`waitpid` volveria con `-EINTR`, init creeria que el interprete se ha ido y
arrancaria otro. Un servidor de ficheros terminando mataria la sesion.

Asi que ignorar tiene que significar **como si no hubiera llegado**, y eso
incluye no despertar a nadie. La comprobacion va antes de apuntar el bit, no
al entregarlo:

```c
if (t && sig == SIGCHLD && !t->sig_handler[SIGCHLD]) return 0;
```

Linux tiene esta misma linea y en el mismo sitio; se llama `sig_ignored()`.
Hasta este paso no hacia falta, porque no habia ninguna senyal que el kernel
mandara por su cuenta a alguien que no la esperaba.

### Reanudar una llamada partida por la mitad

Rebobinar una llamada al sistema es mas facil de lo que parece, y el motivo
es que en AArch64 las instrucciones miden todas cuatro bytes: el `ELR_EL1`
que se guardo apunta justo detras del `svc`, asi que restarle cuatro apunta
al `svc`. Al volver del manejador, el proceso vuelve a ejecutar la llamada
que ya habia hecho.

Lo unico que falta es el x0 que traia, y falta porque el kernel lo pisa: el
valor de retorno va ahi. Los demas argumentos -x1 a x7- siguen intactos en
el marco, porque nadie los toca. Asi que se guarda uno solo, y se guarda en
el unico sitio por donde salen todas las llamadas:

```c
if (ret == -EINTR) task_marcar_reanudable(a0);
```

Linux lleva este mismo apunte y lo llama `orig_x0`. El `a0` de ahi no hubo
que inventarlo: ya estaba en una variable local porque hacia falta para otra
cosa.

Y la marca se borra **al salir a EL0, siempre**, lo primero de
`signal_deliver` y antes de cualquier comprobacion. Dejarla puesta es la
forma de romperlo todo: la siguiente senyal que llegara -por una
interrupcion del reloj, en medio de codigo de usuario cualquiera- rebobinaria
un PC que no apunta a ningun `svc`, y el proceso se iria a ejecutar lo que
hubiera cuatro bytes antes.

El rebobinado se escribe en el marco ANTES de copiarlo a la pila del
proceso, y eso no es un detalle de orden: lo que restaura `sigreturn` es esa
copia. Si se rebobinara despues, se rebobinaria un marco que ya nadie va a
leer.

Se ve funcionando en que no se ve nada:

```
/ $ lento 2 x &
  [1] en el fondo  lento 2 x
/ $ ec                         <- media orden escrita, sin Enter
  [x] terminado                <- el hijo muere AQUI, con la linea a medias
ho la-linea-sobrevivio         <- se termina de escribir
la-linea-sobrevivio            <- y ejecuta la orden entera
```

Sin `SIG_REANUDAR`, en el momento en que muere el hijo el `fgets` del shell
habria vuelto con `EINTR`, el shell habria dado la linea por cortada y
habria sacado un prompt nuevo en medio. La media orden no se habria perdido
-desde el paso 53 vive en el kernel, y una senyal de estas no la tira- pero
el usuario habria visto un prompt aparecer por su cuenta mientras escribia.

### Recoger en el manejador, contar en el prompt

Aqui esperaba yo que el manejador de `SIGCHLD` imprimiera "[1] hecho", y
esta bien que no lo haga, por un motivo que no tiene nada que ver con la
estetica.

`printf` no es reentrante. Escribe en el cubo de `stdout`, y si la senyal
llega justo cuando el bucle principal estaba a medias de otro `printf`, las
dos escrituras se pisan. Un manejador de senyal solo puede llamar a un
punado de funciones -en Unix hay una lista, y `printf` no esta en ella-
porque puede aparecer en cualquier punto del programa, incluido el medio de
una.

Asi que se parte en dos lo que antes era una funcion:

- **`recoger()`** decide y apunta, y no imprime nada. La llama el manejador
  en cuanto un hijo cambia de estado.
- **`anunciar()`** lo cuenta, y solo la llama el bucle principal justo antes
  del prompt.

Y resulta que eso es exactamente lo que hace bash: se entera en el acto y lo
dice en el prompt siguiente. Yo creia que era una cortesia para no
ensuciarte la linea que estas escribiendo; es que el manejador no puede
hablar.

El resultado es que el `jobs` inmediato ya dice la verdad -la tabla se
actualizo cuando `wc` se paro, no cuando al shell le toco mirar- y el
anuncio sigue saliendo en un sitio limpio.

### La ventana en que al manejador no le toca

Mientras el bucle principal espera a un trabajo de primer plano, esta
esperando a un hijo CONCRETO y quiere el codigo de salida de ese. Si el
manejador se pusiera a recoger por su cuenta, podria llevarse justo a ese, y
el `waitpid` de arriba volveria con `-ECHILD`: el shell se quedaria sin
saber como acabo lo que acabas de ejecutar.

En Unix esto se arregla bloqueando la senyal alrededor del trozo delicado,
con `sigprocmask`. Aqui no hay mascaras, asi que lo unico que se puede hacer
es que el manejador sepa cuando no le toca: una variable `no_molestes`
puesta alrededor de las esperas de primer plano. La senyal se pierde, y no
pasa nada grave, porque el `recoger()` del prompt siguiente encuentra lo que
quedara.

Que una senyal se pueda perder asi es, por si hacia falta el argumento, la
razon de que `sigprocmask` exista.

### Cualquiera de mis hijos, y el noveno trabajo

`waitpid` pedia un pid concreto, y el shell podia porque se acuerda de los
pid de cada trabajo. Menos de uno.

`anotar()` devuelve 0 cuando ya no caben mas trabajos -la tabla son ocho- y
**ninguno de los cuatro sitios que la llaman mira ese valor**. Asi que el
noveno trabajo de fondo se bifurca, corre, termina... y se queda de zombi
para siempre, porque el unico que podia enterrarlo era el que no se acordo de
apuntarlo. Una ranura de la tabla de tareas y su memoria, perdidas hasta que
muera el shell, que no muere nunca.

Eso es lo que `PID_CUALQUIERA` sabe hacer y un pid concreto no: recoger sin
saber a quien. No hace falta preguntar quien era, porque de estos no hay
nada que contar -nadie los apunto- y lo unico que se les debe es el
entierro.

Dos cosas del barrido no son obvias:

**Va despues del recorrido de la tabla.** "Cualquiera" incluye a los que si
conocemos, asi que si se barriera primero, los trabajos apuntados perderian
su codigo de salida en manos de quien no sabe de quien era.

**Y va sin `WUNTRACED`.** Un hijo parado contestaria en cada vuelta -nadie
lleva la cuenta de si ya se aviso de esa parada, y eso ya estaba en las
limitaciones- y el bucle no acabaria nunca. Aqui solo se entierra; pararse no
es morirse.

### Una prueba que falla cuando debe, otra vez

El zombi que se escapa no se ve: no hay `ps`. Lo que si se ve es lo que
acaba pasando cuando se escapan varios, y para medirlo hay que quedarse sin
ranuras a proposito.

Ocho `wc &` llenan la tabla de trabajos -cada uno se para con su `SIGTTIN`- y
a partir del noveno, cada trabajo de fondo que se lance corre sin que nadie
lo apunte. Con el barrido puesto, diez seguidos funcionan y el shell sigue
vivo. Sin el:

```
  no caben mas trabajos        x7
  no he podido bifurcarme      <- y aqui se acabaron las ranuras
  no he podido bifurcarme
  no he podido bifurcarme
```

Siete zombis, veinte ranuras de tarea y cuatro ocupadas por init, los dos
servidores y el shell. Las cuentas salen, y la prueba mide algo: la comprobe
desactivando el barrido a mano, que es la unica forma de saber que una prueba
verde no esta verde por casualidad.

### El arbitro no puede dejarse quitar el teclado

Poner el aviso destapo un agujero que llevaba ahi desde el paso 52, y que
este paso no causa pero si permite ver con claridad: **un Ctrl-Z en el prompt
paraba el shell para siempre**. No lo atrapaba, la accion por defecto lo
detenia, e init espera a que su interprete TERMINE y no a que se pare. Nadie
lo reanimaba. La maquina se quedaba muda y se arreglaba apagando.

Lo comprobe compilando el commit anterior: se portaba igual antes y despues,
asi que no lo traia el paso 54.

La tentacion era arreglarlo desde init, que ahora se enteraria -para eso
acabo de ponerle el aviso-. Y es el sitio equivocado: init no tiene por que
opinar sobre si un interprete quiere estar parado. El que tiene que decidir
eso es el propio shell, y lo que decide cualquier shell de verdad es que a el
no le pare nadie. No es que no le importe la senyal: es que **el reparte el
teclado, y quien hace de arbitro no puede dejarse quitar por el juego**. Es
exactamente la misma regla que ya justificaba atrapar `SIGINT`, y no me di
cuenta de que era la misma hasta tener las dos delante.

Asi que una linea, y el mismo manejador vacio que ya servia para Ctrl-C:

```c
signal(SIGINT,  tragar);
signal(SIGTSTP, tragar);
```

Lo interesante son las dos cosas que NO hay que hacer ahi.

**No es un SIG_IGN, y aqui eso es lo correcto y no un atajo.** Un manejador
vacio y un `SIG_IGN` de verdad no son lo mismo: el primero interrumpe la
llamada que estuviera bloqueada, y el segundo no llega ni a molestar -es la
misma distincion que acaba de aparecer con SIGCHLD-. Aqui hace falta que la
interrumpa, porque el driver del terminal **ya tiro la linea que estabas
escribiendo** antes de saber quien iba a atender la senyal, igual que hace
con Ctrl-C. Si el shell no se enterara, se quedaria en pantalla una orden a
medias que ya no existe en ningun sitio. Que la lectura vuelva con `EINTR` es
lo que le dice "redibuja". El resultado es que Ctrl-Z en el prompt se porta
como Ctrl-C, que es lo unico que puede hacer: tirar la linea y empezar otra.

**Y `SIGTTIN` y `SIGTTOU` no se tocan, que es la diferencia con bash.** Alli
se ignoran las tres, porque a bash se le puede arrancar desde el fondo de
otro shell. Aqui el interprete esta siempre en primer plano -init le da la
consola al nacer y el la recupera despues de cada trabajo- asi que nunca va a
leer el teclado sin tener derecho. Y atraparlas con un manejador vacio seria
PEOR que no hacerlo: un shell del fondo que leyera se quedaria girando
-lectura, `EINTR`, prompt, lectura- en vez de pararse, que es justo la
proteccion que `SIGTTIN` existe para dar. Copiar a bash las tres lineas
habria cambiado un agujero que no se puede alcanzar por otro que si.

Ctrl-Z sobre un hijo de primer plano no cambia, y no porque se haya tenido
cuidado: la senyal va al GRUPO que tiene la consola, que en ese momento es el
del hijo y no el del shell. El manejador nuevo no se entera de que existe.

## waitpid tiene que contestar tres cosas

Esta llamada empezo contestando una sola: el codigo de salida. Cada vez que
le faltaba algo se vio donde, y siempre igual -alguien intentando usarla para
algo razonable y descubriendo que la respuesta no daba-.

Primero falto **que paso**, en el paso 52: "parado" no es "terminado", y un
waitpid que las confundiera hacia que el shell siguiera adelante dejando
atras un proceso vivo con sus ficheros abiertos. Se arreglo con un puntero de
salida, `que`, en vez de repartir bits como Unix.

Y en el paso 54 falto **quien**. Puse `PID_CUALQUIERA` porque hacia falta
-un manejador de SIGCHLD no puede preguntar por un pid concreto, porque la
senyal dice que algo cambio y no que cambio- y el resultado fue un mecanismo
a medias: se sabia que habia noticias, pero no de quien. Solo servia para
enterrar. El shell tuvo que seguir recorriendo su tabla de trabajos con pids
concretos, y el barrido de los hijos que no estaban en la tabla quedo como un
segundo bucle aparte.

Son tres, y en un entero no caben.

### El pid no es solo el pid: es el canal de los errores

Devolver el pid parece un detalle de comodidad y es dos cosas a la vez, y la
segunda es la que de verdad estaba rota.

Mientras en el valor de retorno iba el codigo de salida, ese valor tenia que
transportar dos cosas incompatibles: un codigo, que puede ser cualquier
numero con su signo, y un errno, que es negativo por convenio desde el paso
48. Se pisaban. Un hijo que saliera con -11 era indistinguible de un
`-EAGAIN`, y no era un caso rebuscado: **un proceso al que mata una senyal
sale con -1, que es exactamente `-EPERM`**. Lo tenia apuntado en las
limitaciones con un "el barrido lo soporta porque equivocarse ahi solo le
cuesta acabar una vuelta antes", que es la clase de frase que uno escribe
cuando sabe que algo esta mal y no ve como arreglarlo.

Y no se podia arreglar, porque las dos cosas necesitaban todo el rango.

Un pid, no. Un pid es siempre positivo. Asi que en el momento en que el valor
de retorno es un pid, el negativo queda libre entero para los errores y la
ambiguedad no es que se resuelva: **deja de poder existir**. Los dos
problemas -no saber quien, y no poder distinguir un codigo de un fallo- se
van con el mismo cambio, y ahora entiendo que Unix devuelva el pid aqui.
Siempre habia dado por hecho que era para identificar al hijo.

```c
/* (pid, banderas, &que, &codigo) -> pid | -errno */
```

Los otros dos van por puntero y se pueden dejar a cero si no interesan.
`waitpid(pid)` a secas -bloquear hasta que ese hijo acabe, que es lo que
quieren `libc`, `anyadir`, `fp`, `forkd` e `init`- sigue siendo una linea.

### El tercero cambia de significado segun el segundo

`codigo` con un `W_SALIDA` es lo que devolvio el proceso. Con un `W_PARADO`
no puede ser eso, porque un proceso detenido no ha devuelto nada: es **la
senyal que lo paro**. Es lo mismo que hace Unix con `WEXITSTATUS` y
`WSTOPSIG` sobre el mismo status, solo que sin tener que desmontar bits.

No es un ahorro de sitio. Es que para un proceso parado "con que numero
acabo" no quiere decir nada y "quien te paro" si, y hasta ahora ahi iba un
cero. De eso sale una cosa que se ve:

```
/ $ wc &
  [1] en el fondo  wc
/ $ jobs
  [1] parado (queria el teclado)  9  wc

/ $ lento 9 pausa
  [pausa] 1 de 9
^Z
  [2] parado (Ctrl-Z)  lento 9 pausa
```

Las dos decian "parado" y son situaciones distintas: la segunda la pediste tu
y se arregla con `fg` cuando te apetezca; la primera es un programa que esta
esperando algo que solo `fg` le puede dar. Si la lista no te dice por que
esta parado, no te dice que te esta esperando.

### Una parada solo es noticia una vez

Esto no lo quise anyadir: hizo falta, y el sitio donde hizo falta lo dice
mejor que ninguna explicacion.

Con el pid en la respuesta, lo natural es darle la vuelta al bucle del shell:
en vez de recorrer la tabla preguntando por cada pid que creemos que existe,
preguntar "¿de quien hay noticias?" y buscar despues a quien pertenece. Un
bucle que llama a `waitpid(PID_CUALQUIERA)` hasta que no quede nada.

Y ese bucle no acababa nunca. Un zombi se recoge y desaparece de la lista de
candidatos, pero **una parada no desaparece**: el proceso sigue ahi, y sigue
parado, y con `WUNTRACED` volvia a contestar en cada vuelta. Siempre lo
mismo, y quien pregunta sin forma de distinguir "otra noticia" de "la misma
otra vez".

Estaba apuntado en las limitaciones desde el paso 52 -"`WUNTRACED` avisa cada
vez que se pregunta, no solo la primera; un Unix lo lleva en el propio
proceso"- y ahi se quedaba porque el shell lo tapaba anunciandolo solo al
cambiar de estado. Se tapaba mientras el shell recorriera su propia tabla, o
sea mientras supiera de antemano a quien preguntaba. En cuanto el que decide
de quien hablar es el kernel, la tapa no vale.

Asi que son dos campos en el hijo, y el detalle esta en cuando se limpia el
segundo:

```c
    current->parada_sig     = sig;
    current->parada_avisada = 0;
```

Se limpia en `task_parar`, o sea **cada vez que se para de verdad**, y no al
reanudarlo. Lo natural habria sido lo otro -"ya no esta parado, asi que la
proxima parada sera noticia"- y no se puede: reanudarse no es un suceso que
aqui exista. Nadie pasa por ningun sitio cuando un proceso vuelve a correr,
porque volver a correr es que el planificador lo elija, que no es un evento
que nadie observe. Poniendolo al pararse sale gratis y sale bien: si se para,
si se reanuda y si se vuelve a parar, la segunda parada es noticia otra vez.
Comprobado con un Ctrl-Z, un `fg` y otro Ctrl-Z.

Y las dos son del hijo, no del que pregunta, que es donde las lleva un Unix:
"esta parada ya se conto" es un hecho sobre el proceso.

### Dos bucles que se convierten en uno

Con el pid y con la parada que solo se cuenta una vez, las dos funciones del
paso 54 dejan de tener motivo para ser dos:

- `recoger()` recorria la tabla de trabajos, un waitpid por pid conocido.
- `barrer_ajenos()` barria con `PID_CUALQUIERA` lo que la tabla no conocia
  -el noveno trabajo de fondo, el que no cupo y que nadie iba a enterrar-.

Eran dos bucles, dos formas de preguntar, y la lista de a quien preguntar
escrita en un sitio distinto del que decidia que hacer con la respuesta.
Ahora es uno: se pregunta de quien hay noticias, se busca a quien pertenece
el pid, y si no pertenece a nadie se ignora -que ya esta enterrado con solo
haber preguntado, y era todo lo que se le debia-.

```c
        int64_t quien = waitpid_ya(PID_CUALQUIERA, &que, &codigo);
        if (quien < 0) return;           /* -EAGAIN o -ECHILD: ya esta */

        struct trabajo *t = trabajo_de((uint64_t)quien);
        if (!t) continue;                /* uno que nadie apunto */
```

Ese `quien < 0` es el canal de errores separado en una sola linea: antes
habia que comparar contra `-EAGAIN` y contra `-ECHILD` uno por uno, y aun asi
un codigo de salida desafortunado podia colarse por ahi.

El barrido no es que se haya quitado: es que ha dejado de ser codigo. Sale
del mismo bucle, y eso es lo que suele pasar cuando una respuesta empieza a
traer la informacion que le faltaba.

### Un anuncio no es un mensaje

Esto salio en la primera prueba en la Pi, y de las dos cosas que estaban mal
solo una se veia.

```
/ $ wc &
  [1] en el fondo  wc
/ $ jobs
  [1] parado (queria el teclado)  9  wc
  [1] parado (queria el teclado)   wc     <- otra vez, en el prompt
```

La segunda linea la saca `anunciar()` en el prompt siguiente, porque el aviso
seguia pendiente: `jobs` te lo habia contado y no se habia dado por contado.
Y ahi estaba el error de concepto, pequenyo y con consecuencias: yo trataba el
anuncio como un mensaje que hay que emitir, cuando es **asegurarse de que lo
sabes**. Ensenyarte el estado ya es eso. bash marca los trabajos como
notificados cuando los lista, y ahora se por que.

Lo que no se veia es peor. Entre el `anunciar()` del prompt y el momento en
que `jobs` se ejecuta esta **todo el rato que tardas en teclear la orden**. Un
trabajo que termine en esa ventana llega a la lista con sus pids ya a cero, con
`parado` en falso y con su aviso sin contar... y se listaba como "corriendo".
En el codigo de antes del arreglo las dos lineas contradictorias salen
seguidas:

```
  [2] corriendo  10  lento 1 z     <- jobs, sobre un proceso ya enterrado
  [2] hecho    lento 1 z           <- el prompt, una linea despues
```

En QEMU esa ventana es la de un guion, o sea milisegundos. En la Pi es la de
unos dedos.

El arreglo es que `jobs` salde lo que ensenya: a los parados les da el aviso
por contado, y a los terminados no los lista -no les queda estado que
ensenyar- sino que los cuenta y suelta su ranura. Son las dos unicas
respuestas posibles una vez que se acepta que listar y anunciar son lo mismo
dicho de dos maneras.

Y hay una tercera cosa que aprendi mirandolo: el fallo no lo trajo este paso.
Lo trajo el 54, cuando el anuncio se separo de la recogida para que el
manejador de SIGCHLD pudiera llamar a una sin la otra. Desde entonces habia
dos sitios que imprimian el estado de un trabajo y solo uno se acordaba del
aviso. Partir una funcion en dos es facil; acordarse de que ahora hay dos
sitios que tienen que estar de acuerdo, menos.

### Lo que sigue sin haber

No hay `WCONTINUED`, o sea que "ha seguido" no es una noticia. El shell se
entera de las paradas y de las muertes, pero si alguien reanima un trabajo
con un `kill -18` a mano, la lista seguira diciendo que esta parado. No le
afecta al uso normal porque los unicos SIGCONT que existen aqui los manda el
propio shell -`fg` y `bg`- y esos si los apunta el, pero antes de este paso
lo acertaba por casualidad: el bucle viejo preguntaba por todos sus pids en
cada vuelta y veia que ese seguia vivo. Un bucle que reacciona a sucesos solo
sabe de los sucesos que hay.

## Un editor, y por que vi es modal

Hasta aqui no se podia editar un fichero dentro de TinyOS. Habia `write`, que
crea uno con lo que quepa en una linea de orden, o sea un centenar de bytes.
Para cualquier otra cosa habia que sacar la tarjeta y enchufarla al Mac.

Un editor es ademas la primera pieza del camino largo -compilar TinyOS dentro
de TinyOS- y es la unica de ese camino que se paga sola aunque el resto no se
haga nunca.

### La restriccion que explica el diseno entero

Un editor necesita muchas mas ordenes que teclas. Mover el cursor en cuatro
direcciones, insertar, borrar, buscar, guardar, salir, deshacer... y por la
linea serie solo llegan bytes. No hay Alt, no hay teclas de funcion en las que
confiar, no hay raton. Con Ctrl hay 31 combinaciones y ya tienen duenyo.

Asi que las teclas tienen que significar cosas distintas en momentos
distintos. Eso es un MODO, y de ahi sale vi: no es una rareza historica ni
cabezoneria de los viejos, es lo unico que se puede hacer con 26 letras y
cincuenta ordenes.

Y hay una segunda razon, que en esta maquina se puede medir. La linea va a
115200 baudios: 11.520 bytes por segundo. Una pantalla de 80x24 son 1.920
caracteres que con los escapes se van a unos 2.000 bytes, o sea **174
milisegundos**. Si el editor redibujara todo en cada tecla, escribir seria
como escribir debajo del agua.

Bill Joy escribio el vi original en una linea de 300 baudios: 30 bytes por
segundo, o sea **64 segundos por pantalla**. Todo lo que le parece raro a la
gente de vi -que no haya menus, que las ordenes sean una letra, que no se
refresque la pantalla a lo tonto- sale de ese numero. Tenemos 384 veces mas
ancho de banda y la leccion sigue valiendo.

Por eso este editor lleva su propio instrumento: **Ctrl-G dice cuantos bytes
ha mandado al terminal** desde que arranco. Es la unica forma honesta de saber
si lo que acabo de escribir es verdad.

### Y el instrumento me llamo mentiroso a la primera

Habia escrito en un comentario que mover el cursor "no cuesta ni un byte de
redibujado". Once movimientos, y Ctrl-G:

```
  antes:  2.638 bytes
  once h/j/k/l despues:  3.886 bytes      -> 113 bytes por tecla
```

Ciento trece. Lo que pasaba es que cada movimiento repintaba la **linea de
estado entera**: ochenta caracteres de relleno y dos escapes de video inverso,
para que cambiara un numero de dos cifras.

Repintar solo el numero, en su esquina, son 36 bytes contando el
reposicionamiento del cursor. Tres veces menos. Y aun asi es cuatro veces mas
que no tener contador: el vi original no ensenyaba la posicion salvo que se la
pidieras con Ctrl-G, y ahora se por que. En vim la "regla" es una opcion, y
tambien viene apagada.

La medida completa, con el editor ya arreglado:

| lo que se hace | bytes |
|---|---|
| abrir un fichero (pantalla entera) | ~1.900 |
| escribir una letra (su linea + la posicion) | ~60 |
| mover el cursor (la posicion + el cursor) | ~36 |
| mover el cursor sin contador de posicion | ~8 |

### write() no escribe todo lo que le pides

Esto me costo una hora y es la mejor leccion del paso.

La primera version pintaba dos lineas y media y se paraba. Sin error, sin
aviso: la pantalla salia cortada y el resto desaparecia. Los bytes de verdad,
capturados del puerto serie, se cortaban en **exactamente 128**.

`consola_write` en `src/file.c` copia a un buffer de rebote de 128 bytes
-`BOUNCE`, pequenyo porque la pila del kernel es UNA pagina-, se queda con los
primeros 128 y **devuelve cuantos ha cogido**. Es una escritura parcial, y es
perfectamente legal: lo dice POSIX y lo hace cualquier Unix en cuanto hay una
tuberia o un socket por medio. Quien llama tiene que dar la vuelta.

Lo bonito es por que no habia salido antes: **el editor es el primer programa
de este proyecto que escribe mas de 128 bytes de una vez.** Todo lo anterior
imprimia lineas sueltas de texto. Y la libc ya lo hacia bien desde el paso 49
-`lib/file.c` tiene su bucle `while (o < n)`- asi que el fallo no estaba en lo
viejo, estaba en lo nuevo. Un contrato que llevaba 57 pasos cumpliendose sin
que nadie lo mirara.

De paso, una consecuencia que conviene tener presente: con `BOUNCE` en 128, un
redibujado completo son **16 llamadas al sistema** por mucho cubo que se le
ponga delante. El cubo sigue valiendo -sin el serian cien- pero el suelo lo
pone el kernel.

### El modo, siempre visible

El vi original no ensenyaba en que modo estabas. No le sobraban ni filas ni
baudios, y es la queja mas repetida que ha tenido un programa en la historia.
Aqui el modo se ensenya siempre.

Y de eso salio el segundo fallo, que es peor que el primero: al pulsar Escape,
el editor volvia a modo ordenes y la linea de estado **seguia diciendo `--
INSERTAR --`**, porque la salida del modo insercion usaba el redibujado barato
-el que solo toca la posicion- y no llegaba a borrar el texto de la izquierda.

Un estado invisible se adivina mal. Uno visible y falso se cree. Es el peor
fallo posible en un programa modal, y lo tenia yo en el unico sitio del codigo
donde se puede salir del modo.

### El terminal es prestado, y ahora hay trabajo que perder

El editor apaga `T_ECO` y `T_CANONICO` -el modo crudo del paso 53- y se queda
con cada tecla. Pero hay dos que no le llegan nunca, y eso lo decide el driver
mucho antes:

```c
        if (c == 3)  { interrumpir = 1; continue; }   /* uart_irq() */
        if (c == 26) { parar = 1;       continue; }
```

Ctrl-C y Ctrl-Z se los queda `uart_irq` **antes de la disciplina de linea**,
asi que llegan como senyales incluso en modo crudo. Y aqui eso importa como no
importaba antes: **este es el primer programa del proyecto donde perder contra
una senyal cuesta TRABAJO y no solo un proceso.** Si el editor no atrapa
SIGINT, un Ctrl-C se lleva todo lo que no hayas guardado.

Con SIGTSTP hay que hacer trampa, y se nota que es trampa: se traga. Si el
editor se detuviera, dejaria el terminal en modo crudo y sin eco, y el shell
se quedaria escribiendo a ciegas -que es exactamente la limitacion que el
paso 53 dejo apuntada, ahora con dientes-. Suspender un editor de verdad exige
devolver el terminal al pararse y volver a cogerlo al seguir, con SIGCONT y un
redibujado. Eso es un paso aparte, y es el que hace falta para que la tercera
pieza del control de trabajos quede completa.

### Lo que hace, y con que teclas

```
  h j k l  0  $  G  gg  w  b        moverse
  i a A  o O                        insertar (Escape para salir)
  x  D  dd                          borrar
  /texto   n                        buscar, y el siguiente
  :w  :w fichero  :wq  :q  :q!      guardar y salir
  Ctrl-G                            que fichero, cuantas lineas, cuantos bytes
  Ctrl-L                            repintar
```

`:q` con cambios sin guardar se niega y te dice que uses `:q!`. Es lo que hace
vi, y aqui ademas es el sustituto barato de lo que no hay.

### Lo que no tiene, y lo que falta de verdad

Lo que falta de verdad es **deshacer**. No hay `u`, y sin `u` un `dd` en la
linea equivocada es definitivo. El vi original tenia UN nivel de deshacer -uno
solo- y eso no era tacanyeria: guardar el estado anterior de una linea es
barato, guardar una historia entera necesita decidir que es "un cambio", que es
la parte dificil y la que vim tardo veinte anyos en hacer bien. Es el paso
siguiente.

Lo demas son casos del mismo `switch`, que es lo que suele quedar cuando el
mecanismo ya esta: no hay contadores (`3dd`, `5G`), ni copiar y pegar (`yy`,
`p`), ni `J` para juntar lineas, ni `r` ni `cw`. Y la busqueda es texto tal
cual: las expresiones regulares son el otro programa que habria que escribir, y
no es este.

## Los cimientos de un driver de USB

Esto no conduce USB. Lo que hace es quitar las dos cosas que hacian imposible
escribir un driver de USB en EL0, y probar que ya no estan.

Y hace falta empezar por ahi porque en la Pi 3B el USB no es "un puerto mas".
Del controlador DWC2 cuelga un LAN9514, que es a la vez el hub de los cuatro
conectores **y la tarjeta de red**. Sin USB no hay red ni almacenamiento
externo, y hasta la Ethernet esta detras del hub interno: no hay atajo por
ningun lado.

### Una tabla, y no una variable

`irq_register()` decia esto:

```c
    if (irq != IRQ_UART || puerto < 0) return -1;
    if (irq_puerto >= 0) return -1;      /* ya la lleva otro */
```

Una fuente cableada y **un** hueco. Con el conserver dentro, no cabia nadie
mas. Ahora son una tabla de cuatro y una lista de lo reclamable con dos
entradas: la UART y el USB.

La lista sigue escrita a mano, y eso no ha cambiado -un sistema serio la
sacaria de un arbol de dispositivos-. Lo que ha cambiado es que ya no es
*"esta escrito a mano Y ADEMAS solo cabe uno"*. La del temporizador no esta
ni estara: dejar que un proceso se quede con ella es dejarle parar el
planificador.

Y al soltarla hay una asimetria que merece mirarse, porque es la unica parte
de esto que no es mecanica:

```c
        if (irq == IRQ_UART) irq_abrir(irq);
        else                 irq_cerrar(irq);
```

La de la UART **se vuelve a abrir** cuando muere su driver, porque el kernel
tiene driver propio de PL011 y puede seguir el solo: es lo que hace que matar
al conserver no deje la maquina sin teclado. Cualquier otra **se cierra**,
porque detras no hay nadie, y una fuente abierta sin quien la atienda es la
maquina girando en el manejador para siempre. La misma operacion, dos
respuestas opuestas, y la diferencia es si existe un plan B.

### Memoria que un periferico pueda escribir

Un driver en EL0 no puede usar su monton para hablar con un chip, y por tres
motivos que no tienen nada que ver entre si:

**Tiene que estar SEGUIDA de verdad.** Al periferico se le da UNA direccion
fisica y un tamanyo; el no traduce nada. Un buffer de 64 KB que la MMU
presenta junto pero esta repartido en dieciseis paginas sueltas hace que el
DMA escriba en quince sitios que no son suyos. Y eso no da un error: da
corrupcion en un tercero, que es la peor clase de fallo que existe.

**No puede estar cacheada.** El periferico escribe la RAM por su cuenta, sin
pasar por las caches de la CPU. Si la pagina fuera cacheable, la CPU leeria
de su cache lo que el chip ya cambio en la RAM, o escribiria en la cache algo
que el chip nunca llega a ver. La alternativa es mantenimiento de cache a
mano en cada transferencia -limpiar antes de que lea el dispositivo,
invalidar antes de leer nosotros- y equivocarse una vez da un fallo que
aparece una de cada mil veces. No cachear es mas lento de acceder e imposible
de hacer mal.

El indice ya estaba puesto en el MAIR desde hace pasos, con su comentario:

```c
#define MT_NORMAL_NC     3   /* RAM sin cachear (DMA, por ejemplo) */
```

Escrito, explicado y sin usar hasta hoy.

**Y el driver tiene que saber DONDE ESTA en fisica**, que es lo unico de los
tres que no puede averiguar por su cuenta.

### Y ahi esta la pregunta incomoda del microkernel

Dar una direccion fisica es dar un poder, y no uno pequenyo: **el DMA no pasa
por la MMU**. Quien pueda escribir una direccion arbitraria en el registro de
DMA de un periferico puede escribir en cualquier sitio, incluido el kernel. La
proteccion de memoria, que es lo que sostiene todo lo demas, deja de valer.

En una maquina moderna eso lo ataja una IOMMU, que es literalmente una MMU
para perifericos: el chip tambien traduce, y solo ve lo que se le ha
concedido. La Pi 3 no tiene. Asi que la unica frontera posible es no darsela a
cualquiera, y la regla sale de una que ya existia:

```c
    if (!t->mmio_va) return -EPERM;
```

"Tienes un periferico concedido" es lo mismo que "eres un driver", y eso lo
decide el kernel al crear el proceso, y solo init puede pedirlo. Un programa
normal recibe `EPERM`, y `malo` lo comprueba.

No es una frontera bonita -un driver malicioso sigue pudiendo escribir en el
kernel por DMA- pero es la unica que este hardware permite, y conviene tenerlo
escrito en vez de descubrirlo el dia que algo raro pase.

### Lo que el chip contesto

La semilla no escribe ni un bit de control: encender un controlador sin saber
apagarlo es como se cuelga una placa. Solo lee los registros de identidad, que
es la unica forma de saber si la ventana da al sitio correcto.

```
  [usb] soy el pid 8; MMIO del DWC2 en 0x10000000
  [usb] GSNPSID = 0x4f54294a  -> Synopsys DWC2, version 2.94a
  [usb] GHWCFG2 = 0x250dc016  -> DMA interno, 8 canales de anfitrion
  [usb] DMA: 16 paginas en VA 0x18000000 -> PA 0x156000
  [usb] las 16 paginas se escriben y se releen bien
  [usb] el segundo tramo dice EBUSY, como debe
  [usb] IRQ 9 reclamada en el puerto 3
  [usb] la IRQ de la UART ya tiene duenyo: no me la da
  [usb] una IRQ fuera de la lista: no me la da
```

### Lo mismo en la Pi, que no dice lo mismo

Los registros de identidad son lo unico que este paso lee, y por eso es
interesante compararlos: QEMU modela el DWC2, pero no modela ESTE DWC2.

| | QEMU raspi3b | Pi 3B de verdad |
|---|---|---|
| `GSNPSID` | `0x4f54294a` -> 2.94a | `0x4f54280a` -> **2.80a** |
| `GHWCFG2` | `0x250dc016` | `0x228ddd50` |
| `GHWCFG3` | `0x10000044` | **`0x0ff000e8`** |
| `GHWCFG4` | `0x00000000` | **`0x1ff00020`** |

Lo que coincide es lo que importaba confirmar: **DMA interno y 8 canales de
anfitrion** en las dos. Lo que no coincide dice cosas utiles. La version es
otra -2.80a es la que lleva el chip- y `GHWCFG3` trae en la Pi un campo que
QEMU deja a cero: sus bits [31:16] son la profundidad de la FIFO de datos, y
`0x0ff0` son 4080 palabras, o sea **16.320 bytes de FIFO** para repartir entre
los ocho canales. Ese numero acota cuanto puede haber en vuelo a la vez y
hara falta en el paso que planifique transferencias.

Que QEMU conteste algo plausible pero distinto es la razon de que este paso
tenga que probarse en la placa: un driver escrito contra los numeros del
emulador habria dado por hecho una FIFO de cero bytes.

Dos datos de ahi deciden como habra que escribir el driver. **DMA interno**
quiere decir que el controlador lee y escribe la RAM el solo, o sea que lo que
acabamos de anyadir es exactamente lo que hacia falta. Y **ocho canales de
anfitrion** son todas las transferencias que puede tener en vuelo a la vez,
para todo lo que cuelgue del hub: ocho. De ahi sale que este controlador
necesite un planificador por software, que es la parte dificil que viene
despues.

### El fallo que encontro la prueba, y no estaba donde yo miraba

Para comprobar que las paginas de DMA vuelven al morir el driver, matamos el
proceso y miramos las paginas libres antes y despues:

```
  paginas libres : 245375
  kill 8 9
  paginas libres : 245375      <- no ha vuelto nada
```

Y no era el codigo de limpieza. Era que **`reap()` no llegaba a correr**: el
recolector no toca un zombi que tenga padre vivo -existe para que su padre lea
su codigo de salida- e init no esperaba a nadie mas que al interprete. Un
driver que se muriera se quedaba de zombi **para siempre**, con su imagen, su
pila, su ranura de tarea y ahora tambien su tramo de DMA.

Es el problema clasico del proceso 1, y tiene la solucion clasica: un init
entierra. Lo bonito es que las dos piezas que hacian falta llevaban dos pasos
puestas sin que se les hubiera visto el sentido: **SIGCHLD** para enterarse en
el acto (paso 54) y **PID_CUALQUIERA** para recoger sin saber a quien (paso
55), que es justo lo que hace falta aqui porque init no lleva una lista de sus
drivers. Y `SIG_REANUDAR`, porque init se pasa la vida dentro de un `waitpid`
bloqueante y no quiere que se le rompa cada vez que muere alguien.

Seis lineas en `init.c`:

```
  paginas libres : 245375
  kill 8 9
  paginas libres : 245405      <- treinta paginas
```

Treinta: las dieciseis de DMA, mas la imagen, la pila y las tablas de pagina.

### Lo que viene, y donde esta lo dificil

Lo que queda no es mas de lo mismo. El DWC2 de la Pi **no tiene planificador
hardware para las transferencias partidas** (*split transactions*), y todo lo
que no sea alta velocidad detras del hub hay que planificarlo por software,
micro-trama a micro-trama, con plazos de **125 microsegundos**. El driver de
la propia Fundacion lo hace desde una FIQ.

Aqui el tick del planificador son **10 milisegundos**: ochenta veces largo. Y
eso no es un numero que se suba, porque subirlo a 8 kHz para atender USB
significa interrumpir los cuatro nucleos ocho mil veces por segundo. Es justo
el motivo de que exista la FIQ y de que el driver de la Pi la use.

O sea que el paso que hace falta antes de mover un solo byte por USB no es de
USB: es una fuente de tiempo fina, separada del planificador. Y conviene
saberlo antes de empezar a escribir el otro.

## Un solo escritor, o el kernel dejando de ser duenyo de la UART

Esto lleva pendiente desde el paso 8, cuando el `conserver` demostro que un
driver puede vivir en EL0 hablandole al hardware directamente. Lo demostro, y
dejo el sistema con **dos** drivers sobre la misma PL011: el kernel con el
suyo y el conserver con el de verdad.

El README decia que se entrelazaban. Es peor: la Pi lo enseno al arrancar.

```
   ns[rse ]edvider de finhorasvivv  enEE00
MC segun la GPU: 200000000 Hz
```

De **158 caracteres salieron 71**. El 55% destruido, sin un solo mensaje de
error. No se entrelaza: se pierde.

### Por que se pierde, y no solo se mezcla

Las dos rutas de escritura son literalmente las mismas dos lineas. En el
kernel, `putc_raw`:

```c
    while (mmio_read(UART0_FR) & FR_TXFF) { }   /* esperar hueco */
    mmio_write(UART0_DR, (uint32_t)c);          /* escribir      */
```

Y en el conserver, `hw_putc`, con el comentario *"esperar hueco, igual que en
el kernel"*, que resulta ser mas literal de lo que queria decir.

Ese par no es indivisible. Los dos pueden ver hueco cuando queda **uno**, los
dos escriben, y la PL011 se queda con el primero y tira el segundo sin
avisar. A 115200 con una FIFO de transmision de 16 bytes, durante una rafaga
la FIFO esta llena casi todo el tiempo, asi que los dos se pelean
constantemente por el unico hueco que se libera cada 87 microsegundos. De ahi
que el 55% no sea mala suerte: es lo que toca.

El cerrojo compartido no es la salida, y esto ya estaba escrito: el cerrojo es
del kernel, un proceso de EL0 no puede cogerlo, y si se le diera forma de
hacerlo, un desalojo del planificador con el cerrojo cogido dejaria al kernel
girando. La salida es que haya **un** escritor.

### Ceder el dispositivo entero

Y el que tiene que serlo es el que tiene el dispositivo concedido. Asi que
cuando el conserver reclama la UART, el kernel se la da **entera**:

```c
    if (irq == IRQ_UART) uart_ceder();
```

Esa linea esta en `irq_register`, en el mismo sitio donde cambia de manos el
teclado, y eso no es comodidad: es un solo dispositivo. Quedarse la escritura
mientras otro se lleva la lectura es precisamente lo que llevaba cincuenta
pasos sin funcionar.

Desde ese momento el texto del kernel no va al hardware: va a un anillo de
4 KB. El conserver lo saca con una llamada nueva y lo imprime con su propio
escritor. `putc_raw` es el unico sitio donde hace falta decidirlo, porque es
el cuello de botella por donde pasa **todo** byte que el kernel manda al
cable.

De propina, el proyecto gana algo que no tenia: un sitio donde esta lo que el
kernel dijo. Antes se iba por el cable y no quedaba rastro.

### Dos clases de texto, y una se puede perder

Aqui es donde el diseno se complico, y el motivo es bueno.

Metida toda la salida en el anillo, un `cat` de un ELF de 13 KB lo desborda
tres veces. Un diagnostico del kernel que no cabe se puede tirar -es lo que
hace el `printk` de cualquier Unix- pero la salida de un proceso no: un `cat`
no puede salir con agujeros.

Y no son el mismo problema porque no se escriben desde el mismo sitio:

- El texto de un **proceso** entra por `consola_write`, o sea dentro de una
  llamada al sistema: alguien que se puede **dormir**. Asi que cuando el
  anillo se llena, **espera** a que lo vacien. Eso es control de flujo, y es
  lo que tiene cualquier capa de terminal de verdad. Es tambien lo que permite
  que el anillo sea pequenyo: con bloqueo bastan 4 KB; sin el harian falta
  tantos como el mayor `cat` que se le ocurra a nadie.
- El texto del **kernel** se escribe desde donde sea: un manejador de
  interrupcion, un volcado de fallo, el eco de una tecla. Ahi dormir no es una
  opcion, asi que si no cabe se cuenta y se tira.

Pero **se dice**. Perder texto es aceptable; perderlo en silencio es lo que
hacia la version de dos drivers y por eso costo cincuenta pasos verlo:

```
  [kernel] se han perdido 25 bytes de texto: el anillo se lleno
```

Se escribe cuando el anillo se queda vacio, que es el unico momento en que se
sabe que el aviso cabe.

### Y esos 25 bytes eran el eco

El aviso aparecio en la primera prueba de esfuerzo, y los 25 bytes tenian
nombre: eran el eco exacto de la orden que se teclo mientras el volcado
corria. Mientras la pantalla escupia 13 KB, las teclas no se veian.

El eco no puede esperar, y el motivo es bonito: lo produce el kernel **dentro
de la llamada con la que el conserver le entrega las teclas**. Bloquear ahi es
bloquear al unico proceso que puede vaciar el anillo. Un interbloqueo.

Si no puede esperar, se le guarda sitio:

```c
#define KLOG_RESERVA  256

static int klog_hueco(uint64_t cuantos)
{
    uint64_t libre = KLOG_SIZE - 1 - uart_klog_hay();
    return libre >= cuantos + KLOG_RESERVA;
}
```

Un cuarto de KB que el texto de los procesos no puede tocar. Un eco son unos
pocos bytes por tecla y ahi no se queda corto nunca; el que cede es el
volcado, que sabe esperar. Es la misma idea que reservar memoria para lo que
no puede fallar.

Con eso, 13.784 bytes por un anillo de 4.096 y **cero perdidas**, tecleando
por encima.

### El aviso, y por que llega por el reloj

Falta decirle al conserver que hay texto. Y no se puede hacer donde se
escribe, por el orden de cerrojos que `uart.h` lleva escrito desde el paso 13:
quien tiene el cerrojo de la UART no puede pedir `sched_lock`, y avisar por un
puerto lo pide. Escribir texto ocurre con el cerrojo de la UART cogido.

Asi que el aviso va en el **tick del reloj**, que es un sitio donde ya se
avisa por puertos -es lo que hace el reparto de interrupciones- y donde no hay
ningun cerrojo de UART cogido. El precio es hasta 10 ms de retraso para un
mensaje del kernel, que no se nota en algo que se lee con los ojos.

El eco no paga ese precio: el conserver vacia el anillo **justo despues** de
entregar las teclas, en la misma vuelta. Un eco con diez milisegundos de
retraso se nota al escribir; un diagnostico, no.

### panic() se lo queda todo

```c
void panic(const char *msg)
{
    uart_panico_toma_el_mando();
```

Un panico no puede depender de que un proceso siga vivo para contarlo: el
kernel se para ahi mismo, no vuelve a EL0, y el conserver no va a ejecutarse
nunca mas. Un panico que se quedara en el anillo seria una maquina muerta sin
decir por que, que es la unica cosa peor que morirse.

No es una excepcion sucia. Es la regla: lo ultimo que hace un sistema al
morirse lo tiene que poder hacer solo.

### Lo que esto NO arregla

Dos **procesos** escribiendo a la vez siguen entrelazandose entre si: `lento a
| lento b` saca las dos lineas trenzadas. Pero ya no es lo mismo, y la
diferencia importa: ahi no se pierde nada, solo se mezcla, porque los dos
pasan por el mismo anillo y el anillo tiene su cerrojo. Lo que falta es un
cerrojo por descriptor para que una escritura entera sea indivisible, y eso es
un paso aparte y mas pequenyo.

Y queda el kernel escribiendo en crudo durante el arranque, que es correcto:
cuando se imprime el rotulo no existe ni un proceso, y no hay con quien
competir.

## "Sin cubo" no quiere decir letra a letra

El paso 59 dejo la consola con un solo escritor y dejo una cosa a medias: dos
procesos escribiendo a la vez seguian mezclandose. Ya no se perdia nada -pasan
por el mismo anillo y el anillo tiene cerrojo- pero `lento a | lento b` salia
con las dos lineas trenzadas.

Fui a ponerle un cerrojo al descriptor de consola, que es lo que las
limitaciones decian que faltaba. Y antes de escribirlo mire por donde sale ese
texto, porque `lento` escribe a `stderr` a proposito. Esto es lo que habia:

```c
    if (f->modo & M_SINBUF)
        return escribir_todo(f->fd, &b, 1) < 0 ? -1 : c;
```

`M_SINBUF` -el `_IONBF` del estandar- estaba implementado como **un viaje al
kernel por byte**. Asi que un `fprintf(stderr, ...)` de cuarenta caracteres
eran **cuarenta llamadas al sistema**, y un cerrojo en el descriptor no habria
servido de nada: habria hecho indivisible cada letra, que ya lo era. El
trenzado no venia de que faltara un cerrojo; venia de que la linea llegaba al
kernel en cuarenta trozos, y entre dos de ellos cabe el otro proceso entero.

### Lo que el estandar quiere decir

"Sin cubo" no dice "cada byte va solo". Dice que al acabar cada operacion no
queda nada dentro. La diferencia es la que hay entre *"cuando termines,
entrega"* y *"entrega letra a letra"*, y se me habia colado la segunda.

Hace falta saber CUANDO acaba una operacion, y para eso un contador de
profundidad en el `FILE`: cada funcion publica que puede producir mas de un
caracter entra y sale, y el cubo se vacia al salir del todo. Un `fputc` suelto
sigue saliendo en el acto, porque para el la operacion acaba al volver.

Todo desemboca en `fputc`, asi que fueron cuatro sitios: `fwrite`, `fputs`,
`vfprintf` y `puts`. Y el `FILE` de `stderr` ya tenia su cubo de `BUFSIZ` ahi,
sin usar, desde que existe.

La medida, con la contraprueba hecha revirtiendo solo la libc:

```
  un fprintf a stderr (sin cubo): 41 llamadas a write()   <- antes
  un fprintf a stderr (sin cubo): 1 llamada a write()     <- ahora
```

Es la tercera vez en este proyecto que la velocidad sale de efecto secundario
de arreglar otra cosa. Aqui lo que se arreglaba era quien puede colarse en
medio de una linea.

### Y el cerrojo, que ahora si sirve

Con la linea llegando de una pieza, el cerrojo del descriptor hace lo que
prometia: una escritura entera es indivisible.

Es un **mutex** y no un spinlock, y no se podia elegir: dentro de la escritura
se puede dormir, porque si el anillo del paso 59 se llena hay que esperar a que
lo vacien. Un spinlock cogido mientras se duerme cuelga la maquina.

Que se pueda dormir teniendolo cogido obliga a comprobar una cosa, y es la
comprobacion que hay que hacer siempre con un cerrojo que duerme: que el que lo
tiene no dependa de que otro lo coja. Y no depende: el que vacia el anillo es
el `conserver`, que escribe por su cuenta en el hardware y nunca pasa por aqui.
Si pasara, esto seria un interbloqueo en la primera linea que imprimiera.

Dos detalles pequenyos que valen su comentario:

- El copiado desde el proceso se hace **antes** de coger el cerrojo, no dentro.
  Puede provocar un fallo de pagina, y un fallo de pagina con el cerrojo de la
  consola cogido es un camino nuevo por donde llegar a un interbloqueo.
- El mutex es un estatico y NO se inicializa: `mutex_init` no hace mas que
  poner a cero sus tres campos, que es en lo que nace un estatico. Un init
  perezoso habria sido una carrera entre nucleos por inicializar el cerrojo que
  van a usar para no pisarse, que tiene su gracia.

De paso cayo un cadaver: `syscall.c` declaraba un `console_mutex()` que no
estaba definido ni se usaba, resto de las demos que se llevo el paso 43.

### Una prueba que no medía nada

Escribi la prueba obvia -`lento 6 aaaa | lento 6 bbbb`- y salio con las lineas
enteras. Luego la corri en el commit anterior, y salio **igual de limpia**.

En QEMU el trenzado no se reproduce: los tiempos son demasiado gruesos y los
cuarenta viajes de una linea se hacen de una tirada sin que nadie desaloje al
proceso. O sea que la prueba no medía nada, como el trenzado del arranque del
paso 59: los dos son de la placa.

Lo que si se puede medir aqui es el MECANISMO -cuantas llamadas al sistema
cuesta un `fprintf`- y eso es determinista y falla cuando debe. El sintoma hay
que verlo en la Pi.

## El DWC2 se enciende, y una correccion que cambia el plan

Antes del codigo, una correccion mia. Habia escrito que el bloqueo para tener
red por USB eran los **125 microsegundos** de las transferencias partidas
frente a los 10 milisegundos del tick, y que hacia falta una fuente de tiempo
fina antes de mover un byte. No es verdad.

Las transferencias partidas (*split transactions*) existen para que un
anfitrion de alta velocidad hable con dispositivos de **baja o completa** a
traves de un hub: el hub hace la senyalizacion lenta y el anfitrion tiene que
colocar cada trozo en su micro-trama. El LAN9514 no es eso. Es un hub de alta
velocidad con la Ethernet colgada de un puerto **interno, tambien de alta
velocidad**, asi que se le habla con transferencias normales.

Y hay una comprobacion que lo zanja sin mirar ninguna hoja de datos: la Pi hace
unos 90 Mbit/s por Ethernet. Por un enlace de velocidad completa, que son 12,
eso es imposible.

Asi que para la red hacen falta transferencias de **control** y **bulk**, que
se atienden con una interrupcion por transferencia terminada y no con ocho mil
por segundo. Un driver en EL0 sirve. Los 125 microsegundos haran falta el dia
que se enchufe un teclado, que es de baja velocidad, y entonces el problema
sera de verdad.

### Poner el chip en un estado conocido

El firmware de la Pi deja el DWC2 encendido y a medio configurar. Heredar eso
es como se depura durante tres dias algo que funciona en un arranque y no en el
siguiente, asi que lo primero es un reset. El orden no es negociable:

1. **Quitar las puertas de reloj** (`PCGCCTL = 0`). Con el reloj cortado los
   registros contestan basura, y todo lo que venga despues seria un misterio.
2. **Cerrar la salida de interrupciones** mientras se configura. Todavia no hay
   nadie escuchando, y una fuente abierta sin manejador es el sistema girando
   en el vector.
3. **Esperar a que el bus AHB este quieto** antes de resetear. Resetear con una
   transferencia a medias deja el bus colgado, y con el medio chip.
4. **El reset**: se pide poniendo un bit y se sabe que acabo cuando el propio
   chip lo quita. No hay que quitarlo a mano.

Luego el modo anfitrion. Este controlador es OTG -puede ser las dos cosas y
decide mirando un pin- y en la Pi siempre es anfitrion, pero decirselo a mano
quita una variable: si el pin flotara, el chip se quedaria esperando a que
alguien le hable en vez de hablar el. Y el PHY: UTMI+ de alta velocidad, con
`PHYSEL` a cero, que es lo que significa "no me pongas el serie de velocidad
completa". Sin esa linea, la Ethernet no se veria nunca.

### El registro mas traicionero del chip

`HPRT0` es el puerto raiz, y tiene tres clases de bit mezcladas:

- de solo lectura: si hay algo conectado, a que velocidad, el estado de las
  lineas;
- de los que se **borran escribiendo un uno**: los avisos de "ha cambiado
  algo";
- y `PRTENA`, que leido dice si el puerto esta habilitado y **escrito con un
  uno lo deshabilita**.

O sea que un `leer, poner un bit, escribir` sobre este registro apaga el puerto
y de paso borra los avisos que ibas a leer. Es un clasico, y por eso toda
escritura pasa por una funcion de dos lineas que quita esos bits antes:

```c
#define HPRT_W1C  (HPRT_CONNDET | HPRT_ENA | HPRT_ENCHNG | HPRT_OVRCURCHNG)

static void hprt_escribir(uint32_t v) { escribir(HPRT0, v & ~HPRT_W1C); }
```

### Dos escalas de tiempo en la misma funcion

La espera por un bit acabo con dos fases, y el motivo es que aqui se juntan dos
escalas que no se parecen: el bus AHB contesta en **micro**segundos y un reset
de puerto USB dura decenas de **mili**segundos. Con un tick de 10 ms, dormir
para esperar lo primero es pasarse mil veces.

Asi que primero se mira a pelo unas cuantas veces -que resuelve todo lo rapido
sin dormir a nadie- y solo si eso no basta se pasa a dormir por ticks. Es en
pequenyo el problema del planificador de micro-tramas, y parte de la razon de
que ese no pueda vivir aqui.

Con tope, siempre: un bucle sin tope esperando un registro de hardware es la
forma mas comoda de colgar un driver.

### Lo que contesto

En QEMU, sin nada enchufado:

```
  [usb] nucleo en modo anfitrion; HPRT0 = 0x00001000
  [usb] no hay nada conectado al puerto raiz
```

`0x1000` es el bit 12: puerto alimentado y nada mas. Correcto, porque la
raspi3b emulada trae el controlador pero no trae nada conectado.

Asi que se le enchufa uno -`-device usb-storage`- y aparece el camino entero:

```
  [usb] nucleo en modo anfitrion; HPRT0 = 0x00021003
  [usb] algo conectado, velocidad completa (12 Mbit/s)
  [usb] tras el reset: HPRT0 = 0x0002100d, puerto habilitado
  [usb] micro-trama 7697: el bus esta vivo
```

Tres cosas de ahi valen la pena. El **bit 2 puesto** despues del reset dice que
el puerto quedo habilitado, o sea que el enmascarado de los W1C funciono: si
hubiera hecho un read-modify-write ingenuo, ese bit habria salido a cero y el
puerto apagado. El **contador de micro-tramas corriendo** dice que el bus tiene
reloj: el chip esta emitiendo *start of frame* cada 125 microsegundos por su
cuenta, que es el trabajo que un anfitrion hace sin que nadie le diga. Y la
**velocidad completa** es de QEMU; en la Pi, el LAN9514 tiene que decir alta.

### Y la placa dijo que no

En la Pi, con un LAN9514 soldado al puerto, salio esto:

```
  [usb] nucleo en modo anfitrion; HPRT0 = 0x00001400
  [usb] no hay nada conectado al puerto raiz
```

`0x1400` son dos bits: el 12, alimentacion del puerto, y el **10, que es el
estado de las lineas**. `PRTLNSTS = 01` quiere decir D+ alta y D- baja, que es
el reposo de un dispositivo enchufado con su resistencia en D+.

O sea que el hub estaba ahi y el PHY lo estaba viendo. Lo que no habia ocurrido
era que el controlador registrara la conexion, y eso ya no es un problema de
cobre: es nuestro. Ese campo es el diagnostico, y tiene gracia que sea justo el
que QEMU no modela -alli dice "las dos bajas" incluso con un pendrive
conectado-.

Dos cosas estaban mal, y las dos son de orden y de tiempo.

**La seleccion de PHY iba despues del reset.** El DWC2 exige un reset del nucleo
para que un cambio de PHY surta efecto, asi que escribirlo despues es no
escribirlo. Yo reseteaba primero y configuraba despues; la configuracion no
llegaba a aplicarse nunca. En QEMU daba igual porque su modelo no simula el
PHY, asi que el fallo solo existia en el cobre.

**Y no habia antirrebote.** El USB manda antirrebotar una conexion al menos 100
ms antes de darla por buena -TATTDB en la norma- porque un conector que entra
hace contacto varias veces. El controlador hace ese antirrebote por su cuenta y
hasta que acaba `PRTCONNSTS` sigue a cero. Yo miraba **una vez**, 20 ms despues
de dar corriente. En QEMU salia bien porque ahi no hay rebote que antirrebotar.

Las dos veces, el mismo patron: el emulador diciendo si a algo que el cobre dice
no. Y las dos veces el arreglo es correcto aunque no fuera la causa del
sintoma, que es la unica clase de arreglo que merece la pena hacer a ciegas.

### Lo que falta para hablar con el

Tener el puerto habilitado no es hablar. Lo siguiente es un canal, una
transferencia de control y un `GET_DESCRIPTOR`, que es donde el hub dira quien
es. Y ahi apareceran las cosas que este paso solo ha preparado: el buffer de
DMA del paso 58 -porque los descriptores llegan por DMA- y la interrupcion 9,
que hasta ahora esta reclamada y sin usar, porque `GINTMSK` sigue a cero y el
chip no ha pedido atencion ni una vez.

## Hablar con el otro lado

Tener el puerto habilitado no es hablar. Este paso hace la primera
conversacion: un canal, una transferencia de control y un `GET_DESCRIPTOR`,
que es la pregunta con la que todo dispositivo USB dice quien es.

### Lo que arreglo primero, porque estaba mal

**Los avisos que no se podian borrar.** El paso 61 puso `hprt_escribir()` para
que un leer-poner-escribir sobre `HPRT0` no borrara por accidente los bits que
se limpian escribiendo un uno. Funcionaba, y con eso solo resulta que tampoco
se podian borrar **a proposito**. La placa lo dijo con un `HPRT0` que acababa
en `f`, con las dos banderas de cambio encendidas y sin forma de bajarlas.

Hacen falta las dos funciones: una que proteja y otra que reconozca. Lo que no
puede haber es solo la primera. Fui demasiado listo en un sitio donde hacia
falta ser completo.

**Y las FIFO.** El nucleo tiene 4080 palabras que hay que repartir a mano entre
recepcion, transmision no periodica y periodica, y los valores por defecto de
este ejemplar no sirven. El reparto se escribe como "profundidad y donde
empieza", las tres zonas tienen que ir seguidas y sin solaparse, y si dos se
pisan las transferencias salen con datos de la otra: un sintoma que no se parece
en nada a la causa.

### La direccion que no es la direccion

Esto es lo que mas me gusta del paso, porque es un fallo que no da error.

El paso 58 consiguio memoria contigua y su direccion **fisica**, que es lo que
un periferico necesita porque el DMA no pasa por la MMU. Pero en esta placa la
CPU y los perifericos **no ven la RAM en el mismo sitio**: lo que para el ARM es
la direccion 0 es, para el bus de la GPU y para los maestros DMA que cuelgan de
el, la `0xC0000000`.

Hay cuatro alias del mismo byte de RAM -`0x00000000`, `0x40000000`,
`0x80000000` y `0xC0000000`- y se distinguen en como pasan por las caches de la
VideoCore. El de DMA es el ultimo, que es coherente con la L2.

```c
#define BUS(pa)   ((uint32_t)((uint64_t)(pa) | 0xC0000000UL))
```

Darle al chip la fisica a secas no da un error: da un DMA que escribe en otro
sitio. La transferencia dice que fue bien -y fue bien, en una direccion que no
es la tuya- y tu buffer sigue teniendo lo que tenia.

Y ahi esta el detalle que convierte una adivinanza en un diagnostico: **el
buffer se rellena con un patron antes de cada transferencia**. Si el descriptor
aparece, la direccion era buena. Si el patron sigue intacto y el chip dice que
todo fue bien, el DMA fue a otra parte. Sin el patron, "no ha contestado" y "ha
contestado en otro sitio" se ven exactamente igual, y son dos problemas que no
tienen nada que ver.

### El descriptor te dice como leer el descriptor

El primer `GET_DESCRIPTOR` pide **ocho bytes**, y no es timidez. Para leer un
descriptor hay que decirle al controlador cual es el tamanyo maximo de paquete
del dispositivo, y ese dato esta **dentro del descriptor**, en el byte 7.

Ocho es el minimo que la norma obliga a soportar a todo el mundo. Asi que con
ocho se lee lo justo para saber cuanto se puede leer, y despues se pide el
descriptor entero con el tamanyo que acaba de decir. Es circular y tiene salida,
que es lo bonito.

### Tres transferencias para una

Una transferencia de control son tres del canal: el SETUP dice que se pide, los
datos van o vienen, y el estado es un paquete vacio con el que el dispositivo
confirma.

Los PID no son decorativos. El SETUP va con PID de SETUP, y la fase de datos de
un control empieza **siempre** en DATA1, igual que el estado. Equivocarse ahi da
un `DATATGLERR`, que es el chip diciendo "esto no es el paquete que esperaba".

Y los errores del canal se traducen a palabras, porque con los nombres delante
un `HCINT` deja de ser un numero: `STALL` es "el dispositivo no entiende eso",
`XACTERR` es "no ha contestado o ha contestado mal", `BBLERR` es "ha hablado mas
de lo que le tocaba". Tres diagnosticos distintos que llevan a tres sitios
distintos.

De momento se espera **mirando**, no por interrupcion: `GINTMSK` sigue a cero.
Meter las interrupciones antes de que una transferencia funcione es depurar dos
cosas a la vez.

### Y contesto

En QEMU, con un dispositivo enchufado a mano:

```
  [usb] contesta: descriptor de 18 bytes, USB 1.10, paquete maximo 8
  [usb] es 0409:55aa, clase 9, 1 configuracion
  [usb] clase 9 es HUB, que es lo que tiene que ser
```

`0409:55aa` es el hub que QEMU pone por su cuenta. Lo que importa no es quien
sea: es que dos transferencias de control enteras fueron y volvieron, que el
descriptor se leyo bien -18 bytes, la version del USB, el tamanyo de paquete- y
que **la comprobacion del patron paso**, o sea que el DMA aterrizo en nuestro
buffer y la direccion de bus era la correcta.

En la Pi lo que hay ahi es un LAN9514, y tiene que decir `0424:9514` con clase
9. El `0424` es SMSC.

### Lo que falta

Ponerle una direccion con `SET_ADDRESS` -ahora mismo se le habla a la 0, que es
la que usa todo dispositivo recien reseteado-, leerle el descriptor de hub, y
encender sus puertos. Ahi es donde aparecera la Ethernet, en un puerto interno
del propio hub.

## Limitaciones conocidas

- `munmap` devuelve las paginas de datos pero no las tablas de nivel 3 que
  se crearon para mapearlas. Son 50 por cada 100 MB tocados salteado, y se
  recuperan al morir el proceso. Liberarlas exige contar cuantas entradas
  quedan vivas en cada tabla.

- El reloj arranca siempre en la misma base: dos sesiones seguidas empiezan
  a la misma hora, asi que un fichero de ayer puede parecer mas nuevo que
  uno de hoy. Lo cura un reloj de verdad, o que init guarde la hora al
  salir.
- No hay zonas horarias. El reloj esta en hora local porque FAT lo esta, y
  el sistema no sabe cual es.
- No hay `stat` sobre un descriptor abierto (`fstat`), ni permisos, ni
  duenyo: FAT no los guarda.

- La unica frontera de privilegio entre procesos es "eres init o no eres
  init". Sin usuarios, sin grupos y sin capacidades.
- `/etc/rc` solo entiende `NOMBRE=valor` y comentarios: no hay ordenes ni
  condiciones. Un init de verdad ejecuta un guion.
- `sched_lock` es un cerrojo grande: protege la tabla de tareas, las colas
  de espera y los puertos IPC a la vez. Partirlo seria mas rapido, no mas
  correcto. (Lo que si se saco de el es la carga de un proceso, que son
  milisegundos: `task_create_user()` reserva la ranura con el cerrojo, carga
  sin el y publica con el otra vez.)
- `vi` no sabe DESHACER. Sin `u`, un `dd` en la linea equivocada es
  definitivo, y lo unico que hay entre tu y el desastre es que `:q` se niegue
  a salir con cambios. El vi original tenia un nivel, uno solo.
- `vi` no se puede suspender: se traga el Ctrl-Z, porque pararse dejaria el
  terminal en modo crudo y sin eco y el shell escribiendo a ciegas. Hacerlo
  bien es devolver el terminal al pararse y recuperarlo con SIGCONT.
- `vi` no sabe el tamanyo del terminal. No hay `ioctl` ni `TIOCGWINSZ`, asi
  que da por hecho 80x24 y se deja corregir con las variables `LINES` y
  `COLUMNS`. Un terminal de verdad se lo diria, y ademas avisaria por
  `SIGWINCH` cuando cambiara.
- `vi` guarda el fichero como un array de lineas, no como un "gap buffer".
  Insertar un caracter mueve media linea en vez de mover un hueco, y son 8192
  lineas de 1024 caracteres como mucho. Para ficheros de megabytes habria que
  cambiar la estructura, no el codigo.
- `vi` no tiene contadores (`3dd`), ni copiar y pegar, ni `J`, ni `r`, ni
  `cw`, y la busqueda es texto tal cual: sin expresiones regulares.
- Un mensaje de la linea de estado se queda puesto hasta que algo lo tape. El
  modo no -eso se corrige siempre-, pero un "escrito /nota.txt" sigue ahi
  mientras te mueves. Es lo que hace vi.
- El shell no tiene historial, ni tuberias de mas de dos, ni `2>`.
  Redirigir stderr pide poder nombrar el descriptor de destino (`2>&1`), y
  eso es una sintaxis nueva, no una llamada nueva.
- `SIGTTOU` existe como numero pero no se genera nunca: un proceso de
  segundo plano que escribe ensucia la pantalla y nadie lo para. Unix hace
  lo mismo salvo que se lo pidas con `stty tostop`, y el motivo es que
  escribir desde el fondo es molesto pero no te quita nada; leer, si.
- Las teclas de control siguen escritas a mano: Ctrl-C y Ctrl-Z en los dos
  drivers, y el resto en la disciplina de linea. En Unix son una tabla del
  terminal que se puede cambiar con `stty`; aqui son constantes.
- La edicion de linea es un backspace y un Ctrl-U. No hay historial, ni
  mover el cursor, ni borrar una palabra. Todo eso vive en la misma
  disciplina y son mas casos del mismo `switch`, no un mecanismo nuevo.
- El terminal es UNO y global: un solo modo, una sola linea a medias, un
  solo buffer. En Unix cada terminal tiene el suyo, y lo que aqui es una
  variable estatica alli cuelga del descriptor. Con una UART y una
  consola, la diferencia no se ve.
- La linea son 128 caracteres y lo que pase de ahi se descarta en
  silencio. Un terminal de verdad pita.
- No hay `ICRNL` ni `ONLCR` configurables: el `\r` del terminal se
  convierte siempre en `\n` al entrar, y el `\n` en `\r\n` al salir.
  Estan bien para este cable y no se pueden apagar.
- `T_ECO` y `T_CANONICO` se guardan y se reponen a mano. Si un programa
  muere entre las dos llamadas -un Ctrl-C mientras pide la contrasenya-
  el terminal se queda como lo dejo. Un Unix tampoco lo arregla solo:
  por eso existe el `stty sane` que todos hemos tecleado a ciegas.
- La contraprueba de `anyadir` depende del tiempo: pierde 19 lineas de 60
  en QEMU y 7 en la Pi, con la misma ventana. Si algun dia dejara de
  perder ninguna, no seria que el `lseek` se ha arreglado, seria que la
  prueba ha dejado de medir.
- No hay sesiones, ni proceso lider, ni `SIGHUP`. Con un solo terminal y
  un solo shell, una sesion seria una etiqueta que no distingue nada.
- Dos procesos que escriban a la vez en la consola ya no se trenzan letra a
  letra: desde el paso 60 una escritura entera es indivisible y un `fprintf`
  entero es una sola escritura. Lo que SI puede pasar es que se mezclen por
  LINEAS -el orden entre dos procesos no esta garantizado- y que una linea de
  mas de 128 caracteres se parta, porque `consola_write` acepta 128 por llamada
  y la libc da la vuelta. Subir ese tope es subir un buffer que vive en la pila
  del kernel, que es una pagina.
- El texto del KERNEL si se puede perder: un diagnostico se escribe desde
  sitios donde no se puede dormir -un manejador de interrupcion, el eco de una
  tecla- asi que si el anillo esta lleno se tira. Lo dice cuando pasa
  (`se han perdido N bytes`), y hay 256 bytes reservados para que el eco no
  compita con un volcado.
- Un mensaje del kernel puede tardar hasta 10 ms en salir: el aviso al duenyo
  de la consola va en el tick del reloj, porque avisar donde se escribe
  romperia el orden de cerrojos. El eco no paga ese precio -el conserver vacia
  el anillo en la misma vuelta en que entrega las teclas- pero un `[kernel]`
  suelto, si.
- El anillo del kernel son 4 KB y no se puede leer desde un programa: solo lo
  saca quien tiene la UART. No hay `dmesg`, aunque ya hay donde ponerlo.
- No hay `WCONTINUED`: "ha seguido" no es un suceso que nadie observe, asi
  que un `kill -18` a mano deja la lista diciendo "parado" de algo que corre.
  Los unicos SIGCONT de aqui los manda el propio shell con `fg` y `bg`, y
  esos si los apunta. Y no se puede arreglar como las paradas, porque
  reanudarse es que el planificador te elija, no un sitio por el que se pase.
- `parada_avisada` es un bit del HIJO y no del que pregunta, igual que en
  Unix. Con un padre por hijo da igual; si dos pudieran esperar al mismo,
  solo uno se enteraria de la parada.
- `esperar` con `PID_CUALQUIERA` no sabe elegir por grupo. Unix tiene
  `waitpid(-pgid)` -"cualquiera de ese trabajo"- y aqui el signo ya esta
  gastado en distinguir el pid de "cualquiera", asi que harian falta
  banderas. Con trabajos de dos procesos como mucho, no hace falta.
- No hay `sigprocmask`: una senyal no se puede bloquear, solo atrapar o no.
  El shell lo suple con una variable que apaga su propio manejador mientras
  espera en primer plano, y el precio es que esa senyal se pierde. Funciona
  porque hay una red detras -el `recoger()` del prompt- y no porque sea
  equivalente.
- `SIG_REANUDAR` es la unica bandera que hay, y no hay `SIG_IGN`: para
  ignorar una senyal hay que atraparla con un manejador que no haga nada, que
  no es lo mismo -un manejador vacio interrumpe las llamadas bloqueadas, y
  `SIG_IGN` no llega ni a molestar-. El shell se apoya en esa diferencia a
  proposito para Ctrl-C y Ctrl-Z, pero no siempre sale a favor: un programa
  que quisiera ignorar una senyal sin que se le rompan las lecturas no puede.
  Tampoco hay `sigaction` de verdad, ni `sa_mask`, ni `SIGINFO`.
- El manejador de `SIGTSTP` del shell se HEREDA en el fork y solo se borra en
  el exec, asi que hay una ventana de unos microsegundos -entre bifurcarse y
  convertirse en el programa- en la que un hijo se tragaria un Ctrl-Z en vez
  de pararse. bash pone los manejadores en su sitio en el hijo justo despues
  del fork; aqui no, porque para alcanzarla habria que pulsar la tecla dentro
  de esa ventana.
- Se reanuda solo la llamada que devolvio `-EINTR`, y solo reponiendo x0. Con
  las llamadas de ahora basta, porque el kernel no pisa ningun otro
  argumento; el dia que alguna escriba en x1 habria que guardar mas.
- `strtol` sigue sin detectar desbordamiento, aunque ya hay `ERANGE` donde
  ponerlo.
- `errno` es una variable global y no una por hilo. Con un solo hilo por
  proceso da igual; el dia que haya hilos dentro de un proceso, no.
- `printf` entiende banderas, anchura, precision y `l`, pero no notacion
  exponencial (`%e`, `%g`) ni `long double`.
- El cambio de contexto de FP es perezoso solo al restaurar. Al salir se
  salva siempre, porque con cuatro nucleos dejar el estado vivo en los
  registros de otro nucleo exigiria IPIs (ver "Coma flotante").
- Una transaccion con el servidor de ficheros cada vez. Hay un solo puerto
  de respuesta y las respuestas no dicen a quien pertenecen, asi que un
  mutex las serializa. La tarjeta es un solo dispositivo de todas formas,
  pero el limite es del mecanismo, no del hardware.
- Dos escrituras con `O_ANYADIR` de mas de 240 bytes desde descriptores
  DISTINTOS no se pierden, pero pueden entrelazarse: cada mensaje se
  coloca al final por su cuenta, asi que una linea larga puede salir
  partida con otra en medio. El cerrojo de `struct fichero` no llega ahi
  -son dos descripciones distintas- y el servidor no sabe que esos tres
  mensajes eran uno. Un Unix de verdad los mete bajo el cerrojo del inodo.
- No hay `O_CREAT | O_EXCL`, que es la apertura que necesita ser
  indivisible de verdad: "creamelo solo si no existe". `file_open` con
  `O_ANYADIR` pregunta el tamanyo y, si no esta, lo crea, y entre las dos
  peticiones cabe otro. Para anyadir da igual -el que pierde se lo
  encuentra hecho- pero es exactamente por lo que en Unix eso es una
  bandera del `open` y no dos llamadas.
- Un descriptor abierto para leer no comprueba que no sea un directorio:
  `file_open` con `O_LEER` acepta el `FS_OK` de un `FS_SIZE` sin mirar
  `FS_ES_DIR`. El fallo aparece despues, al leer, y dice lo que no es.
- El buffer de teclas se queda en el kernel aunque el driver este fuera.
  Es deliberado (ver "El teclado, tambien en EL0"), pero significa que el
  kernel sigue sabiendo que es una consola.
- La lista de interrupciones que un proceso puede reclamar sigue escrita a
  mano en `irq_register()` -ahora son dos, la UART y el USB, y caben cuatro a
  la vez-. Un sistema serio la sacaria de un arbol de dispositivos, donde cada
  periferico dice que IRQ usa.
- No hay IOMMU, y por tanto no hay proteccion contra un driver que haga DMA
  donde no debe: una direccion fisica es la llave para saltarse la MMU, porque
  el DMA no pasa por ella. La unica frontera es a quien se le da -solo a quien
  ya tiene un periferico concedido- y eso protege de los programas normales,
  no de un driver equivocado.
- Un tramo de DMA por proceso, y hasta 4 MB. Si un driver necesita varios
  buffers, reparte el suyo, que es como funciona un "DMA pool" de verdad.
- La memoria de DMA se pide y no se devuelve hasta que el proceso muere: no
  hay `dma_free`. Un driver la pide al arrancar y la tiene para siempre, que
  es lo que hace un driver, pero no es una regla que el kernel imponga.
- El tick son 10 ms, y las transferencias partidas del USB piden 125 us. Eso
  NO bloquea la red -el LAN9514 es de alta velocidad y no usa transferencias
  partidas- pero si bloquea cualquier dispositivo de baja o completa velocidad
  enchufado por fuera: un teclado USB necesita una fuente de tiempo fina y
  separada del planificador, y el driver de la Fundacion usa una FIQ para eso.
- El driver de USB no pide ninguna interrupcion todavia: `GINTMSK` esta a cero
  y se pregunta mirando los registros. La IRQ 9 esta reclamada y sin usar.
- El puerto raiz de la Pi enumera a velocidad COMPLETA y deberia ser alta: el
  LAN9514 es un hub de alta velocidad, asi que el *chirp* del reset no esta
  saliendo. No bloquea nada -control y bulk funcionan igual, y con todo el bus
  a velocidad completa tampoco hacen falta transferencias partidas- pero la red
  ira a 12 Mbit/s en vez de 480. Los sospechosos son la anchura del UTMI+ y el
  tiempo de turnaround.
- Solo se usa el canal 0, y de uno en uno. Hay ocho, y usarlos a la vez es lo
  que hara falta el dia que haya varias transferencias en vuelo.
- Un solo dispositivo, en la direccion 0, y sin `SET_ADDRESS`: se le habla a la
  que usa todo dispositivo recien reseteado. Con un hub por delante eso deja de
  valer en el paso siguiente.
- Los mensajes del driver de USB salen DESPUES del prompt del shell, porque
  encender el puerto y resetearlo lleva mas de los 100 ms que init espera. Las
  lineas salen enteras -de eso se encargan los pasos 59 y 60- pero llegan
  tarde. Un Linux hace lo mismo: la enumeracion del USB aparece despues del
  login.
- El servidor entiende FAT16 y FAT32, pero nada de FAT12 ni exFAT, y
  escribe los nombres en 8.3.
- El entorno son 16 variables y 512 bytes de texto por proceso, en un
  array fijo: un entorno que crece sin limite necesita un asignador
  detras, y el asignador necesita el entorno para saber donde esta el
  monton.
- No hay `unset`, ni variables locales del shell (todo lo que se pone se
  exporta), ni `${VAR}` con llaves.
- Los montajes estan escritos a mano: la particion FAT32 es `/` y la
  FAT16 es `/boot`, y no hay `mount` ni `/etc/fstab`. Con dos particiones
  y una placa concreta, una tabla de dos entradas dice mas que un
  mecanismo general sin usar.
- Un punto de montaje se inventa al listar su padre, asi que no se puede
  borrar ni entrar en el con `cd ..` desde dentro esperando encontrar una
  entrada de verdad.
- No hay borrado en cascada (`rm -r`), a proposito: es facil de escribir y
  dificil de deshacer.
- `mv` no cruza particiones, porque ahi dejaria de ser un renombrado. Hay
  que copiar con `cp` y borrar.
- No se lleva la cuenta de clusters libres del FSInfo de FAT32: se marca
  como desconocida y que la recalcule quien la quiera.
- La ruta son 256 bytes; cada componente puede llevar nombre largo.
- El anillo de entrada de la consola son 64 bytes y lo que no cabe se
  pierde. Ahora al menos lo dice; un terminal de verdad tendria control de
  flujo (XON/XOFF o RTS/CTS) y no perderia nada.
- Los ficheros mapeados son de solo lectura: no hay nada que devuelva los
  cambios al disco. Un proceso puede tener cuatro a la vez.
- Llenar una pagina mapeada son 24 mensajes al servidor (176 bytes cada
  uno). Funciona, y es lento.
- `copy_from_user` y `copy_to_user` copian byte a byte. Las de verdad
  mueven palabras enteras y tienen varias entradas en la tabla, una por
  cada tamanyo de acceso.
- Ya no hay menu de depuracion en el kernel: si init no arranca, no hay
  forma de interactuar con la maquina.
- El nombre corto que acompanya a uno largo se busca probando `~1`, `~2`…
  y mirando el directorio entero en cada intento. Con muchas colisiones es
  lento, y el numero se come las letras.
- Un directorio no crece: si se queda sin huecos seguidos para una cadena
  VFAT, no se puede crear el fichero aunque quede sitio en el disco.
- De los nombres largos solo se entiende el ASCII. Lo de fuera sale como
  '?', a proposito: un byte truncado al azar daria un nombre que parece
  bueno y no abre nada.
- No hay escapes (`\ `) ni comillas dentro de comillas.
- Un programa puede recibir 16 argumentos y 256 bytes de texto entre
  todos.
- No hay diario ni nada que se le parezca: un corte de corriente a mitad de
  una escritura deja el volumen inconsistente, como en 1980.
- La linea de ordenes son 128 caracteres, asi que `write` no puede crear
  ficheros de mas de un centenar de bytes. Para mover volumen esta `cp`.
- Un mensaje lleva 256 bytes, de los que 176 son datos utiles, asi que
  cargar un programa de 12 KB son unas 70 idas y venidas por el IPC.
  Funciona y se nota.
- El monton busca el primer hueco que valga, recorriendo la lista: es O(n)
  y basta a esta escala. Lo siguiente serian listas por tamanyos.
- El monton del kernel nunca le devuelve paginas al PMM. Crece y no encoge.
  El de un proceso si sabe encoger, con `sbrk` negativo, pero `free()` no lo
  usa nunca: haria falta saber que el trozo liberado esta justo en el tope.
- `pmm_alloc_contig()` busca n paginas seguidas recorriendo el bitmap, asi
  que se vuelve lenta si la memoria se fragmenta. Cuando duela, lo que hay
  que traer es un asignador por compañeros ("buddy").
- La pila de kernel sigue siendo de una pagina: ahora un desbordamiento se
  caza al instante, pero se caza. Darle mas de una pagina es cambiar un
  numero; darle paginacion bajo demanda como a la de usuario es mas
  delicado, porque el fallo llegaria estando ya dentro del kernel.
- Un `FILE` que escribe a una **tuberia** se llena entero antes de soltar
  nada, porque una tuberia no es un terminal. En un filtro interactivo eso
  se nota: `upper` sigue leyendo y escribiendo el descriptor a pelo justo
  por eso, y no porque se nos olvidara cambiarlo. La libc de verdad tiene
  `setvbuf` para poder decidirlo desde fuera; aqui no lo hay.
- `fopen` entiende `"r"`, `"w"` y `"a"`, pero no `"r+"` ni `"a+"`, ni hay
  `setvbuf` ni `scanf`. Los modos mixtos son una decision -un cubo que se
  usa en las dos direcciones tiene que saber en cual se uso la ultima vez
  y vaciarse al cambiar de sentido, que es donde mas se equivoca todo el
  mundo-; `scanf` es que aun no ha hecho falta.
- `ungetc` acepta **uno** solo, y solo si antes se leyo algo del cubo. Es lo
  que garantiza el estandar, y mas de uno obligaria a un buffer aparte.
- El monton de un proceso se mapea entero al pedirlo: `sbrk` es ansioso.
  Podria ser perezoso como la pila, y dar las paginas segun se tocaran.
- El contador de referencias es un byte por pagina del mapa entero: 258 KB
  de `.bss` para algo que casi siempre vale 1. Una estructura dispersa
  ahorraria memoria a cambio de bastante mas codigo.
- Una pagina no puede compartirse mas de 255 veces. Con `MAX_TASKS` en 24
  no es un limite alcanzable, pero esta ahi.
- Una tuberia son 1 KB y dos extremos: no hay `|` de tres programas
  seguidos, ni redireccion a ficheros (`>` y `<`), que necesitaria que el
  shell pudiera abrir uno.
- Un zombi cuyo padre nunca lo espera se queda ahi hasta que el padre
  muere. Es exactamente el problema que tienen los Unix de verdad.
- No se anidan: mientras se atiende una, las demas esperan. Y no hay
  mascaras ni `sigaction`, solo un manejador por senyal.
- El planificador no tiene ni prioridades ni afinidad: una tarea se ejecuta
  en el primer nucleo que la mire. Es una eleccion, no un olvido — con esta
  carga no hay nada que priorizar.
- El kernel conserva su propio driver de UART, asi que cuando el servidor de
  consola esta activo hay DOS drivers sobre el mismo hardware y el texto se
  entremezcla. Y esto no es un problema de cerrojos: el `conserver` vive en
  EL0 con la PL011 mapeada en su espacio y escribe en ella directamente, sin
  pasar por nada del kernel — que es justamente lo que demuestra el paso 8.
  Ponerle un cerrojo compartido significaria que el kernel se quedaria
  girando cuando un proceso de usuario fuera desalojado teniendolo.

  La respuesta de un microkernel estricto es otra: que el kernel no tenga
  driver. Solo una salida de panico que escriba a pelo, y todo lo demas por
  mensajes al servidor. Eso implica que el menu y las demos de `kernel.c`
  dejen de ser codigo de kernel y pasen a ser un proceso de usuario, que es
  una reforma del proyecto entero y no un arreglo.
