/* user/forkd.c - Bifurcarse, y ensenyar lo que NO cuesta
 *
 * Dos cosas a la vez:
 *
 *   que padre e hijo quedan aislados de verdad, aunque compartan paginas
 *   que bifurcar un proceso con un mega de datos no copia un mega
 *
 * Lo segundo se mide contando paginas libres antes y despues. Si el fork
 * copiara la memoria, bifurcar con 1 MB reservado costaria 256 paginas.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

#define MEGA  (1024 * 1024)

static int global = 111;         /* en .data, compartida hasta que se toque */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n  paginas libres al empezar : %lu\n", freepages());

    unsigned char *grande = malloc(MEGA);
    if (!grande) { printf("  no hay 1 MB\n"); exit(1); }
    for (uint64_t i = 0; i < MEGA; i++) grande[i] = (unsigned char)i;

    uint64_t antes = freepages();
    printf("  tras reservar y tocar 1 MB: %lu\n", antes);

    int64_t hijo = fork();
    if (hijo < 0) { printf("  el fork ha fallado\n"); exit(1); }

    if (hijo == 0) {
        /* --- El hijo --- */
        printf("\n  [hijo]  soy el pid %lu", getpid());
        printf(", la global vale %lu\n", (uint64_t)global);

        global = 222;                      /* aqui salta el copy-on-write */
        grande[0] = 99;                    /* y aqui otra vez */

        printf("  [hijo]  la cambio a %lu y me voy\n", (uint64_t)global);
        exit(0);
    }

    /* --- El padre --- */
    uint64_t despues = freepages();
    printf("\n  [padre] el hijo es el pid %lu\n", (uint64_t)hijo);
    printf("  [padre] el fork ha costado %lu paginas\n", antes - despues);
    printf("          (si copiara el mega, serian 256 y pico)\n");

    waitpid((uint64_t)hijo);

    printf("\n  [padre] mi global sigue valiendo %lu", (uint64_t)global);
    printf("%s", global == 111 ? "   <- aislado\n" : "   <- MAL\n");
    printf("  [padre] y grande[0] sigue siendo %lu\n", (uint64_t)grande[0]);
    printf("  [padre] paginas libres al final : %lu\n", freepages());

    exit(0);
}
