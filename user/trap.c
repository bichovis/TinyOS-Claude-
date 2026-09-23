/* user/trap.c - Atrapar Ctrl-C
 *
 * Al llegar la senyal, el proceso "aparece" dentro de manejador() sin
 * haberlo llamado: lo ha fabricado el kernel reescribiendo su contexto. Y
 * cuando manejador() termina, vuelve exactamente a donde estaba, a mitad
 * de lo que fuera que estuviese haciendo.
 *
 * A la cuarta se rinde: vuelve a poner la accion por defecto y deja que la
 * siguiente lo mate. Asi se ve que atrapar una senyal es una eleccion, y
 * que se puede dejar de elegir.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

static volatile int veces;

static void manejador(int sig)
{
    veces++;
    printf("\n  [trap] atrapada la senyal %lu", (uint64_t)sig);
    printf(", van %lu\n", (uint64_t)veces);

    if (veces >= 3) {
        signal(SIGINT, 0);              /* vuelta a la accion por defecto */
        printf("  [trap] me rindo: la proxima me mata\n");
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (signal(SIGINT, manejador) < 0) {
        printf("  [trap] no he podido registrar el manejador\n");
        exit(1);
    }

    printf("\n  [trap] soy el pid %lu\n", getpid());
    printf("  [trap] pulsa Ctrl-C. Las tres primeras las atrapo.\n");

    for (uint64_t i = 1; ; i++) {
        sleep(50);
        printf("  [trap] sigo aqui, vuelta %lu\n", i);
    }
}
