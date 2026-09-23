/* file.h - Descriptores de fichero y tuberias
 *
 * Hasta aqui, un programa que queria escribir llamaba a SYS_write y el
 * kernel lo mandaba a la UART. Directo, y por eso mismo imposible de
 * redirigir: no habia ningun sitio donde decir "lo que escriba este, que
 * vaya a otro lado".
 *
 * Un descriptor es ese sitio. El programa escribe "en el 1" y quien decide
 * que es el 1 es quien lo arranco. De ahi sale todo: las tuberias, la
 * redireccion a ficheros, y que un mismo programa sirva para las dos cosas
 * sin enterarse.
 */
#pragma once
#include <stdint.h>
#include "sync.h"

#define MAX_FD     8
#define PIPE_BUF   1024

struct pipe {
    char     buf[PIPE_BUF];
    uint32_t head, tail, count;
    int      lectores, escritores;   /* extremos abiertos de cada lado */
    struct waitqueue hay_datos;
    struct waitqueue hay_hueco;
};

enum { F_LIBRE = 0, F_CONSOLA, F_PIPE_R, F_PIPE_W };

struct fichero {
    int          tipo;
    int          refs;               /* cuantos descriptores apuntan aqui */
    struct pipe *p;
};

void    file_init(void);
struct fichero *file_consola(void);

int64_t file_read(struct fichero *f, uint64_t uva, uint64_t n);
int64_t file_write(struct fichero *f, uint64_t uva, uint64_t n);
void    file_dup(struct fichero *f);
void    file_close(struct fichero *f);

/* Crea una tuberia y devuelve sus dos extremos. 0 si va bien. */
int     file_pipe(struct fichero **lectura, struct fichero **escritura);
