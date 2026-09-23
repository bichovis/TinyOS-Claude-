/* user/env.c - Ensenyar el entorno
 *
 * Es un PROGRAMA y no una orden interna, y eso no es casualidad: al serlo,
 * demuestra lo que quiere ensenyar. Si "env" imprime PATH es porque el
 * shell se bifurco, el hijo heredo el entorno, hizo exec, y el kernel se
 * lo volvio a dar al programa nuevo. Una orden interna no probaria nada.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!environ || !environ[0]) {
        printf("\n  el entorno esta vacio\n");
        return 0;
    }

    printf("\n");
    for (int i = 0; environ[i]; i++)
        printf("  %s\n", environ[i]);

    return 0;
}
