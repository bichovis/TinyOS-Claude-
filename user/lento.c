/* user/lento.c - Un programa que tarda, para poder interrumpirlo
 *
 * No hace nada util a proposito: cuenta despacio y dice quien es. Lo que
 * se prueba con el es a quien llega el Ctrl-C y quien sigue vivo despues,
 * y para eso hace falta que haya algo vivo a lo que llegar.
 *
 * Escribe por stderr y no por stdout, y no es un detalle: en "lento a |
 * lento b" la salida del primero se va por la tuberia y no se ve, y lo
 * que hay que ver es justamente si los DOS siguen contando. stderr no
 * lleva cubo y no se redirige aqui, asi que sale siempre y en el acto.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 10;
    const char *etiqueta = argc > 2 ? argv[2] : "lento";

    fprintf(stderr, "  [%s] pid %lu, grupo %lu\n", etiqueta,
            (unsigned long)getpid(), (unsigned long)getpgid(0));

    for (int i = 1; i <= n; i++) {
        fprintf(stderr, "  [%s] %d de %d\n", etiqueta, i, n);
        sleep(50);                       /* 100 ticks = 1 s, o sea medio */
    }

    fprintf(stderr, "  [%s] terminado\n", etiqueta);
    return 0;
}
