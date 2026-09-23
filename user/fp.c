/* user/fp.c - Coma flotante, y una prueba de que el cambio de contexto
 * perezoso no se pierde nada.
 *
 * Lo facil es comprobar que 2.5 * 4 da 10. Lo que de verdad hay que probar
 * es otra cosa: que el estado SOBREVIVE a los cambios de contexto. Un
 * error en fp_save o en fp_restore no hace que las cuentas salgan mal
 * siempre; hace que salgan mal cuando entre dos instrucciones se ha
 * colado otro proceso. Y eso, en una maquina rapida y con un solo
 * programa corriendo, puede no pasar nunca.
 *
 * Asi que aqui se fuerza: se guarda un valor en la FPU, se llama a yield()
 * -que pasa por el planificador, salva la FPU y la apaga- y se comprueba
 * que al volver sigue estando. Con un fork por medio, para probar tambien
 * que el hijo hereda lo que el padre tenia a medias.
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

/* volatile para que el compilador no se lleve el valor a memoria ni
 * precalcule nada: lo queremos VIVO en un registro de la FPU. */
static volatile double acumulado;

/* noinline para que no se funda con quien la llama y el valor tenga que
 * viajar de verdad por los registros de argumentos de FP. */
__attribute__((noinline))
static double media(const double *v, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; i++) s += v[i];
    return s / (double)n;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n  --- coma flotante en EL0 ---\n");

    /* 1. Que las cuentas salgan. */
    double v[5] = { 1.5, 2.25, 3.125, 4.0, 5.625 };
    printf("  media de 5 numeros  : %f\n", media(v, 5));
    printf("  un tercio           : %.10f\n", 1.0 / 3.0);
    printf("  redondeo de 0.9999  : %.2f   (truncar daria 0.99)\n", 0.9999);
    printf("  negativo y anchura  : [%10.3f]\n", -2.71828);

    /* 2. Que sobrevive a un cambio de contexto.
     *
     * Se deja un valor en la FPU, se cede la CPU cincuenta veces -cada
     * yield es un paso por el planificador, que salva la FPU y la apaga- y
     * se comprueba que al volver el numero no ha cambiado. Si fp_save y
     * fp_restore no fueran simetricos, esto lo caza. */
    acumulado = 0.0;
    double paso = 0.1;
    for (int i = 0; i < 50; i++) {
        acumulado = acumulado + paso;
        yield();                       /* aqui la FPU se salva y se apaga */
    }
    printf("  50 sumas de 0.1     : %.4f  %s\n", acumulado,
           (acumulado > 4.999 && acumulado < 5.001) ? "ok" : "MAL");

    /* 3. Que el hijo hereda lo que el padre tenia a medias. */
    acumulado = 3.75;
    int64_t hijo = fork();
    if (hijo == 0) {
        acumulado = acumulado * 2.0;
        printf("  [hijo]  heredado 3.75, x2 = %.2f  %s\n", acumulado,
               (acumulado > 7.49 && acumulado < 7.51) ? "ok" : "MAL");
        exit(0);
    }
    if (hijo > 0) {
        waitpid((uint64_t)hijo);
        printf("  [padre] el mio sigue en %.2f  %s\n", acumulado,
               (acumulado > 3.74 && acumulado < 3.76) ? "ok" : "MAL");
    }

    printf("  --- fin ---\n");
    return 0;
}
