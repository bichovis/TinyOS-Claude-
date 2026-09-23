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
                 umalloc.c   malloc/free de usuario, encima de sbrk
                 deep.c      recursion honda: se come la pila a proposito
                 conserver.c driver de la UART en EL0, sirve el puerto 0
                 client.c    imprime mandando mensajes al servidor
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
- El servidor de ficheros entiende FAT16 y solo mira el directorio raiz:
  nada de FAT32 ni de subdirectorios.
- No hay diario ni nada que se le parezca: un corte de corriente a mitad de
  una escritura deja el volumen inconsistente, como en 1980.
- Los ficheros nuevos no llevan fecha. FAT tiene campos para ella, pero la
  Pi no tiene reloj de tiempo real y no hay de donde sacarla: mejor un cero
  honesto que una fecha inventada.
- La linea de ordenes son 128 caracteres, asi que `write` no puede crear
  ficheros de mas de un centenar de bytes. Para mover volumen esta `cp`.
- El reloj base del EMMC esta puesto a mano (41.666 MHz, el de la placa).
  Lo suyo seria preguntarselo a la GPU por el buzon, pero el buzon es del
  kernel y el driver vive en EL0.
- Un mensaje lleva 48 bytes, asi que cargar un programa de 4 KB son 86
  idas y venidas por el IPC. Funciona y se nota.
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
