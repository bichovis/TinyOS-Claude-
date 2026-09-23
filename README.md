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
                 header.S    la cabecera que lee el cargador
                 hello.c     usa syscalls directas
                 conserver.c driver de la UART en EL0, sirve el puerto 0
                 client.c    imprime mandando mensajes al servidor
    tools/       bin2c.py           binario de usuario -> array de C
                 fetch-firmware.sh  baja el firmware de Broadcom para la SD
    config.txt   lo que la GPU lee antes de arrancar la CPU
    switch.S     cambio de contexto (solo registros callee-saved)
    pmm.c        reparte la RAM en paginas de 4 KB (bitmap)
    vmm.c        tablas de traduccion de 3 niveles y espacios de usuario
    smp.c        despierta los nucleos 1-3 y les manda trabajo
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

El kernel recibe un binario **plano**: una tira de bytes sin secciones ni
simbolos, porque `objcopy` se los ha comido. Mirandolo no hay forma de saber
donde acaba el codigo y empiezan los datos — y esa diferencia es justo la
que decide los permisos de cada pagina.

Asi que el programa lo dice de su puno y letra. Los primeros 48 bytes de
toda imagen son una cabecera (`include/user_abi.h`, emitida por
`user/header.S`) con las direcciones que el enlazador conoce y el kernel no:

    [text_start, text_end)   solo lectura, ejecutable   <- de la imagen
    [text_end,   data_end)   lectura/escritura          <- de la imagen
    [data_end,   bss_end )   lectura/escritura          <- ceros

El corte entre el primer tramo y el segundo esta alineado a 4 KB en
`user/user.ld`, y no por estetica: una pagina no puede ser medio ejecutable.

Con esto un programa de usuario ya puede tener variables globales. Antes no:
la imagen entera se mapeaba de solo lectura, asi que escribir en `.data` era
un fallo de permisos y `.bss` ni siquiera estaba mapeada. Y a cambio se gana
W^X de verdad — `user/hello.c` lo enseña por los dos lados, escribiendo en
sus globales y muriendo si toca su propio codigo.

Un detalle del que es facil no darse cuenta: `__data_end` en `user.ld` NO se
alinea. Ese simbolo marca el ultimo byte que `objcopy` escribe en la imagen,
y si se redondea, la cabecera promete mas bytes de los que hay.

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

## `wfe` y no `wfi`

Un detalle de una linea que resulto ser todo el reparto de trabajo.

La tarea idle de cada nucleo esperaba con `wfi`, que solo despierta con una
interrupcion: un nucleo ocioso tardaba hasta un tick entero en enterarse de
que habia una tarea lista. Con `wfe` despierta ademas con `sev` — y
`spin_unlock()` ya hace `sev`. Asi que cada vez que alguien suelta el
cerrojo del planificador, y eso incluye cada vez que una tarea pasa a lista,
los nucleos ociosos se despiertan solos y van a buscar trabajo.

Un `sev` de mas cuesta una vuelta del bucle idle sin encontrar nada. Un
`sev` de menos cuesta 10 ms de un nucleo parado.

Lo encontro el comando `w`, que empezo a dar el resultado exacto sin
cerrojo — imposible si hubiera carrera. Ahora imprime en que nucleo corrio
cada martillo, que es lo que delato el problema:

    sin cerrojo : (nucleos: 3 3 3 3 ) 800000     <- los cuatro en el mismo
    sin cerrojo : (nucleos: 3 0 1 2 ) 289604     <- ya repartidos

Un dato compartido no se corrompe por compartirlo: se corrompe por
compartirlo AL MISMO TIEMPO. De ahi tambien la barrera de salida de los
hilos martillo, sin la cual cada uno terminaba antes de que arrancara el
siguiente.

(Esos 289604 son de una Pi 3B: se pierde el 64% de los incrementos. QEMU
deja unos 403000, porque solapa menos. El hardware siempre es mas duro con
este error.)

## Limitaciones conocidas

- Despertar a los nucleos ociosos con `sev` es un martillazo: lo reciben los
  cuatro cada vez que alguien suelta el cerrojo del planificador, tengan o
  no algo que hacer. Lo fino seria un IPI por los mailboxes locales del
  BCM2837 (`0x4000008C + 0x10*core`), dirigido a UN nucleo ocioso.
- `sched_lock` es un cerrojo grande: protege la tabla de tareas, las colas
  de espera y los puertos IPC a la vez, y se mantiene cogido durante la
  carga entera de un proceso. Partirlo seria mas rapido, no mas correcto.
- El planificador no tiene ni prioridades ni afinidad: una tarea se ejecuta
  en el primer nucleo que la mire.
- El cerrojo de la UART hace indivisible cada LLAMADA, no cada linea: dos
  `uart_puts` no se entrelazan, pero un `uart_puts` seguido de un
  `uart_dec` si puede partirse. Con cuatro nucleos escribiendo se nota. Para
  lineas enteras habria que sostener el cerrojo desde fuera.
- El kernel conserva su propio driver de UART para depuracion, asi que
  cuando el servidor de consola esta activo hay dos escritores sobre el
  mismo hardware y el texto puede entremezclarse. Un microkernel estricto
  dejaria en el kernel, como mucho, una salida de panico.
