/* ipc_abi.h - Contrato entre el kernel y los procesos de usuario
 *
 * Este es el UNICO fichero que comparten los dos mundos. El kernel y los
 * programas de usuario se compilan y enlazan por separado y no comparten
 * ni una linea de codigo; lo unico que tienen en comun es este acuerdo
 * sobre como se ve un mensaje y que numero tiene cada llamada.
 */
#pragma once

/* 128 y no 48: una peticion de escritura tiene que llevar el nombre del
 * fichero Y los datos en el mismo mensaje. Con 8 puertos de 8 mensajes,
 * la cola entera del kernel son 10 KB de .bss. */
/* Un mensaje lleva 512 bytes de datos: 128 hasta el paso 31, 256 hasta el
 * 44, y ahora 512.
 *
 * Cada vez que crece es por lo mismo: la ruta y el trozo de fichero salen
 * del MISMO mensaje, asi que alargar una encoge el otro. Con 256 bytes una
 * ruta decente dejaba el trozo en 176, y una ruta de verdad -las de un
 * arbol de fuentes pasan de 150 caracteres- no cabia de ninguna manera.
 *
 * Lo que cuesta: ocho puertos de ocho huecos pasan de 17,9 KB a 34 KB de
 * monton del kernel. A cambio la ruta llega a 256 y el trozo sube de 176
 * a 240, que ademas hace las lecturas un 30% mas baratas.
 *
 * Que dos cosas que no tienen nada que ver compitan por el mismo espacio
 * es el sintoma de un protocolo que mete todo en un mensaje de tamanyo
 * fijo. Lo limpio seria separar la ruta de los datos; mientras tanto,
 * esto es un numero que se sube cuando hace falta. */
#define MSG_DATA_MAX         512

struct message {
    unsigned long from;             /* pid del remitente: lo pone el kernel */
    unsigned long type;             /* lo interpreta la aplicacion          */
    unsigned long len;              /* bytes utiles de data[]               */
    char          data[MSG_DATA_MAX];
};                                  /* 152 bytes */

/* Puerto conocido: el servidor de consola es siempre el 0, para que los
 * clientes sepan a donde escribir sin necesitar un servicio de nombres. */
#define PORT_CONSOLE   0

/* Tipos de mensaje que entiende el servidor de consola */
#define CMSG_PRINT     1
#define CMSG_IRQ       2    /* lo manda el KERNEL: ha llegado una interrupcion */

/* La unica interrupcion que un proceso puede pedir, por ahora. */
#define IRQ_UART      57

/* --- Numeros de llamada al sistema ------------------------------------ */
#define SYS_write         1    /* (fd, buffer, bytes) -> escritos | -1     */
#define SYS_exit          2
#define SYS_yield         3
#define SYS_getpid        4
#define SYS_sleep         5
#define SYS_uptime        6
#define SYS_port_create   7    /* (deseado o -1) -> id de puerto           */
#define SYS_send          8    /* (puerto, struct message *) -> 0 | -1     */
#define SYS_recv          9    /* (puerto, struct message *) -> 0 | -1     */
#define SYS_mmio_base    10    /* () -> VA del MMIO concedido, 0 si ninguno*/
#define SYS_spawn        11    /* (buffer, bytes, argv[], envp[]) -> pid|-1*/
#define SYS_clock_rate   12    /* (id) -> Hz de un reloj de la placa        */
#define SYS_read         13    /* (fd, buffer, bytes) -> leidos | 0 | -1    */
#define SYS_waitpid      14    /* (pid) -> 0 cuando ese proceso termina     */
#define SYS_sbrk         15    /* (delta) -> tope viejo del monton          */
#define SYS_fork         16    /* () -> pid del hijo en el padre, 0 en el hijo */
#define SYS_freepages    17    /* () -> paginas de 4 KB libres en el sistema */
#define SYS_exec         18    /* (buffer, bytes, argv[], envp[]) -> no vuelve*/
#define SYS_kill         19    /* (pid, senyal) -> 0 | -1                    */
#define SYS_signal       20    /* (senyal, manejador, trampolin) -> 0 | -1   */
#define SYS_sigreturn    21    /* lo llama el trampolin, no el programa      */
#define SYS_pipe         22    /* (int fds[2]) -> 0 | -1                     */
#define SYS_close        23    /* (fd) -> 0 | -1                             */
#define SYS_dup2         24    /* (viejo, nuevo) -> nuevo | -1               */

