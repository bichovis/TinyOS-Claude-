/* stdlib.c - Lo que no cabe en otro sitio */
#include "stdlib.h"
#include "syscall.h"

void exit(int codigo)
{
    syscall2(SYS_exit, (uint64_t)codigo, 0);

    /* El kernel no devuelve de SYS_exit, pero el compilador no lo sabe y
     * noreturn le ha prometido que aqui no se sigue. */
    for (;;) ;
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
