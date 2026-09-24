/* file.c - Ficheros con buffer
 *
 * Un FILE es un descriptor con un cubo delante. Eso es todo.
 *
 * Lo que tiene miga es CUANDO se vacia el cubo, porque de esa decision
 * salen tanto el rendimiento como el fallo mas viejo de depurar con
 * printf.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "syscall.h"

/* --- Modos -------------------------------------------------------------
 * LINEA quiere decir "vacia al llegar un salto de linea". Es lo que hace
 * stdout cuando habla con un terminal: alguien esta MIRANDO, y una salida
 * que aparece a trozos de 512 bytes no sirve para seguir un programa.
 *
 * Hacia un fichero no hay nadie mirando, asi que se llena el cubo entero.
 * La misma funcion se comporta distinto segun a donde escriba, y eso no
 * es una inconsistencia: es que el criterio es "que le sirve a quien
 * lee". */
#define M_LEER      1
#define M_ESCRIBIR  2
#define M_LINEA     4
#define M_SINBUF    8
#define M_EOF      16
#define M_ERROR    32
#define M_MIO      64      /* la ranura esta ocupada */
#define M_SINDECIDIR 128   /* aun no sabemos si hay alguien mirando */

#define MAX_ABIERTOS 8

static FILE abiertos[MAX_ABIERTOS];

/* Los tres de siempre. stderr SIN buffer, a proposito: es para lo que
 * tienes que ver aunque el programa se este cayendo. */
static FILE _stdin  = { 0, M_LEER | M_MIO | M_SINDECIDIR, 0, 0, { 0 } };
static FILE _stdout = { 1, M_ESCRIBIR | M_MIO | M_SINDECIDIR, 0, 0, { 0 } };
static FILE _stderr = { 2, M_ESCRIBIR | M_SINBUF | M_MIO, 0, 0, { 0 } };

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

/* --- Quien hay al otro lado -------------------------------------------
 *
 * No hay syscall para preguntar "esto es un terminal?", y no hace falta
 * inventarla: basta con intentar moverse por el fichero. Un fichero de
 * verdad te deja; la consola no, porque no tiene posicion. La respuesta
 * llega por el camino de atras, como efecto secundario de una operacion
 * que iba a otra cosa.
 *
 * errno se guarda y se repone: esto es una pregunta, no un fallo, y el
 * programa no tiene por que enterarse de que la hemos hecho. */
int isatty(int fd)
{
    int guardado = errno;
    int es = lseek(fd, 0, DESDE_ACTUAL) < 0;
    errno = guardado;
    return es;
}

/* Decidir el modo de stdout la primera vez que se escribe algo.
 *
 * `prog > fichero` y `prog` a secas son el MISMO binario con el mismo
 * printf, y sin embargo uno debe vaciar por lineas y el otro por cubos
 * llenos. Lo que cambia no esta en el programa: esta en el otro extremo
 * del descriptor, y por eso la pregunta se hace aqui y no en el que
 * escribe. */
static void decidir(FILE *f)
{
    f->modo &= ~M_SINDECIDIR;
    if (!isatty(f->fd)) return;

    if (f->modo & M_ESCRIBIR) { f->modo |= M_LINEA; return; }

    /* Y leer del terminal CON cubo, que hasta el paso 53 no se podia.
     *
     * El motivo de no poder nunca fue el rendimiento: era de quien son
     * los caracteres. El terminal lo comparten todos los procesos por
     * turnos, y un shell que se guardara 512 bytes "por si acaso" se
     * estaria quedando con lo que el usuario escribio para el programa
     * que viene despues, que se quedaria esperando algo que ya no llega.
     *
     * Lo que ha cambiado no esta aqui: es que ahora una lectura del
     * terminal devuelve UNA LINEA como mucho. Con esa frontera, el cubo
     * no puede robar nada, porque lo que se lleva es exactamente lo que
     * se escribio para ti.
     *
     * Asi que la regla sigue siendo la misma -de un fichero puedes leer
     * de golpe porque el fichero es tuyo- y lo que ha pasado es que ahora
     * una linea del terminal tambien lo es. */
}

/* --- Vaciar ------------------------------------------------------------ */

/* Cuantas veces hemos bajado al kernel de verdad.
 *
 * Esta a la vista a proposito. Todo el capitulo se resume en la
 * diferencia entre este numero y la cantidad de caracteres escritos, y
 * un numero que puedes imprimir convence mas que un parrafo. */
unsigned long stdio_escrituras = 0;

/* Y cuantas veces hemos bajado a LEER. Desde el paso 53 este numero mide
 * algo distinto que antes: no cuantas veces se llena el cubo, sino
 * cuantas lineas se han tecleado. */
