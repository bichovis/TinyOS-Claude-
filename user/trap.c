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
 *
 * Y comprueba una cosa mas, que no se ve: que el manejador NO LE PISA LOS
 * REGISTROS DE COMA FLOTANTE. El bucle principal lleva una cuenta con
 * decimales, el manejador hace las suyas, y al volver la cuenta tiene que
 * seguir donde estaba. Si el marco de senyal no guardara la FPU, esto
 * daria un numero distinto cada vez que pulsas Ctrl-C, y solo si pulsas.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

static volatile int veces;

/* Cuentas con decimales DENTRO del manejador. Son las que pisarian los
 * registros del programa interrumpido si el kernel no los guardara. */
__attribute__((noinline))
static double ensuciar_la_fpu(int n)
{
    double x = 1.0;
    for (int i = 0; i < n; i++) x = x * 1.5 + 0.25;
    return x;
}

static void manejador(int sig)
{
    veces++;
    printf("\n  [trap] atrapada la senyal %lu", (uint64_t)sig);
    printf(", van %lu\n", (uint64_t)veces);
    printf("  [trap] y de paso ensucio la FPU: %.3f\n", ensuciar_la_fpu(20));

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

    /* Una cuenta con decimales que va creciendo despacio. Lo importante
     * no es el numero: es que sea el MISMO con senyales y sin ellas. */
    double cuenta = 0.0;

    for (uint64_t i = 1; ; i++) {
        cuenta += 0.125;
        sleep(50);
        printf("  [trap] vuelta %lu, mi cuenta va por %.3f  (%lu x 0.125)\n",
               i, cuenta, i);
    }
}
