/* stdlib.c - Lo que no cabe en otro sitio */
#include "stdlib.h"
#include <stdint.h>
#include "syscall.h"

void exit(int codigo)
{
    syscall2(SYS_exit, (uint64_t)codigo, 0);

    /* El kernel no devuelve de SYS_exit, pero el compilador no lo sabe y
     * noreturn le ha prometido que aqui no se sigue. */
    for (;;) ;
}

/* --- El entorno -------------------------------------------------------
 *
 * Hay un tope de entradas y un almacen fijo, y es a proposito: un entorno
 * que crece sin limite necesita un asignador detras, y el asignador
 * necesita que el entorno ya funcione para saber donde esta el monton.
 * Con un array se rompe esa pescadilla.
 *
 * La copia se hace la primera vez que alguien escribe: hasta entonces se
 * usa tal cual el que dejo el kernel en la pila. Quien solo lee -que son
 * casi todos- no paga nada. */
#define ENV_MAX      16
#define ENV_BYTES   512

char **environ;                       /* lo rellena crt0 */

static char  *env_mio[ENV_MAX + 1];
static char   env_texto[ENV_BYTES];
static uint64_t env_usado;
static int    env_copiado;

/* ¿Empieza esta entrada por "nombre="? */
static int casa(const char *entrada, const char *nombre)
{
    uint64_t i = 0;
    while (nombre[i] && entrada[i] == nombre[i]) i++;
    return !nombre[i] && entrada[i] == '=';
}

const char *getenv(const char *nombre)
{
    if (!environ) return 0;

    for (int i = 0; environ[i]; i++)
        if (casa(environ[i], nombre)) {
            const char *p = environ[i];
            while (*p != '=') p++;
            return p + 1;
        }
    return 0;
}

/* Guardar "nombre=valor" en el almacen propio y devolver donde quedo. */
static char *guardar(const char *nombre, const char *valor)
{
    uint64_t n = 0, v = 0;
    while (nombre[n]) n++;
    while (valor[v]) v++;

    if (env_usado + n + 1 + v + 1 > ENV_BYTES) return 0;

    char *dst = env_texto + env_usado;
    for (uint64_t i = 0; i < n; i++) env_texto[env_usado++] = nombre[i];
    env_texto[env_usado++] = '=';
    for (uint64_t i = 0; i < v; i++) env_texto[env_usado++] = valor[i];
    env_texto[env_usado++] = 0;
    return dst;
}

int setenv(const char *nombre, const char *valor)
{
    if (!nombre || !*nombre || !valor) return -1;

    /* La primera escritura se lleva el entorno a memoria propia. El que
     * dejo el kernel vive en la pila y no se puede hacer crecer. */
    if (!env_copiado) {
        int n = 0;
        if (environ)
            for (; environ[n] && n < ENV_MAX; n++) env_mio[n] = environ[n];
        env_mio[n] = 0;
        environ = env_mio;
        env_copiado = 1;
    }

    char *nuevo = guardar(nombre, valor);
    if (!nuevo) return -1;

    for (int i = 0; environ[i]; i++)
        if (casa(environ[i], nombre)) { environ[i] = nuevo; return 0; }

    int n = 0;
    while (environ[n]) n++;
    if (n >= ENV_MAX) return -1;

    environ[n]     = nuevo;
    environ[n + 1] = 0;
    return 0;
}

/* --- Ordenar ----------------------------------------------------------
 *
 * Quicksort con la mediana de tres. Elegir el pivote asi no es un adorno:
 * el quicksort de libro -pivote el primero- se vuelve CUADRATICO justo
 * con lo que mas aparece en la vida real, que son datos ya ordenados o
 * casi. Mirar tres y quedarse con el de en medio cuesta dos comparaciones
 * y quita ese caso.
 *
 * Los elementos se mueven byte a byte porque aqui no se sabe lo que son.
 * Esa es toda la diferencia entre un qsort de libreria y uno escrito para
 * un tipo concreto: el de libreria no puede usar el asignador de
 * estructuras del compilador, tiene que copiar a mano.
 */
static void intercambiar(char *a, char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) { char t = a[i]; a[i] = b[i]; b[i] = t; }
}

