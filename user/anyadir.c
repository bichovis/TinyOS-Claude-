/* user/anyadir.c - Que dos programas escriban en el mismo fichero
 *
 * La pregunta de este paso no es como se anyade al final de un fichero:
 * eso son dos lineas. Es quien decide DONDE acaba el fichero, y cuando.
 *
 * Si lo decide quien escribe, lo hace preguntando primero y escribiendo
 * despues, y entre la pregunta y la respuesta el fichero ya ha cambiado.
 * Si lo decide el servidor dentro de la propia escritura, no hay entre.
 *
 * Este programa mide las dos cosas, porque una prueba que solo ensenya la
 * version buena no prueba nada: podria estar funcionando por casualidad.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "syscall.h"

#define HIJOS    3
#define LINEAS  20
#define ANCHO   16                      /* incluido el salto de linea */

/* Una linea de ANCHO bytes exactos y con la letra del hijo repetida. Asi
 * no hace falta confiar en la cuenta: si una linea sale con dos letras
 * distintas dentro, es que dos escrituras se han solapado, y se ve. */
static void linea_de(char *dst, int hijo, int n)
{
    char letra = 'A' + hijo;
    for (int i = 0; i < ANCHO - 1; i++) dst[i] = letra;
    dst[0] = '0' + hijo;
    dst[1] = ':';
    dst[2] = '0' + (n / 10);
    dst[3] = '0' + (n % 10);
    dst[ANCHO - 1] = '\n';
}

/* Contar lo que ha sobrevivido. Devuelve el tamanyo, y en 'rotas' cuantas
 * lineas no son de un solo hijo. */
static long revisar_fichero(const char *ruta, int *rotas, int *por_hijo)
{
    for (int i = 0; i < HIJOS; i++) por_hijo[i] = 0;
    *rotas = 0;

    FILE *f = fopen(ruta, "r");
    if (!f) return -1;

    char l[ANCHO + 1];
    long bytes = 0;

    while (fread(l, 1, ANCHO, f) == ANCHO) {
        bytes += ANCHO;

        int hijo = l[0] - '0';
        if (hijo < 0 || hijo >= HIJOS || l[ANCHO - 1] != '\n') { (*rotas)++; continue; }

        /* Todo el relleno tiene que ser la letra de ESE hijo. */
        char letra = 'A' + hijo;
        int limpia = 1;
        for (int i = 4; i < ANCHO - 1; i++) if (l[i] != letra) limpia = 0;

        if (limpia) por_hijo[hijo]++;
        else        (*rotas)++;
    }

    /* Lo que sobre y no llegue a una linea entera tambien es un destrozo. */
    char sobra;
    while (fread(&sobra, 1, 1, f) == 1) { bytes++; (*rotas)++; }

    fclose(f);
    return bytes;
}

static void informe(const char *que, long bytes, int rotas, int *por_hijo)
{
    long esperado = (long)HIJOS * LINEAS * ANCHO;

    printf("  %s\n", que);
    printf("    esperados %ld bytes, hay %ld\n", esperado, bytes);
    printf("    por hijo:");
    for (int i = 0; i < HIJOS; i++) printf(" %d", por_hijo[i]);
    printf("  (tendrian que ser %d cada uno)\n", LINEAS);
    if (rotas) printf("    lineas destrozadas: %d\n", rotas);
}

/* --- Lo que se quiere: cada hijo con SU descriptor, abierto para anyadir.
 *
 * Los tres escriben a la vez y ninguno mira donde acaba el fichero: lo
 * resuelve el servidor dentro de cada escritura. Salen las 60 lineas,
 * mezcladas en cualquier orden y todas enteras. El orden no importaba;
 * que no se pise nadie, si. */
static void con_anyadir(const char *ruta)
{
    unlink(ruta);

    int64_t hijos[HIJOS];

    for (int h = 0; h < HIJOS; h++) {
        int64_t pid = fork();
        if (pid != 0) { hijos[h] = pid; continue; }

        int fd = (int)openf(ruta, O_ANYADIR);
        if (fd < 0) exit(1);

        for (int n = 0; n < LINEAS; n++) {
            char l[ANCHO];
            linea_de(l, h, n);
            write(fd, l, ANCHO);
            yield();                    /* dejar correr a los otros */
        }
        closefd(fd);
        exit(0);
    }

    for (int h = 0; h < HIJOS; h++)
        if (hijos[h] > 0) waitpid((uint64_t)hijos[h]);

    int rotas, por_hijo[HIJOS];
    long bytes = revisar_fichero(ruta, &rotas, por_hijo);
    informe("con O_ANYADIR (cada hijo abre el suyo):", bytes, rotas, por_hijo);

    int bien = (bytes == (long)HIJOS * LINEAS * ANCHO) && rotas == 0;
    for (int i = 0; i < HIJOS; i++) if (por_hijo[i] != LINEAS) bien = 0;
    printf("    %s\n", bien ? "ok: no se ha perdido nada" : "MAL: falta algo");
}

