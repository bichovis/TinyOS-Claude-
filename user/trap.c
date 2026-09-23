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
#include "syscall.h"

static volatile int veces;

static void num(const char *antes, uint64_t v, const char *despues)
{
    char b[24];
    uint64_t n = udec(b, v);
    b[n] = 0;
    kprint(antes); kprint(b); kprint(despues);
}

static void manejador(int sig)
{
    veces++;
    num("\n  [trap] atrapada la senyal ", (uint64_t)sig, "");
    num(", van ", (uint64_t)veces, "\n");

    if (veces >= 3) {
        signal(SIGINT, 0);              /* vuelta a la accion por defecto */
        kprint("  [trap] me rindo: la proxima me mata\n");
    }
}

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (signal(SIGINT, manejador) < 0) {
        kprint("  [trap] no he podido registrar el manejador\n");
        exit(1);
    }

    num("\n  [trap] soy el pid ", getpid(), "\n");
    kprint("  [trap] pulsa Ctrl-C. Las tres primeras las atrapo.\n");

    for (uint64_t i = 1; ; i++) {
        sleep(50);
        num("  [trap] sigo aqui, vuelta ", i, "\n");
    }
}
