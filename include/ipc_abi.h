/* ipc_abi.h - Contrato entre el kernel y los procesos de usuario
 *
 * Este es el UNICO fichero que comparten los dos mundos. El kernel y los
 * programas de usuario se compilan y enlazan por separado y no comparten
 * ni una linea de codigo; lo unico que tienen en comun es este acuerdo
 * sobre como se ve un mensaje y que numero tiene cada llamada.
 */
#pragma once

#define MSG_DATA_MAX   48

struct message {
    unsigned long from;             /* pid del remitente: lo pone el kernel */
    unsigned long type;             /* lo interpreta la aplicacion          */
    unsigned long len;              /* bytes utiles de data[]               */
    char          data[MSG_DATA_MAX];
};                                  /* 72 bytes */

/* Puerto conocido: el servidor de consola es siempre el 0, para que los
 * clientes sepan a donde escribir sin necesitar un servicio de nombres. */
#define PORT_CONSOLE   0

/* Tipos de mensaje que entiende el servidor de consola */
#define CMSG_PRINT     1

/* --- Numeros de llamada al sistema ------------------------------------ */
#define SYS_write         1
#define SYS_exit          2
#define SYS_yield         3
#define SYS_getpid        4
#define SYS_sleep         5
#define SYS_uptime        6
#define SYS_port_create   7    /* (deseado o -1) -> id de puerto           */
#define SYS_send          8    /* (puerto, struct message *) -> 0 | -1     */
#define SYS_recv          9    /* (puerto, struct message *) -> 0 | -1     */
#define SYS_mmio_base    10    /* () -> VA del MMIO concedido, 0 si ninguno*/
#define SYS_spawn        11    /* (buffer, bytes) -> pid | -1              */
#define SYS_clock_rate   12    /* (id) -> Hz de un reloj de la placa        */

/* Relojes que un driver de EL0 puede preguntar. El kernel es el dueño del
 * buzon de la GPU y solo contesta a esta lista corta. */
#define CLK_EMMC        1
#define CLK_UART        2
#define CLK_CORE        4
