/* user/hello.c - Primer proceso de espacio de usuario de TinyOS
 *
 * Esto NO es parte del kernel: se compila y enlaza por separado, en su
 * propio espacio de direcciones (0x400000), y se ejecuta en EL0. No puede
 * llamar a uart_puts ni a nada del kernel; solo a traves de 'svc'.
 */
#include "syscall.h"

/* Dos variables globales. Hasta el paso 12 esto era imposible: el kernel
 * mapeaba la imagen ENTERA de solo lectura, asi que escribir en 'veces'
 * era un fallo de permisos, y 'marca' ni siquiera estaba mapeada porque
 * .bss no ocupa sitio en el binario. */
static int  veces = 41;       /* .data: llega con su valor desde la imagen */
static char marca[32];        /* .bss : no esta en la imagen, llega a cero */

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

    /* --- .data: viene con valor, y se puede cambiar --- */
    kprint("  >> global de .data: ");
    kdec((uint64_t)veces);
    veces++;
    kprint(" -> le sumo uno -> ");
    kdec((uint64_t)veces);
    kprint("\n");

    /* --- .bss: no esta en la imagen y aun asi llega a cero --- */
    kprint("  >> global de .bss, sin inicializar: ");
    kdec((uint64_t)marca[0]);
    for (int i = 0; i < 11; i++)
        marca[i] = (char)("escribible"[i]);
    kprint(" -> escribo en ella -> ");
    kprint(marca);
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
