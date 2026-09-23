/* stdio.c - printf y compania
 *
 * Todo pasa por un solo sitio, formatear(), y lo unico que cambia es a
 * donde van los caracteres: a un descriptor o a un buffer. Esa es la
 * diferencia entre printf y snprintf, y es la unica.
 *
 * La salida a descriptor se acumula en un buffer de 128 bytes y se vacia
 * cuando se llena. Sin eso, cada caracter seria una llamada al sistema, y
 * una llamada al sistema cuesta una excepcion, un cambio de nivel y un
 * viaje por la tabla de vectores. Imprimir una linea de ochenta caracteres
 * costaria ochenta.
 */
#include "stdio.h"
#include "string.h"
#include "syscall.h"

#define VACIADO  128

struct destino {
    int     fd;          /* a donde escribir, o -1 si es a un buffer */
    char   *buf;
    size_t  cap;
    size_t  n;           /* ocupado del buffer */
    size_t  total;       /* caracteres producidos, quepan o no */
};

static void vaciar(struct destino *d)
{
    size_t o = 0;
    while (o < d->n) {
        int64_t k = write(d->fd, d->buf + o, d->n - o);
        if (k <= 0) break;                /* la salida se cerro */
        o += (size_t)k;
    }
    d->n = 0;
}

static void emitir(struct destino *d, char c)
{
    d->total++;

    if (d->fd >= 0) {
        d->buf[d->n++] = c;
        if (d->n == d->cap) vaciar(d);
        return;
    }

    /* A un buffer: si no cabe se sigue CONTANDO pero no se guarda. Es lo
     * que hace que snprintf pueda decirte cuanto sitio habria hecho
     * falta. */
    if (d->n < d->cap) d->buf[d->n++] = c;
}

static void relleno(struct destino *d, int cuantos, char c)
{
    while (cuantos-- > 0) emitir(d, c);
}

/* Una cadena fija con su anchura, para los casos raros de %f. */
static void formato_fijo(struct destino *d, const char *s, int ancho, int izq)
{
    int n = (int)strlen(s);
    if (!izq) relleno(d, ancho - n, ' ');
    while (*s) emitir(d, *s++);
    if (izq)  relleno(d, ancho - n, ' ');
}

/* Un numero, ya sin signo, en la base que sea. El signo se trata fuera
 * porque el relleno con ceros va DETRAS del '-': "-007", no "00-7". */
static void numero(struct destino *d, unsigned long v, unsigned base,
                   int mayus, int neg, int ancho, int ceros, int izq)
{
    char digitos[24];
    int  n = 0;

    const char *tabla = mayus ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) digitos[n++] = '0';
    while (v) { digitos[n++] = tabla[v % base]; v /= base; }

    int largo = n + (neg ? 1 : 0);
    int hueco = ancho - largo;

    if (!izq && !ceros) relleno(d, hueco, ' ');
    if (neg) emitir(d, '-');
    if (!izq && ceros)  relleno(d, hueco, '0');

    while (n) emitir(d, digitos[--n]);

    if (izq) relleno(d, hueco, ' ');
}