/* --- Para los drivers de EL0 ------------------------------------------
 * Un proceso no puede recibir interrupciones: las interrupciones son de
 * EL1. Lo que si puede es pedir que se le conviertan en MENSAJES.
 *
 * Al llegar la interrupcion, el kernel la enmascara y manda un CMSG_IRQ al
 * puerto que se le dijo. El driver la atiende cuando le toca -ya en EL0,
 * como un mensaje mas de su bucle- y llama a irq_ack() para volver a
 * abrirla. Que quede enmascarada mientras tanto es lo que evita que una
 * interrupcion que nadie atiende inunde el sistema. */
#define SYS_irq_register 25   /* (irq, puerto) -> 0 | -1                    */
#define SYS_irq_ack      26   /* (irq) -> 0 | -1                            */

/* El driver de consola le pasa al kernel lo que ha leido del teclado. */
#define SYS_console_push 27   /* (buffer, bytes) -> 0 | -1                  */
#define SYS_console_int  28   /* () -> Ctrl-C al proceso de primer plano    */

/* Abrir un fichero de la tarjeta y quedarselo en un descriptor. Es lo que
 * hace falta para que el shell pueda redirigir con > y <. */
#define SYS_open         29   /* (nombre, modo) -> fd | -1                  */

/* El directorio actual de un proceso. Se hereda en el fork y sobrevive al
 * exec; por eso "cd" tiene que ser una orden interna del shell y no un
 * programa (ver sched.h). */
#define SYS_chdir        30   /* (ruta) -> 0 | -1                           */
#define SYS_getcwd       31   /* (buffer, bytes) -> 0 | -1                  */

/* Unir el cwd con una ruta relativa y normalizar. Lo hace el kernel para
 * que solo haya UNA implementacion: la usan los programas y la usa el
 * propio kernel para la redireccion. */
#define SYS_realpath     32   /* (ruta, salida) -> 0 | -1                   */

/* Mapear un fichero en el espacio del proceso. No lo lee: reserva las
 * direcciones y deja que los fallos de pagina vayan trayendo los trozos
 * segun se toquen. Devuelve la direccion, y el tamanyo en *tam. */
#define SYS_mmap         33   /* (ruta, uint64_t *tam) -> direccion | -1    */

#define SYS_munmap       34   /* (direccion) -> 0 | -1                      */

/* --- El arranque ------------------------------------------------------
 *
 * Arrancar uno de los programas que el kernel lleva dentro, por NOMBRE.
 * Solo puede llamarla init, que es el primer proceso y el unico en el que
 * el kernel confia para esto.
 *
 * Existe porque hay un problema del huevo y la gallina: el servidor de
 * ficheros es un programa, y para leer un programa de la tarjeta hace
 * falta el servidor de ficheros. Alguien tiene que traer los primeros
 * dentro, y ese alguien es el kernel. Es lo mismo que hace un initramfs,
 * con tres entradas en vez de un sistema de ficheros entero.
 *
 * Se pide el DISPOSITIVO por nombre, no por direccion. Un proceso no
 * puede decir "mapeame la pagina 0x3F201000": dice "necesito la UART", y
 * el kernel decide si eso significa algo y que direccion es. La
 * diferencia es que la lista de lo concedible esta escrita en el kernel y
 * no la elige quien pregunta. */
#define SYS_bootstrap    35   /* (nombre, argv, envp, dispositivo) -> pid   */

#define DEV_NINGUNO       0
#define DEV_UART          1
#define DEV_EMMC          2

/* Ceder la consola a un proceso: quien la tenga recibe el Ctrl-C y lee del
 * teclado. Solo init, que es quien decide que hay en primer plano. */
#define SYS_consola      36   /* (pid) -> 0 | -1                            */

/* --- La hora -----------------------------------------------------------
 *
 * La Pi NO TIENE RELOJ DE TIEMPO REAL. No hay pila, no hay nada que siga
 * contando con la maquina apagada: al arrancar, el sistema no sabe que dia
 * es y no hay forma de que lo averigue solo.
 *
 * Lo que si sabe es cuanto lleva encendida. Asi que el reloj es una suma:
 *   hora = base + tiempo desde el arranque
 * y 'base' la pone alguien de fuera. Por defecto es la fecha en que se
 * compilo el kernel, que es una mentira util: no es la hora, pero ordena
 * bien los ficheros que escriba este sistema, y eso es lo que necesita
 * make. init puede corregirla con SYS_settime si /etc/fecha dice otra.
 *
 * Un reloj que solo sabe que el tiempo avanza, no que hora es. */
