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
#define MSG_DATA_MAX   128

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
