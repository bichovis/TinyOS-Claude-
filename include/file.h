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
#include "fs_abi.h"

#define MAX_FD     8
#define PIPE_BUF   1024

struct pipe {
    char     buf[PIPE_BUF];
    uint32_t head, tail, count;
    int      lectores, escritores;   /* extremos abiertos de cada lado */
    struct waitqueue hay_datos;
    struct waitqueue hay_hueco;
};

enum { F_LIBRE = 0, F_CONSOLA, F_PIPE_R, F_PIPE_W, F_FICHERO, F_DIRECTORIO };

/* Un descriptor guarda la RUTA ENTERA, ya absoluta. Cabe de sobra, porque
 * el servidor no admite ninguna mas larga. */
#define FICH_NOMBRE  FS_PATH_MAX

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

/* Unir el directorio actual con una ruta relativa, y normalizar. Es
 * textual: no mira el disco. Ver src/path.c. */
int path_resolve(const char *base, const char *rel, char *out, uint64_t cap);

/* Leer de un fichero a memoria del kernel. Lo usa el fallo de pagina de un
 * fichero mapeado. */
int64_t fs_leer_en(const char *ruta, uint64_t off, char *dst, uint64_t n);
int64_t fs_tamano(const char *ruta);     /* -1 si no esta o es directorio */

/* Comprobar que una ruta absoluta nombra un directorio. Lo pregunta al
 * servidor, que es el unico que lo sabe. */
int fs_es_directorio(const char *ruta);

/* Abre un fichero de la tarjeta. modo es O_LEER u O_ESCRIBIR. */
struct fichero *file_open(const char *nombre, int modo);

/* Y un directorio, para recorrerlo. Un descriptor de directorio es lo
 * mismo que uno de fichero con otra cosa dentro: en vez de por que byte
 * vamos, por que ENTRADA vamos. */
struct fichero *file_opendir(const char *ruta);
int  file_readdir(struct fichero *f, void *info);

int64_t file_seek(struct fichero *f, int64_t desplazamiento, int desde);

/* Las operaciones sobre el nombre, que no necesitan descriptor. */
int  fs_borrar(const char *ruta);
int  fs_mkdir(const char *ruta);
int  fs_rmdir(const char *ruta);
int  fs_renombrar(const char *origen, const char *destino);
int  fs_estado(const char *ruta, uint64_t *tam, uint64_t *mtime, uint64_t *flags);

/* Crea una tuberia y devuelve sus dos extremos. 0 si va bien. */
int     file_pipe(struct fichero **lectura, struct fichero **escritura);