static void formatear(struct destino *d, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emitir(d, *fmt); continue; }
        fmt++;

        /* Banderas */
        int izq = 0, ceros = 0;
        for (;; fmt++) {
            if (*fmt == '-')      izq = 1;
            else if (*fmt == '0') ceros = 1;
            else break;
        }

        /* Anchura */
        int ancho = 0;
        while (*fmt >= '0' && *fmt <= '9') ancho = ancho * 10 + (*fmt++ - '0');

        /* Precision: decimales en %f, o cuantos caracteres como mucho en
         * %s. Menos 1 quiere decir "no la han dicho". */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        /* Longitud. Hace falta de verdad: en esta maquina un int son 32
         * bits y un puntero o un uint64_t son 64, y sacar el argumento
         * equivocado de la pila no desplaza medio numero, desplaza todos
         * los siguientes. */
        int largo = 0;
        while (*fmt == 'l') { largo++; fmt++; }

        switch (*fmt) {
        case 'd':
        case 'i': {
            long v = largo ? va_arg(ap, long) : (long)va_arg(ap, int);
            unsigned long m = (v < 0) ? (unsigned long)(-(v + 1)) + 1
                                      : (unsigned long)v;
            numero(d, m, 10, 0, v < 0, ancho, ceros, izq);
            break;
        }
        case 'u': {
            unsigned long v = largo ? va_arg(ap, unsigned long)
                                    : (unsigned long)va_arg(ap, unsigned int);
            numero(d, v, 10, 0, 0, ancho, ceros, izq);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long v = largo ? va_arg(ap, unsigned long)
                                    : (unsigned long)va_arg(ap, unsigned int);
            numero(d, v, 16, *fmt == 'X', 0, ancho, ceros, izq);
            break;
        }
        case 'p': {
            unsigned long v = (unsigned long)va_arg(ap, void *);
            emitir(d, '0'); emitir(d, 'x');
            numero(d, v, 16, 0, 0, 16, 1, 0);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!izq) relleno(d, ancho - 1, ' ');
            emitir(d, c);
            if (izq)  relleno(d, ancho - 1, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(nulo)";
            int n = (int)strlen(s);
            if (prec >= 0 && prec < n) n = prec;      /* como mucho prec */
            if (!izq) relleno(d, ancho - n, ' ');
            for (int i = 0; i < n; i++) emitir(d, s[i]);
            if (izq)  relleno(d, ancho - n, ' ');
            break;
        }

        /* %f es la unica conversion que obliga a este fichero a usar la
         * FPU de verdad, y por eso lib/ se compila sin
         * -mgeneral-regs-only. El kernel, que si lo lleva, no podria tener
         * un printf con decimales aunque quisiera. */
        case 'f': {
            double v = va_arg(ap, double);
            if (prec < 0) prec = 6;

            int neg = 0;
            if (v < 0) { neg = 1; v = -v; }

            /* NaN no es igual a si mismo: es la unica forma de detectarlo
             * sin funciones de <math.h>, que no tenemos. */
            if (v != v)        { formato_fijo(d, "nan", ancho, izq); break; }
            if (v > 1.7e308)   { formato_fijo(d, neg ? "-inf" : "inf", ancho, izq); break; }

            /* Redondear ANTES de partir. Sin esto, 0.9999 con dos
             * decimales sale "0.99" en vez de "1.00": truncar no es
             * redondear. */
            double mitad = 0.5;
            for (int i = 0; i < prec; i++) mitad /= 10.0;
            v += mitad;

            /* Por encima de 2^64 la parte entera ya no cabe en un entero y
             * esto daria un numero inventado. Mejor decirlo. */
            if (v >= 18446744073709551616.0) {
                formato_fijo(d, neg ? "-enorme" : "enorme", ancho, izq);
                break;
            }

            unsigned long entero = (unsigned long)v;
            double        resto  = v - (double)entero;

            /* Se formatea primero en memoria y luego se rellena, porque la
             * anchura hay que contarla sobre el numero YA escrito y aqui no
             * se sabe de antemano cuantos digitos tiene la parte entera.
             * El truco es que el destino intermedio es un destino normal:
             * la misma maquinaria que usa snprintf. */
            char tmp[80];
            struct destino t = { -1, tmp, sizeof(tmp) - 1, 0, 0 };

            if (neg) emitir(&t, '-');
            numero(&t, entero, 10, 0, 0, 0, 0, 0);
            if (prec > 0) {
                emitir(&t, '.');
                for (int i = 0; i < prec; i++) {
                    resto *= 10.0;
                    int dig = (int)resto;
                    if (dig > 9) dig = 9;
                    emitir(&t, (char)('0' + dig));
                    resto -= dig;
                }
            }
            tmp[t.n] = 0;

            formato_fijo(d, tmp, ancho, izq);
            break;
        }
        case '%':
            emitir(d, '%');
            break;
        case 0:
            /* Un '%' al final de la cadena. Salirse sin leer el byte
             * siguiente, que ya no es nuestro. */
            return;
        default:
            /* Una conversion que no conocemos: escribirla tal cual, que se
             * vea. Tragarsela en silencio esconde el error. */
            emitir(d, '%');
            emitir(d, *fmt);
            break;
        }
    }
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct destino d = { -1, buf, cap ? cap - 1 : 0, 0, 0 };
    formatear(&d, fmt, ap);
    if (cap) buf[d.n] = 0;
    return (int)d.total;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    char acumulado[VACIADO];
    struct destino d = { 1, acumulado, VACIADO, 0, 0 };

    va_list ap;
    va_start(ap, fmt);
    formatear(&d, fmt, ap);
    va_end(ap);

    vaciar(&d);
    return (int)d.total;
}

int putchar(int c)
{
    char b = (char)c;
    return (write(1, &b, 1) == 1) ? c : -1;
}

int puts(const char *s)
{
    size_t n = strlen(s), o = 0;
    while (o < n) {
        int64_t k = write(1, s + o, n - o);
        if (k <= 0) return -1;
        o += (size_t)k;
    }
    return putchar('\n');
}

int getchar(void)
{
    char c;
    return (read(0, &c, 1) == 1) ? (int)(unsigned char)c : -1;
}