#define SYS_time         37   /* () -> segundos desde 1970                  */
#define SYS_settime      38   /* (segundos) -> 0 | -1   (solo init)         */

/* Memoria nueva, a cero, escribible, y traida segun se toque.
 *
 * sbrk mueve UN tope: la memoria es un bloque contiguo que crece y
 * encoge por arriba. Eso basta para un malloc pequenyo y se queda corto
 * en cuanto alguien quiere un arena grande y poder soltarlo entero sin
 * esperar a que se vacie lo que hay encima.
 *
 * mmap anonimo da tramos independientes: cada uno se pide, se usa y se
 * suelta por su cuenta. Es como reserva memoria cualquier compilador, y
 * por eso esta aqui. */
#define SYS_mmap_anon    47   /* (bytes) -> direccion | -1                  */

/* --- La superficie de fichero que espera una libc ---------------------
 *
 * Hasta aqui, cualquier programa que quisiera borrar un fichero tenia que
 * saber COMO se habla con el servidor: componer un struct fs_request,
 * crear un puerto, mandar el mensaje y esperar la respuesta. Cuarenta
 * lineas para un unlink.
 *
 * Eso esta bien cuando el que escribe el programa esta aprendiendo como
 * funciona un servidor de ficheros. Deja de estarlo en cuanto quieres
 * portar codigo que ya existe: newlib no sabe nada de puertos, sabe de
 * unlink(), stat() y lseek().
 *
 * El kernel ya hacia de intermediario para open, read y write. Estas
 * completan el juego. La IPC sigue ahi debajo, intacta: lo que cambia es
 * que ya no hay que conocerla para usar un fichero. */
#define SYS_stat         39   /* (ruta, struct estado *) -> 0 | -1          */
#define SYS_lseek        40   /* (fd, desplazamiento, desde) -> posicion    */
#define SYS_unlink       41   /* (ruta) -> 0 | -1                           */
#define SYS_mkdir        42   /* (ruta) -> 0 | -1                           */
#define SYS_rmdir        43   /* (ruta) -> 0 | -1                           */
#define SYS_rename       44   /* (origen, destino) -> 0 | -1                */
#define SYS_opendir      45   /* (ruta) -> fd | -1                          */
#define SYS_readdir      46   /* (fd, struct fs_info *) -> 1 | 0 | -1       */

/* Desde donde cuenta lseek. Los tres de siempre. */
#define DESDE_INICIO      0
#define DESDE_ACTUAL      1
#define DESDE_FINAL       2

/* Lo que se sabe de un fichero. Poco, porque poco hay: FAT no guarda
 * duenyo, ni permisos, ni enlaces. Inventar campos que siempre valen lo
 * mismo seria fingir que este sistema tiene cosas que no tiene. */
struct estado {
    unsigned long tam;
    unsigned long mtime;        /* segundos desde 1970, 0 si no hay */
    unsigned long flags;        /* FS_ES_DIR */
};

#define O_LEER            0
#define O_ESCRIBIR        1   /* lo crea, y si ya estaba lo vacia           */

/* --- Senyales ----------------------------------------------------------
 * Los numeros son los de siempre, para que no haya que aprenderselos otra
 * vez. Solo estan los tres que hacen falta.
 *
 * SIGKILL no se puede atrapar, y eso no es una limitacion: es su unico
 * motivo de existir. Si un proceso pudiera ignorarla, no habria forma de
 * acabar con un programa que se ha vuelto loco. */
#define SIG_MAX      16
#define SIGINT        2    /* Ctrl-C, la interrupcion del teclado           */
#define SIGKILL       9    /* fulminante, no se atrapa ni se ignora         */
#define SIGTERM      15    /* "haz el favor de irte", si se atrapa          */

/* Relojes que un driver de EL0 puede preguntar. El kernel es el dueño del
 * buzon de la GPU y solo contesta a esta lista corta. */
#define CLK_EMMC        1
#define CLK_UART        2
#define CLK_CORE        4
