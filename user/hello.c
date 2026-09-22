/* user/hello.c - Primer proceso de espacio de usuario de TinyOS
 *
 * Esto NO es parte del kernel: se compila y enlaza por separado, en su
 * propio espacio de direcciones (0x400000), y se ejecuta en EL0. No puede
 * llamar a uart_puts ni a nada del kernel; solo a traves de 'svc'.
 */
#include "syscall.h"

/* Imprimir un numero por la via directa del kernel (SYS_write). El cliente
 * del servidor de consola lo hace por mensajes; este proceso usa la syscall
 * a proposito, para que se vean los dos caminos. */
static void kdec(uint64_t v)
{
    char buf[24];
    uint64_t n = udec(buf, v);
    buf[n] = 0;
    kprint(buf);
}

/* Que el enlazador ponga _start el primero (ver user/user.ld) */
void _start(void) __attribute__((section(".text.start")));

void _start(void)
{
    kprint("\n  >> Hola desde EL0. Soy un proceso de usuario.\n");

    kprint("  >> mi pid es ");
    kdec(getpid());
    kprint("\n");

    for (int i = 1; i <= 3; i++) {
        kprint("  >> vuelta ");
        kdec((uint64_t)i);
        kprint(", uptime ");
        kdec(uptime());
        kprint(" ms\n");
        sleep(60);                 /* syscall bloqueante: el kernel me duerme */
    }

    kprint("  >> Ahora intento leer memoria del kernel (0x80000)...\n");
    volatile unsigned int *kernel_mem = (volatile unsigned int *)0x80000UL;
    unsigned int robado = *kernel_mem;      /* deberia morir aqui */

    kprint("  >> LO HE CONSEGUIDO, el aislamiento no funciona: ");
    kdec(robado);
    kprint("\n");
    exit(1);
}