unsigned long stdio_lecturas = 0;

static int escribir_todo(int fd, const char *s, int n)
{
    int o = 0;
    while (o < n) {
        stdio_escrituras++;
        int64_t k = write(fd, s + o, (uint64_t)(n - o));
        if (k <= 0) return -1;
        o += (int)k;
    }
    return 0;
}

int fflush(FILE *f)
{
    /* fflush(0) vacia TODOS. Es lo que llama exit(), y sin eso lo ultimo
     * que imprime un programa no llega nunca. */
    if (!f) {
        int mal = 0;
        if (fflush(stdout) < 0) mal = -1;
        if (fflush(stderr) < 0) mal = -1;
        for (int i = 0; i < MAX_ABIERTOS; i++)
            if (abiertos[i].modo & M_MIO)
                if (fflush(&abiertos[i]) < 0) mal = -1;
        return mal;
    }

    if (!(f->modo & M_ESCRIBIR) || f->n == 0) return 0;

    if (escribir_todo(f->fd, f->buf, f->n) < 0) {
        f->modo |= M_ERROR;
        f->n = 0;
        return -1;
    }
    f->n = 0;
    return 0;
}

/* --- Abrir y cerrar ---------------------------------------------------- */

FILE *fopen(const char *ruta, const char *modo)
{
    if (!modo || (modo[0] != 'r' && modo[0] != 'w' && modo[0] != 'a')) {
        errno = EINVAL;
        return 0;
    }

    FILE *f = 0;
    for (int i = 0; i < MAX_ABIERTOS; i++)
        if (!(abiertos[i].modo & M_MIO)) { f = &abiertos[i]; break; }

    if (!f) { errno = EMFILE; return 0; }

    /* Las tres letras son los tres contratos con lo que ya hubiera:
     * 'r' exige que este, 'w' lo vacia, 'a' lo respeta y escribe detras.
     * El cubo de arriba no cambia en nada: esto se decide una vez, al
     * abrir, y luego lo cumple el descriptor sin que la libc vuelva a
     * pensar en ello. */
    int64_t fd = openf(ruta, modo[0] == 'r' ? O_LEER :
                             modo[0] == 'a' ? O_ANYADIR : O_ESCRIBIR);
    if (fd < 0) return 0;                  /* openf ya puso errno */

    f->fd   = (int)fd;
    f->modo = M_MIO | (modo[0] == 'r' ? M_LEER : M_ESCRIBIR);
    f->n    = 0;
    f->pos  = 0;

    /* En modo 'a' no hay nada mas que hacer para que ftell diga el
     * tamanyo desde el primer momento: el descriptor ya viene colocado al
     * final, y ftell no guarda ninguna posicion propia -se la pregunta al
     * descriptor cada vez-. Un campo mas aqui seria una segunda copia de
     * un numero que ya existe, y las dos copias acaban discrepando. */

    return f;
}

int fclose(FILE *f)
{
    if (!f) { errno = EINVAL; return -1; }

    /* Vaciar ANTES de cerrar. Al reves, el cubo se escribiria en un
     * descriptor que ya no existe, o peor: en el que le tocara a ese
     * numero despues. */
    int r = fflush(f);

    if (closefd(f->fd) < 0) r = -1;
    f->modo = 0;                           /* la ranura, libre */
    return r;
}

/* --- Leer -------------------------------------------------------------- */

/* Llenar el cubo. Devuelve cuantos trajo, 0 al final, -1 si fallo. */
static int rellenar(FILE *f)
{
    if (f->modo & M_SINDECIDIR) decidir(f);
    if (f->modo & (M_EOF | M_ERROR)) return 0;

    /* Antes de quedarse esperando a que alguien teclee, sacar lo que haya
     * pendiente de escribir.
     *
     * Sin esto, un programa que pregunta "nombre: " sin salto de linea se
     * queda mudo esperando una respuesta a una pregunta que no ha llegado
     * a la pantalla. Es la razon por la que en C no hace falta un
     * fflush(stdout) antes de cada scanf: la lectura lo hace por ti. */
    fflush(stdout);

    stdio_lecturas++;
    int64_t k = read(f->fd, f->buf, (f->modo & M_SINBUF) ? 1 : BUFSIZ);
    if (k < 0) { f->modo |= M_ERROR; return -1; }
    if (k == 0) { f->modo |= M_EOF;  return 0; }

    f->n   = (int)k;
    f->pos = 0;
    return (int)k;
}

