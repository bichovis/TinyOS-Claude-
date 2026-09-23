/* user/upper.c - Pasar la entrada a mayusculas
 *
 * El primer programa del sistema que lee de su ENTRADA en vez de
 * inventarsela. Y no sabe de donde viene: si lo arranca el shell a secas,
 * del teclado; si lo arranca detras de una tuberia, del programa anterior.
 * Esa ignorancia es justamente lo que lo hace combinable.
 */
#include <stdlib.h>
#include "syscall.h"

static char buf[128];

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    for (;;) {
        int64_t n = read(0, buf, sizeof(buf));
        if (n <= 0) break;                   /* 0 = se acabo la entrada */

        for (int64_t i = 0; i < n; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 32);

        /* write() puede escribir MENOS de lo que se le pide, y entonces
         * hay que volver. Con una tuberia detras no pasa nunca y el fallo
         * no se ve; con un fichero detras, si. */
        for (int64_t o = 0; o < n; ) {
            int64_t k = write(1, buf + o, (uint64_t)(n - o));
            if (k <= 0) break;
            o += k;
        }
    }
    exit(0);
}
