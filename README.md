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
                 conserver.c driver de la UART en EL0, sirve el puerto 0
                 client.c    imprime mandando mensajes al servidor
    tools/       bin2c.py           binario de usuario -> array de C
                 fetch-firmware.sh  baja el firmware de Broadcom para la SD
    config.txt   lo que la GPU lee antes de arrancar la CPU
    switch.S     cambio de contexto (solo registros callee-saved)
    pmm.c        reparte la RAM en paginas de 4 KB (bitmap)
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

## Limitaciones conocidas

- `sched_lock` es un cerrojo grande: protege la tabla de tareas, las colas
  de espera y los puertos IPC a la vez. Partirlo seria mas rapido, no mas
  correcto. (Lo que si se saco de el es la carga de un proceso, que son
  milisegundos: `task_create_user()` reserva la ranura con el cerrojo, carga
  sin el y publica con el otra vez.)
- La entrada de consola sigue siendo del kernel: `SYS_read` la sirve, pero
  no hay un servidor de teclado como lo hay de pantalla.
- El shell no tiene tuberias, ni redireccion, ni historial, ni segundo
  plano: lee, carga, arranca y espera.
- El servidor de ficheros solo LEE, solo entiende FAT16 y solo mira el
  directorio raiz. Nada de escribir, nada de FAT32, nada de subdirectorios.
- El reloj base del EMMC esta puesto a mano (41.666 MHz, el de la placa).
  Lo suyo seria preguntarselo a la GPU por el buzon, pero el buzon es del
  kernel y el driver vive en EL0.
- Un mensaje lleva 48 bytes, asi que cargar un programa de 4 KB son 86
  idas y venidas por el IPC. Funciona y se nota.
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
