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
#include "syscall.h"

#define MEGA  (1024 * 1024)

static int global = 111;         /* en .data, compartida hasta que se toque */

static void num(const char *antes, uint64_t v, const char *despues)
{
    char b[24];
    uint64_t n = udec(b, v);
    b[n] = 0;
    kprint(antes); kprint(b); kprint(despues);
}

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    (void)argc; (void)argv;

    num("\n  paginas libres al empezar : ", freepages(), "\n");

    unsigned char *grande = malloc(MEGA);
    if (!grande) { kprint("  no hay 1 MB\n"); exit(1); }
    for (uint64_t i = 0; i < MEGA; i++) grande[i] = (unsigned char)i;

    uint64_t antes = freepages();
    num("  tras reservar y tocar 1 MB: ", antes, "\n");

    int64_t hijo = fork();
    if (hijo < 0) { kprint("  el fork ha fallado\n"); exit(1); }

    if (hijo == 0) {
        /* --- El hijo --- */
        num("\n  [hijo]  soy el pid ", getpid(), "");
        num(", la global vale ", (uint64_t)global, "\n");

        global = 222;                      /* aqui salta el copy-on-write */
        grande[0] = 99;                    /* y aqui otra vez */

        num("  [hijo]  la cambio a ", (uint64_t)global, " y me voy\n");
        exit(0);
    }

    /* --- El padre --- */
    uint64_t despues = freepages();
    num("\n  [padre] el hijo es el pid ", (uint64_t)hijo, "\n");
    num("  [padre] el fork ha costado ", antes - despues, " paginas\n");
    kprint("          (si copiara el mega, serian 256 y pico)\n");

    waitpid((uint64_t)hijo);

    num("\n  [padre] mi global sigue valiendo ", (uint64_t)global, "");
    kprint(global == 111 ? "   <- aislado\n" : "   <- MAL\n");
    num("  [padre] y grande[0] sigue siendo ", (uint64_t)grande[0], "\n");
    num("  [padre] paginas libres al final : ", freepages(), "\n");

    exit(0);
}
