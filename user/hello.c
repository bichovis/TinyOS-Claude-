/* user/hello.c - Primer proceso de espacio de usuario de TinyOS
 *
 * Esto NO es parte del kernel: se compila y enlaza por separado, en su
 * propio espacio de direcciones (0x80000000), y se ejecuta en EL0. No puede
 * llamar a uart_puts ni a nada del kernel; solo a traves de 'svc'.
 */
#include "syscall.h"

/* Que el enlazador ponga _start el primero (ver user/user.ld) */
void _start(void) __attribute__((section(".text.start")));

void _start(void)
{
    print("\n  >> Hola desde EL0. Soy un proceso de usuario.\n");

    print("  >> mi pid es ");
    print_dec(getpid());
    print("\n");

    for (int i = 1; i <= 3; i++) {
        print("  >> vuelta ");
        print_dec((uint64_t)i);
        print(", uptime ");
        print_dec(uptime());
        print(" ms\n");
        sleep(60);                 /* syscall bloqueante: el kernel me duerme */
    }

    print("  >> Ahora intento leer memoria del kernel (0x80000)...\n");
    volatile unsigned int *kernel_mem = (volatile unsigned int *)0x80000UL;
    unsigned int robado = *kernel_mem;      /* deberia morir aqui */

    print("  >> LO HE CONSEGUIDO, el aislamiento no funciona: ");
    print_dec(robado);
    print("\n");
    exit(1);
}
