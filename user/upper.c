/* user/upper.c - Pasar la entrada a mayusculas
 *
 * El primer programa del sistema que lee de su ENTRADA en vez de
 * inventarsela. Y no sabe de donde viene: si lo arranca el shell a secas,
 * del teclado; si lo arranca detras de una tuberia, del programa anterior.
 * Esa ignorancia es justamente lo que lo hace combinable.
 */
#include "syscall.h"

static char buf[128];

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    (void)argc; (void)argv;

    for (;;) {
        int64_t n = read(0, buf, sizeof(buf));
        if (n <= 0) break;                   /* 0 = se acabo la entrada */

        for (int64_t i = 0; i < n; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 32);

        write(1, buf, (uint64_t)n);
    }
    exit(0);
}
