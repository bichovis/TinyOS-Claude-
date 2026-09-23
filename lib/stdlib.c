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