int fgetc(FILE *f)
{
    if (!f || !(f->modo & M_LEER)) { errno = EBADF; return -1; }

    if (f->pos >= f->n && rellenar(f) <= 0) return -1;
    return (unsigned char)f->buf[f->pos++];
}

/* Devolver un caracter al cubo.
 *
 * Solo uno, y solo si hay sitio detras. El estandar garantiza exactamente
 * eso -un ungetc- y la razon es esta: mas de uno obliga a un buffer
 * aparte, y con uno basta para lo unico que se usa de verdad, que es
 * mirar el siguiente caracter antes de decidir que hacer. */
int ungetc(int c, FILE *f)
{
    if (!f || c < 0 || f->pos == 0) return -1;
    f->buf[--f->pos] = (char)c;
    f->modo &= ~M_EOF;
    return c;
}

size_t fread(void *dst, size_t tam, size_t n, FILE *f)
{
    if (!f || !(f->modo & M_LEER) || !tam) return 0;

    char *d = dst;
    size_t total = tam * n, hechos = 0;

    while (hechos < total) {
        if (f->pos >= f->n && rellenar(f) <= 0) break;

        int hay = f->n - f->pos;
        size_t quiero = total - hechos;
        if ((size_t)hay > quiero) hay = (int)quiero;

        memcpy(d + hechos, f->buf + f->pos, (size_t)hay);
        f->pos  += hay;
        hechos  += (size_t)hay;
    }

    return hechos / tam;                   /* elementos COMPLETOS */
}

char *fgets(char *dst, int max, FILE *f)
{
    if (!dst || max < 2) return 0;

    int o = 0;
    while (o < max - 1) {
        int c = fgetc(f);
        if (c < 0) break;

        dst[o++] = (char)c;
        if (c == '\n') break;              /* el salto SE QUEDA */
    }

    if (!o) return 0;
    dst[o] = 0;
    return dst;
}

/* --- Escribir ---------------------------------------------------------- */

int fputc(int c, FILE *f)
{
    if (!f || !(f->modo & M_ESCRIBIR)) { errno = EBADF; return -1; }

    if (f->modo & M_SINDECIDIR) decidir(f);

    char b = (char)c;

    if (f->modo & M_SINBUF)
        return escribir_todo(f->fd, &b, 1) < 0 ? -1 : c;

    f->buf[f->n++] = b;

    /* Se vacia por dos motivos: porque el cubo esta lleno, o porque ha
     * llegado un salto de linea y alguien esta mirando. */
    if (f->n == BUFSIZ || ((f->modo & M_LINEA) && b == '\n'))
        if (fflush(f) < 0) return -1;

    return c;
}

size_t fwrite(const void *src, size_t tam, size_t n, FILE *f)
{
    if (!f || !(f->modo & M_ESCRIBIR) || !tam) return 0;

    const char *s = src;
    size_t total = tam * n;

    for (size_t i = 0; i < total; i++)
        if (fputc((unsigned char)s[i], f) < 0) return i / tam;

    return n;
}

int fputs(const char *s, FILE *f)
{
    for (; *s; s++)
        if (fputc(*s, f) < 0) return -1;
    return 0;
}

/* --- Moverse ----------------------------------------------------------- */

int fseek(FILE *f, long desp, int desde)
{
    if (!f) { errno = EBADF; return -1; }

    /* Lo que este en el cubo deja de valer: al escribir hay que soltarlo
     * antes de moverse, y al leer hay que tirarlo porque es de otro
     * sitio. */
    if (f->modo & M_ESCRIBIR) { if (fflush(f) < 0) return -1; }
    else                      { f->n = f->pos = 0; }

    if (lseek(f->fd, desp, desde) < 0) return -1;

    f->modo &= ~M_EOF;
    return 0;
}

long ftell(FILE *f)
{
    if (!f) { errno = EBADF; return -1; }

    int64_t donde = lseek(f->fd, 0, DESDE_ACTUAL);
    if (donde < 0) return -1;

    /* La posicion de VERDAD no es la del descriptor: hay que descontar lo
     * que queda por consumir del cubo, o sumar lo que queda por escribir.
     * Es el error que se comete siempre al escribir un ftell. */
    if (f->modo & M_LEER) return (long)donde - (f->n - f->pos);
    return (long)donde + f->n;
}

void rewind(FILE *f) { fseek(f, 0, DESDE_INICIO); }

/* --- Estado ------------------------------------------------------------ */

int  feof(FILE *f)      { return f && (f->modo & M_EOF)   ? 1 : 0; }
int  ferror(FILE *f)    { return f && (f->modo & M_ERROR) ? 1 : 0; }
void clearerr(FILE *f)  { if (f) f->modo &= ~(M_EOF | M_ERROR); }
