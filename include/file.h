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

enum { F_LIBRE = 0, F_CONSOLA, F_PIPE_R, F_PIPE_W, F_FICHERO };

#define FICH_NOMBRE  16

struct fichero {
    int          tipo;
    int          refs;               /* cuantos descriptores apuntan aqui */
    struct pipe *p;

    /* Solo para F_FICHERO. El servidor de ficheros no tiene open ni close
     * -cada peticion lleva el nombre y el desplazamiento-, asi que un
     * descriptor de fichero abierto es exactamente esto: un nombre y por
     * donde vamos. No hay nada que cerrar al otro lado. */
    char         nombre[FICH_NOMBRE];
    uint64_t     off;
};

void    file_init(void);
struct fichero *file_consola(void);

int64_t file_read(struct fichero *f, uint64_t uva, uint64_t n);
int64_t file_write(struct fichero *f, uint64_t uva, uint64_t n);
void    file_dup(struct fichero *f);
void    file_close(struct fichero *f);

/* Abre un fichero de la tarjeta. modo es O_LEER u O_ESCRIBIR. */
struct fichero *file_open(const char *nombre, int modo);

/* Crea una tuberia y devuelve sus dos extremos. 0 si va bien. */
int     file_pipe(struct fichero **lectura, struct fichero **escritura);
