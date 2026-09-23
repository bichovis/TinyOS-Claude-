/* user/kill.c - Mandarle una senyal a otro proceso
 *
 *   kill 17        manda SIGTERM
 *   kill 17 9      manda SIGKILL, que no se puede atrapar
 *
 * Los pids salen del comando 'l' del menu del kernel.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

static uint64_t numero(const char *s)
{
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("\n  uso: kill PID [senyal]   (por defecto 15, SIGTERM)\n");
        exit(1);
    }

    uint64_t pid = numero(argv[1]);
    int      sig = (argc > 2) ? (int)numero(argv[2]) : SIGTERM;

    if (kill(pid, sig) < 0) {
        printf("\n  no hay ningun proceso con ese pid\n");
        exit(1);
    }

    printf("\n  senyal enviada\n");
    exit(0);
}
