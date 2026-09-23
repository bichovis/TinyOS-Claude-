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
                 stdlib.c    exit, atoi, abs
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

## Limitaciones conocidas

- La unica frontera de privilegio entre procesos es "eres init o no eres
  init". Sin usuarios, sin grupos y sin capacidades.
- `/etc/rc` solo entiende `NOMBRE=valor` y comentarios: no hay ordenes ni
  condiciones. Un init de verdad ejecuta un guion.
- `sched_lock` es un cerrojo grande: protege la tabla de tareas, las colas
  de espera y los puertos IPC a la vez. Partirlo seria mas rapido, no mas
  correcto. (Lo que si se saco de el es la carga de un proceso, que son
  milisegundos: `task_create_user()` reserva la ranura con el cerrojo, carga
  sin el y publica con el otra vez.)
- El shell no tiene historial, ni segundo plano, ni tuberias de mas de dos,
  ni `>>`: lee, carga, arranca y espera.
- La libc no tiene `errno` ni ficheros con buffer (`FILE`, `fopen`): se
  trabaja con descriptores. `printf` entiende banderas, anchura, precision
  y `l`, pero no notacion exponencial (`%e`, `%g`) ni `long double`.
- El cambio de contexto de FP es perezoso solo al restaurar. Al salir se
  salva siempre, porque con cuatro nucleos dejar el estado vivo en los
  registros de otro nucleo exigiria IPIs (ver "Coma flotante").
- Una transaccion con el servidor de ficheros cada vez. Hay un solo puerto
  de respuesta y las respuestas no dicen a quien pertenecen, asi que un
  mutex las serializa. La tarjeta es un solo dispositivo de todas formas,
  pero el limite es del mecanismo, no del hardware.
- Un descriptor de fichero no comparte el desplazamiento entre padre e
  hijo. En Unix `fork` duplica el descriptor y los dos avanzan el MISMO
  offset; aqui `file_dup` solo sube el contador y el offset es de la
  estructura, asi que dos procesos que escriban en el mismo fd se pisan.
- El buffer de teclas se queda en el kernel aunque el driver este fuera.
  Es deliberado (ver "El teclado, tambien en EL0"), pero significa que el
  kernel sigue sabiendo que es una consola.
- Solo se puede pedir la interrupcion de la UART. La lista de fuentes que
  un proceso puede reclamar esta escrita a mano en `irq_register()`; un
  sistema serio la sacaria de un arbol de dispositivos.
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
- La ruta son 64 bytes y cada componente 8.3, o sea unos cinco niveles.
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
- Solo `exec` y `spawn` usan la copia que sabe fallar. Las demas llamadas
  siguen comprobando el rango a mano y trayendo las paginas antes.
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
- Los ficheros nuevos no llevan fecha. FAT tiene campos para ella, pero la
  Pi no tiene reloj de tiempo real y no hay de donde sacarla: mejor un cero
  honesto que una fecha inventada.
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
