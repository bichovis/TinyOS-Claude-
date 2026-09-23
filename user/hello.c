/* user/hello.c - Primer proceso de espacio de usuario de TinyOS
 *
 * Esto NO es parte del kernel: se compila y enlaza por separado, en su
 * propio espacio de direcciones (0x400000), y se ejecuta en EL0. No puede
 * llamar a uart_puts ni a nada del kernel; solo a traves de 'svc'.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

/* Dos variables globales. Hasta el paso 12 esto era imposible: el kernel
 * mapeaba la imagen ENTERA de solo lectura, asi que escribir en 'veces'
 * era un fallo de permisos, y 'marca' ni siquiera estaba mapeada porque
 * .bss no ocupa sitio en el binario. */
static int  veces = 41;       /* .data: llega con su valor desde la imagen */
static char marca[32];        /* .bss : no esta en la imagen, llega a cero */

/* Que el enlazador ponga _start el primero (ver user/user.ld) */
int main(int argc, char **argv)
{
    printf("\n  >> Hola desde EL0. Soy un proceso de usuario.\n");

    /* Los argumentos. El kernel los dejo en mi propia pila antes de que yo
     * existiera, con x0 = argc y x1 = argv, que es el convenio de siempre. */
    printf("  >> me han llamado con ");
    printf("%lu", (uint64_t)argc);
    printf(" argumento(s):");
    for (int i = 0; i < argc; i++) {
        printf(" [");
        printf("%s", argv[i]);
        printf("]");
    }
    printf("\n");

    printf("  >> mi pid es ");
    printf("%lu", getpid());
    printf("\n");

    /* --- .data: viene con valor, y se puede cambiar --- */
    printf("  >> global de .data: ");
    printf("%lu", (uint64_t)veces);
    veces++;
    printf(" -> le sumo uno -> ");
    printf("%lu", (uint64_t)veces);
    printf("\n");

    /* --- .bss: no esta en la imagen y aun asi llega a cero --- */
    printf("  >> global de .bss, sin inicializar: ");
    printf("%lu", (uint64_t)marca[0]);
    for (int i = 0; i < 11; i++)
        marca[i] = (char)("escribible"[i]);
    printf(" -> escribo en ella -> ");
    printf("%s", marca);
    printf("\n");

    for (int i = 1; i <= 3; i++) {
        printf("  >> vuelta ");
        printf("%lu", (uint64_t)i);
        printf(", uptime ");
        printf("%lu", uptime());
        printf(" ms\n");
        sleep(60);                 /* syscall bloqueante: el kernel me duerme */
    }

    printf("  >> Ahora intento leer memoria del kernel (0x80000)...\n");
    volatile unsigned int *kernel_mem = (volatile unsigned int *)0x80000UL;
    unsigned int robado = *kernel_mem;      /* deberia morir aqui */

    printf("  >> LO HE CONSEGUIDO, el aislamiento no funciona: 0x%08x\n", robado);
    exit(1);
}
