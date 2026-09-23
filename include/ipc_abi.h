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
/* Un mensaje lleva 256 bytes de datos. Eran 128 hasta el paso 31, y
 * crecieron porque una peticion al servidor de ficheros tiene que llevar
 * ahora una RUTA entera y no un nombre de doce caracteres. Con 128 no
 * cabian las dos cosas: o la ruta era ridicula o el trozo de fichero se
 * quedaba en la mitad.
 *
 * Lo que cuesta: cada puerto tiene ocho huecos, y hay ocho puertos, asi
 * que el kernel pasa de 9,7 KB de colas a 17,9 KB. A cambio, FS_CHUNK
 * sube de 96 a 176 bytes y leer un fichero necesita casi la mitad de
 * viajes. */
#define MSG_DATA_MAX         256

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
#define SYS_spawn        11    /* (buffer, bytes, args) -> pid | -1        */
#define SYS_clock_rate   12    /* (id) -> Hz de un reloj de la placa        */
#define SYS_read         13    /* (fd, buffer, bytes) -> leidos | 0 | -1    */
#define SYS_waitpid      14    /* (pid) -> 0 cuando ese proceso termina     */
#define SYS_sbrk         15    /* (delta) -> tope viejo del monton          */
#define SYS_fork         16    /* () -> pid del hijo en el padre, 0 en el hijo */
#define SYS_freepages    17    /* () -> paginas de 4 KB libres en el sistema */
#define SYS_exec         18    /* (buffer, bytes, args) -> no vuelve         */
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