/* --- Lo que pasa sin ello: preguntar donde acaba y escribir ahi.
 *
 * Aqui los hijos comparten UN descriptor heredado del padre, y cada uno
 * hace lo que escribiria cualquiera que no supiera que existe O_ANYADIR:
 * un lseek al final, y luego un write. Dos operaciones.
 *
 * El sleep de en medio no fabrica el fallo, ensancha su ventana. Sin el,
 * el fallo sigue estando y aparece cuando le apetece, que en una prueba
 * es peor que no aparecer: convierte un error en un misterio. Lo que hace
 * el sleep es que el resultado sea el mismo todas las veces.
 *
 * Y fijate en lo que NO es el problema: no es compartir el descriptor. Si
 * estos hijos escribieran sin el lseek, compartiendo el desplazamiento,
 * saldria bien. Es el lseek -la pregunta- lo que rompe la escritura en
 * dos, y lo habiamos puesto para ir sobre seguro. */
static void sin_anyadir(const char *ruta)
{
    unlink(ruta);

    int fd = (int)openf(ruta, O_ESCRIBIR);
    if (fd < 0) { printf("  no puedo crear %s\n", ruta); return; }

    int64_t hijos[HIJOS];

    for (int h = 0; h < HIJOS; h++) {
        int64_t pid = fork();
        if (pid != 0) { hijos[h] = pid; continue; }

        for (int n = 0; n < LINEAS; n++) {
            char l[ANCHO];
            linea_de(l, h, n);

            int64_t fin = lseek(fd, 0, DESDE_FINAL);   /* ¿donde acaba? */
            sleep(1);                                  /* ...y ya no acaba ahi */
            lseek(fd, fin, DESDE_INICIO);
            write(fd, l, ANCHO);
        }
        exit(0);
    }

    for (int h = 0; h < HIJOS; h++)
        if (hijos[h] > 0) waitpid((uint64_t)hijos[h]);
    closefd(fd);

    int rotas, por_hijo[HIJOS];
    long bytes = revisar_fichero(ruta, &rotas, por_hijo);
    informe("con lseek al final y luego write:", bytes, rotas, por_hijo);

    long esperado = (long)HIJOS * LINEAS * ANCHO;
    printf("    %s\n", bytes < esperado
           ? "y eso es lo que tenia que pasar: se han perdido lineas"
           : "esta vez ha colado, que es lo peor que puede hacer un fallo asi");
}

/* --- Que ">>" no vacia, y que ftell lo sabe desde el principio --------- */
static void basico(const char *ruta)
{
    unlink(ruta);

    FILE *f = fopen(ruta, "w");
    if (!f) { printf("  MAL: no puedo crear: %s\n", strerror(errno)); return; }
    fputs("uno\n", f);
    fclose(f);

    f = fopen(ruta, "a");
    if (!f) { printf("  MAL: no puedo anyadir: %s\n", strerror(errno)); return; }

    /* Recien abierto en modo 'a', ftell tiene que decir el tamanyo. Si
     * dijera 0 no seria un detalle: significaria que el descriptor cree
     * estar al principio. */
    long donde = ftell(f);
    printf("  ftell nada mas abrir con \"a\" dice %ld: %s\n",
           donde, donde == 4 ? "ok" : "MAL (tendria que ser 4)");

    fputs("dos\n", f);
    fclose(f);

    f = fopen(ruta, "r");
    char todo[32];
    size_t n = f ? fread(todo, 1, sizeof(todo) - 1, f) : 0;
    todo[n] = 0;
    if (f) fclose(f);

    printf("  \"a\" no vacio lo que habia: %s\n",
           strcmp(todo, "uno\ndos\n") == 0 ? "ok" : "MAL");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n  --- anyadir al final ---\n");
    basico("/anyade.txt");

    printf("\n  --- tres hijos, un fichero ---\n");
    con_anyadir("/anyade1.txt");
    sin_anyadir("/anyade2.txt");

    printf("\n");
    return 0;
}