void qsort(void *base, size_t n, size_t tam,
           int (*comparar)(const void *, const void *))
{
    if (n < 2 || !tam) return;

    char *v = base;

    /* Tramos pequenyos: insercion. Es O(n^2) y es MAS RAPIDA aqui, porque
     * no tiene la ceremonia de la recursion y los datos ya estan en
     * cache. El umbral clasico anda por ocho. */
    if (n <= 8) {
        for (size_t i = 1; i < n; i++)
            for (size_t j = i; j > 0 && comparar(v + (j - 1) * tam,
                                                 v + j * tam) > 0; j--)
                intercambiar(v + (j - 1) * tam, v + j * tam, tam);
        return;
    }

    /* Mediana de tres: primero, medio y ultimo. El elegido se pone al
     * final para que el bucle de particion no tenga que esquivarlo. */
    size_t medio = n / 2;
    char *a = v, *b = v + medio * tam, *c = v + (n - 1) * tam;

    if (comparar(a, b) > 0) intercambiar(a, b, tam);
    if (comparar(b, c) > 0) {
        intercambiar(b, c, tam);
        if (comparar(a, b) > 0) intercambiar(a, b, tam);
    }
    intercambiar(b, c, tam);                  /* el pivote, al final */

    char *pivote = v + (n - 1) * tam;
    size_t frontera = 0;

    for (size_t i = 0; i < n - 1; i++)
        if (comparar(v + i * tam, pivote) < 0)
            intercambiar(v + i * tam, v + frontera++ * tam, tam);

    intercambiar(v + frontera * tam, pivote, tam);

    qsort(v, frontera, tam, comparar);
    qsort(v + (frontera + 1) * tam, n - frontera - 1, tam, comparar);
}

void *bsearch(const void *clave, const void *base, size_t n, size_t tam,
              int (*comparar)(const void *, const void *))
{
    const char *v = base;
    size_t bajo = 0, alto = n;

    while (bajo < alto) {
        /* bajo + (alto-bajo)/2 y no (bajo+alto)/2: la suma puede
         * desbordar con arrays enormes. Es el fallo que estuvo veinte
         * anyos en la busqueda binaria de la biblioteca de Java. */
        size_t medio = bajo + (alto - bajo) / 2;

        int r = comparar(clave, v + medio * tam);
        if (r == 0) return (void *)(v + medio * tam);
        if (r < 0)  alto = medio;
        else        bajo = medio + 1;
    }
    return 0;
}

/* --- De texto a numero ------------------------------------------------
 *
 * strtol es lo que atoi deberia haber sido: dice donde se paro, entiende
 * bases, y se puede saber si leyo algo. atoi no puede distinguir "0" de
 * "hola", y por eso sigue habiendo programas que tratan una entrada mala
 * como un cero.
 */
static int digito_de(char c, int base)
{
    int v;
    if (c >= '0' && c <= '9')      v = c - '0';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    else return -1;

    return v < base ? v : -1;
}

unsigned long strtoul(const char *s, char **fin, int base)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;

    int negativo = 0;
    if (*p == '+') p++;
    else if (*p == '-') { negativo = 1; p++; }

    /* "0x" para hexadecimal y "0" para octal, pero solo si la base lo
     * permite o no se dijo ninguna. Con base 16 el "0x" es opcional. */
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
        && digito_de(p[2], 16) >= 0) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (p[0] == '0' && digito_de(p[1], 8) >= 0) ? 8 : 10;
    }

    unsigned long v = 0;
    const char *primero = p;

    for (int d; (d = digito_de(*p, base)) >= 0; p++)
        v = v * (unsigned long)base + (unsigned long)d;

    /* Si no se leyo ni un digito, 'fin' vuelve al principio DE TODO, no a
     * donde se quedo el analisis. Asi el que llama puede comprobar
     * fin == s y saber que no habia numero. */
    if (fin) *fin = (char *)(p == primero ? s : p);

    return negativo ? (unsigned long)(-(long)v) : v;
}

long strtol(const char *s, char **fin, int base)
{
    return (long)strtoul(s, fin, base);
}

/* Sin errno, sin detectar desbordamiento y parando en el primer caracter
 * que no sea un digito. Es la de toda la vida: comoda y traicionera. */
long atol(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;

    int signo = 1;
    if (*s == '-') { signo = -1; s++; }
    else if (*s == '+') s++;

    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v * signo;
}

int atoi(const char *s) { return (int)atol(s); }

int abs(int v) { return v < 0 ? -v : v; }
