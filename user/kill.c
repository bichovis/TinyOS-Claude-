/* user/kill.c - Mandarle una senyal a otro proceso
 *
 *   kill 17        manda SIGTERM
 *   kill 17 9      manda SIGKILL, que no se puede atrapar
 *
 * Los pids salen del comando 'l' del menu del kernel.
 */
#include "syscall.h"

static uint64_t numero(const char *s)
{
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    if (argc < 2) {
        kprint("\n  uso: kill PID [senyal]   (por defecto 15, SIGTERM)\n");
        exit(1);
    }

    uint64_t pid = numero(argv[1]);
    int      sig = (argc > 2) ? (int)numero(argv[2]) : SIGTERM;

    if (kill(pid, sig) < 0) {
        kprint("\n  no hay ningun proceso con ese pid\n");
        exit(1);
    }

    kprint("\n  senyal enviada\n");
    exit(0);
}
