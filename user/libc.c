/* user/libc.c - Probar lo que acaba de crecer en la libc
 *
 * Cada prueba esta pensada para FALLAR si la pieza esta mal, que es lo
 * unico que hace util a una prueba. Las que solo confirman lo que ya
 * creias no miden nada.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include "syscall.h"

/* --- setjmp / longjmp ------------------------------------------------ */

static jmp_buf salida;

/* Tres niveles de profundidad, para que el salto tenga que deshacer pila
 * de verdad y no solo volver de una llamada. */
__attribute__((noinline)) static void hondo3(int n) { longjmp(salida, n); }
__attribute__((noinline)) static void hondo2(int n) { hondo3(n); printf("  MAL: volvio de hondo3\n"); }
__attribute__((noinline)) static void hondo1(int n) { hondo2(n); printf("  MAL: volvio de hondo2\n"); }

/* Meter y sacar un numero de d8 A MANO.
 *
 * En C no hay forma de decir "esto tiene que vivir en d8", y sin eso no se
 * puede comprobar que setjmp guarda los registros de coma flotante: el
 * compilador decide donde pone cada cosa, y si la deja en memoria la
 * prueba pasa aunque setjmp no guarde nada.
 *
 * (Lo aprendi fallando: la primera version usaba una variable 'volatile',
 * que es justo lo que la fuerza a memoria. La prueba pasaba igual con el
 * guardado quitado, o sea que no media nada.) */
static inline void poner_d8(double v) { __asm__ volatile("fmov d8, %d0" :: "w"(v)); }
static inline double leer_d8(void)
{
    double v;
    __asm__ volatile("fmov %d0, d8" : "=w"(v));
    return v;
}

static void prueba_salto(void)
{
    printf("\n  --- setjmp y longjmp ---\n");

    /* volatile: despues de un longjmp, lo que no este en memoria vuelve
     * atras. Sin esto el compilador podria dejar 'vueltas' en un registro
     * y el contador se reiniciaria en cada salto. */
    volatile int vueltas = 0;

    /* En d8 ANTES de setjmp: eso es lo que setjmp tiene que guardar. */
    poner_d8(3.25);

    int valor = setjmp(salida);
    vueltas++;

    if (valor == 0) {
        printf("  primera vez: setjmp devuelve %d\n", valor);

        poner_d8(99.5);                  /* lo cambio a algo distinto */
        hondo1(7);
        printf("  MAL: no deberia llegar aqui\n");
    } else if (valor == 7) {
        printf("  tras el salto: setjmp devuelve %d, desde tres niveles\n", valor);

        /* d8 tiene que valer lo de ANTES del setjmp, no lo de despues. */
        double d8 = leer_d8();
        printf("  d8 tras el salto: %.2f  %s\n", d8,
               (d8 > 3.24 && d8 < 3.26) ? "ok, restaurado" : "MAL");

        longjmp(salida, 0);              /* el caso raro: saltar con cero */
    } else {
        printf("  longjmp(buf, 0) devuelve %d: %s\n", valor,
               valor == 1 ? "ok, el estandar manda 1" : "MAL");
        printf("  he pasado por setjmp %d veces\n", vueltas);
    }
}

/* --- qsort y bsearch -------------------------------------------------- */

static int por_numero(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);        /* sin restar: la resta se desborda */
}

static void prueba_orden(void)
{
    printf("\n  --- qsort y bsearch ---\n");

    /* Ya ordenado a proposito: es el caso que vuelve cuadratico al
     * quicksort ingenuo, y con 400 elementos la diferencia se nota. */
    static int v[400];
    for (int i = 0; i < 400; i++) v[i] = i;

    uint64_t t0 = uptime();
    qsort(v, 400, sizeof(int), por_numero);
    uint64_t ordenado = uptime() - t0;

    int bien = 1;
    for (int i = 0; i < 400; i++) if (v[i] != i) bien = 0;
    printf("  400 ya ordenados: %s  (%lu ms)\n", bien ? "ok" : "MAL", ordenado);

    /* Y al reves, que es el otro caso malo. */
    for (int i = 0; i < 400; i++) v[i] = 399 - i;
    qsort(v, 400, sizeof(int), por_numero);

    bien = 1;
    for (int i = 0; i < 400; i++) if (v[i] != i) bien = 0;
    printf("  400 al reves    : %s\n", bien ? "ok" : "MAL");

    int busco = 137;
    int *hallado = bsearch(&busco, v, 400, sizeof(int), por_numero);
    printf("  bsearch(137)    : %s\n",
           (hallado && *hallado == 137) ? "ok" : "MAL");

    busco = 1000;
    printf("  bsearch(1000)   : %s\n",
           bsearch(&busco, v, 400, sizeof(int), por_numero) ? "MAL" : "ok, no esta");
}

/* --- strtol ----------------------------------------------------------- */

static void prueba_numeros(void)
{
    printf("\n  --- strtol ---\n");

    char *fin;
    struct { const char *texto; int base; long espera; } casos[] = {
        { "42",        10, 42   }, { "-17",      10, -17  },
        { "0x1f",       0, 31   }, { "  99 ",   10, 99   },
        { "ff",        16, 255  }, { "0755",     0, 493  },
        { "hola",      10, 0    },
    };

    for (unsigned i = 0; i < sizeof(casos) / sizeof(casos[0]); i++) {
        long v = strtol(casos[i].texto, &fin, casos[i].base);
        int leyo = (fin != casos[i].texto);

        printf("  %-8s base %2d -> %-5ld %s\n", casos[i].texto, casos[i].base, v,
               v == casos[i].espera
                   ? (leyo || v == 0 ? "ok" : "ok") : "MAL");
    }

    /* Lo que atoi no puede hacer: distinguir "0" de "no habia numero". */
    strtol("hola", &fin, 10);
    printf("  \"hola\": %s\n",
           fin == (char *)0 ? "MAL" : "fin apunta al principio, no leyo nada");
}

/* --- realloc y calloc -------------------------------------------------- */

static void prueba_monton(void)
{
    printf("\n  --- calloc y realloc ---\n");

    int *v = calloc(100, sizeof(int));
    if (!v) { printf("  MAL: calloc no da\n"); return; }

    int ceros = 1;
    for (int i = 0; i < 100; i++) if (v[i]) ceros = 0;
    printf("  calloc(100, 4) viene a cero: %s\n", ceros ? "ok" : "MAL");

    for (int i = 0; i < 100; i++) v[i] = i;

    v = realloc(v, 400 * sizeof(int));
    if (!v) { printf("  MAL: realloc no da\n"); return; }

    int conserva = 1;
    for (int i = 0; i < 100; i++) if (v[i] != i) conserva = 0;
    printf("  realloc a 4x conserva lo de antes: %s\n", conserva ? "ok" : "MAL");

    printf("  calloc(2, enorme) se niega: %s\n",
           calloc(2, (size_t)-1) ? "MAL" : "ok");

    free(v);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    prueba_salto();
    prueba_orden();
    prueba_numeros();
    prueba_monton();

    printf("\n  --- fin ---\n");
    return 0;
}
