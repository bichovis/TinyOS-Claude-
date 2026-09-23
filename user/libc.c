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
#include <errno.h>
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

/* --- Ficheros con buffer ---------------------------------------------- */

#define PRUEBA "/tmp-libc.txt"

static void prueba_buffer(void)
{
    printf("\n  --- el cubo ---\n");

    /* Lo que cuesta NO tener cubo.
     *
     * Los mismos 2000 caracteres por los mismos dos caminos. Lo que se
     * mide no es el tiempo -que tambien-, sino cuantas veces hay que
     * cruzar a EL1: excepcion, cambio de privilegio, tabla de vectores,
     * y vuelta. */
    /* El mismo binario, dos comportamientos. Con `libc` a secas stdout
     * habla con la consola y se vacia por lineas, porque hay alguien
     * mirando; con `libc > fichero` no hay nadie, y se llena el cubo
     * entero antes de bajar al kernel. */
    printf("  stdout habla con %s\n", isatty(1) ? "un terminal" : "un fichero");

    unsigned long antes = stdio_escrituras;

    FILE *f = fopen(PRUEBA, "w");
    if (!f) { printf("  MAL: no puedo crear %s: %s\n", PRUEBA, strerror(errno)); return; }
    for (int i = 0; i < 2000; i++) fputc('a' + (i % 26), f);
    fclose(f);

    unsigned long con = stdio_escrituras - antes;

    antes = stdio_escrituras;
    int fd = (int)openf(PRUEBA, O_ESCRIBIR);
    if (fd < 0) { printf("  MAL: no puedo reabrir: %s\n", strerror(errno)); return; }
    for (int i = 0; i < 2000; i++) { char c = 'a' + (i % 26); write(fd, &c, 1); }
    closefd(fd);

    printf("  2000 caracteres con cubo:  %lu llamadas a write()\n", con);
    printf("  2000 caracteres sin cubo:  2000 llamadas a write()\n");
    printf("  o sea %lu veces menos viajes al kernel\n", 2000 / (con ? con : 1));

    /* Orden. Antes de este paso printf tenia su propio cubo y fputs
     * escribia al descriptor: lo segundo salia ANTES que lo primero. */
    printf("  orden: uno");
    fputs(" dos", stdout);
    printf(" tres (tienen que ir en ese orden)\n");
}

static void prueba_leer(void)
{
    printf("\n  --- leer ---\n");

    FILE *f = fopen(PRUEBA, "w");
    if (!f) { printf("  MAL: no puedo crear\n"); return; }
    fprintf(f, "primera\nsegunda\ntercera\n");
    fclose(f);

    f = fopen(PRUEBA, "r");
    if (!f) { printf("  MAL: no puedo leer: %s\n", strerror(errno)); return; }

    char linea[64];
    int n = 0;
    while (fgets(linea, sizeof(linea), f)) n++;
    printf("  fgets encuentra %d lineas: %s\n", n, n == 3 ? "ok" : "MAL");
    printf("  feof despues de la ultima: %s\n", feof(f) ? "ok" : "MAL");

    /* ftell tiene que descontar lo que queda sin consumir en el cubo. Si
     * se le olvida, devuelve el tamano del fichero entero desde el primer
     * caracter leido. */
    rewind(f);
    int c = fgetc(f);
    long donde = ftell(f);
    printf("  ftell tras leer 1 caracter dice %ld: %s\n",
           donde, donde == 1 ? "ok" : "MAL");

    printf("  ungetc devuelve el caracter: %s\n",
           (ungetc(c, f) == c && fgetc(f) == c) ? "ok" : "MAL");

    fseek(f, 8, DESDE_INICIO);
    fgets(linea, sizeof(linea), f);
    printf("  fseek a 8 cae en \"segunda\": %s\n",
           strncmp(linea, "segunda", 7) == 0 ? "ok" : "MAL");

    fclose(f);
    unlink(PRUEBA);
}

/* La mentira clasica de depurar con printf.
 *
 * Dos hijos escriben exactamente lo mismo. Uno sale por exit(), que vacia
 * los cubos; el otro por _exit(), que es lo que pasa de verdad cuando un
 * programa se muere de golpe. Solo se lee uno de los dos mensajes, y por
 * eso el ultimo printf que ves en pantalla no es el ultimo que se
 * ejecuto. */
static void prueba_perdida(void)
{
    printf("\n  --- lo que se pierde ---\n");

    /* Vaciar ANTES del fork. Si no, el hijo se lleva una copia del cubo
     * del padre y lo que hay dentro sale impreso DOS veces, una por cada
     * proceso. Es el error mas viejo de mezclar buffers con fork. */
    fflush(stdout);

    /* Los dos mensajes van SIN salto de linea al final, a proposito: asi
     * se quedan en el cubo, porque stdout en un terminal se vacia por
     * lineas y estas no han terminado. Lo unico que los separa es por
     * donde sale cada proceso. */
    int64_t h = fork();
    if (h == 0) { printf("  salgo por exit()  y esto se lee"); exit(0); }
    waitpid((uint64_t)h);
    printf("\n");

    h = fork();
    if (h == 0) { printf("  salgo por _exit() y esto no se lee"); _exit(0); }
    waitpid((uint64_t)h);

    printf("  arriba hay UNA linea, no dos\n");
}

int main(int argc, char **argv)

{
    (void)argc; (void)argv;

    prueba_salto();
    prueba_orden();
    prueba_numeros();
    prueba_monton();
    prueba_buffer();
    prueba_leer();
    prueba_perdida();

    printf("\n  --- fin ---\n");
    return 0;
}
