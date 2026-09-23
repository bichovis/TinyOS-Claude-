/* stdio.h - Salida con formato
 *
 * printf no es magia: es un bucle sobre una cadena que, al encontrar un
 * '%', saca el siguiente argumento de la pila y lo convierte a texto. Lo
 * unico que no se puede escribir en C normal es "saca el siguiente
 * argumento", y para eso estan los va_ de <stdarg.h>, que si da el
 * compilador aunque no haya libc.
 */
#pragma once
#include <stdarg.h>
#include <stddef.h>

/* El atributo no es decoracion: hace que GCC compruebe que los % cuadran
 * con los argumentos. "%d" con un puntero pasa a ser un error de
 * compilacion en vez de un numero sin sentido en pantalla. */
/* --- Ficheros con buffer ----------------------------------------------
 *
 * POR QUE EXISTEN. Cada write() es una llamada al sistema, y una llamada
 * cuesta una excepcion, un cambio de nivel de privilegio y un viaje por
 * la tabla de vectores. Escribir una linea de ochenta caracteres de uno
 * en uno son ochenta de esos viajes para mover ochenta bytes.
 *
 * Un FILE es un descriptor con un cubo delante: se va llenando y se vacia
 * de golpe. Eso es TODO lo que es.
 *
 * Y de ahi sale el fallo clasico de depurar con printf: si el programa
 * revienta, lo que estaba en el cubo no llega nunca, asi que el ultimo
 * mensaje que ves NO es el ultimo que se ejecuto. La gente lleva cincuenta
 * anyos persiguiendo fantasmas por esto.
 *
 * Por eso stderr NO lleva buffer: es para lo que tienes que ver aunque
 * todo se este cayendo.
 */
#define BUFSIZ   512

/* Abierto para leer O para escribir, no las dos cosas. "r+" obliga a
 * llevar dos posiciones y a vaciar el cubo cada vez que se cambia de
 * sentido, y no hace falta aqui. */
typedef struct _FILE {
    int      fd;
    unsigned modo;
    int      n;            /* bytes utiles en el cubo    */
    int      pos;          /* por donde vamos leyendo    */
    char     buf[BUFSIZ];
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *ruta, const char *modo);
int   fclose(FILE *f);
int   fflush(FILE *f);            /* con 0, vacia todos */

size_t fread(void *dst, size_t tam, size_t n, FILE *f);
size_t fwrite(const void *src, size_t tam, size_t n, FILE *f);

int   fgetc(FILE *f);
int   fputc(int c, FILE *f);
int   ungetc(int c, FILE *f);
char *fgets(char *dst, int max, FILE *f);
int   fputs(const char *s, FILE *f);

int   fseek(FILE *f, long desp, int desde);
long  ftell(FILE *f);
void  rewind(FILE *f);

int   isatty(int fd);             /* hay alguien mirando? */

/* Llamadas a write() que han salido de aqui, desde que arranco el
 * programa. Comparala con los caracteres que has escrito. */
extern unsigned long stdio_escrituras;
int   feof(FILE *f);
int   ferror(FILE *f);
void  clearerr(FILE *f);

int fprintf(FILE *f, const char *fmt, ...)
                                  __attribute__((format(printf, 2, 3)));
int vfprintf(FILE *f, const char *fmt, va_list ap)
                                  __attribute__((format(printf, 2, 0)));

int printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int snprintf(char *buf, size_t cap, const char *fmt, ...)
                                  __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
                                  __attribute__((format(printf, 3, 0)));

int putchar(int c);
int puts(const char *s);          /* anyade '\n', como manda el estandar */
int getchar(void);                /* -1 al final de la entrada */
