/* user/sh.c - Un interprete de ordenes, en espacio de usuario
 *
 * Es el primer programa que ata todo lo demas. No tiene ningun privilegio
 * especial y no sabe hacer nada por si mismo:
 *
 *   lee una linea           con SYS_read
 *   busca el programa       preguntandole al servidor de ficheros
 *   lo arranca              con spawn(), pasandole la linea de argumentos
 *   espera a que termine    con waitpid()
 *
 * Todas las ordenes son programas de la tarjeta. No hay ninguna "dentro
 * del shell", y eso no es purismo: significa que anyadir una orden es
 * copiar un fichero a la SD, sin tocar ni recompilar nada.
 */
#include "syscall.h"
#include "fs_abi.h"

#define MAX_LINEA   128

static struct message m;
static char           linea[MAX_LINEA];
static char           nombre[32];
static int64_t        mi_puerto;



/* --- Leer una linea, con eco y borrado ------------------------------- */
/* El eco lo hace el shell, no el kernel: quien lee es quien decide como
 * se ve lo que se escribe. */
/* Devuelve los caracteres leidos, o -1 si se ha acabado la entrada.
 *
 * Esa segunda posibilidad no existia hasta que read() pudo fallar. Sin
 * distinguirla, un shell cuya entrada se cierra se queda dando vueltas
 * imprimiendo prompts vacios para siempre. */
static int64_t leer_linea(void)
{
    uint64_t n = 0;

    for (;;) {
        int k = kgetc();
        if (k < 0) return -1;                    /* se acabo la entrada */
        char c = (char)k;

        if (c == '\r' || c == '\n') {
            kprint("\n");
            linea[n] = 0;
            return (int64_t)n;
        }

        if (c == 8 || c == 127) {            /* retroceso */
            if (n) {
                n--;
                kprint("\b \b");             /* borrar en pantalla */
            }
            continue;
        }

        if (c >= ' ' && n < MAX_LINEA - 1) {
            linea[n++] = c;
            char eco[2] = { c, 0 };
            kprint(eco);
        }
    }
}

/* --- El nombre del fichero -------------------------------------------
 * FAT16 guarda los nombres en mayusculas y en formato 8.3, asi que
 * "cat" se escribe "CAT.ELF" en la tarjeta. Traducirlo aqui es lo que
 * permite escribir en minusculas y sin extension. */
static void nombre_de(const char *orden)
{
    uint64_t o = 0;
    int      punto = 0;

    for (uint64_t i = 0; orden[i] && o < sizeof(nombre) - 5; i++) {
        char c = orden[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == '.') punto = 1;
        nombre[o++] = c;
    }

    if (!punto) {
        nombre[o++] = '.'; nombre[o++] = 'E';
        nombre[o++] = 'L'; nombre[o++] = 'F';
    }
    nombre[o] = 0;
}

/* --- Traer el programa de la tarjeta ---------------------------------
 * Devuelve los bytes leidos y deja la imagen en *img, del monton. Y deja
 * en 'motivo' POR QUE si no ha podido: decir "no encuentro" cuando lo que
 * pasa es que el servidor no esta arrancado manda a buscar el fallo al
 * sitio equivocado. */
static const char *motivo;

static uint64_t tamano_de(const char *fichero)
{
    struct fs_request r;
    r.port = (unsigned long)mi_puerto;
    r.arg  = 0;
    for (int i = 0; i < FS_NAME_MAX; i++) r.name[i] = 0;
    ucopy(r.name, fichero, ustrlen(fichero) + 1);

    m.type = FS_SIZE;
    m.len  = sizeof(r);
    ucopy(m.data, (const char *)&r, sizeof(r));

    if (msg_send(PORT_FILES, &m) < 0) {
        motivo = "no hay servidor de ficheros: arrancalo con 'f'";
        return 0;
    }
    if (msg_recv((uint64_t)mi_puerto, &m) < 0) {
        motivo = "el servidor de ficheros no ha contestado";
        return 0;
    }
    if (m.type != FS_OK) {
        motivo = "no esta en la tarjeta";
        return 0;
    }
    return ((struct fs_info *)m.data)->size;
}

static uint64_t cargar(const char *fichero, unsigned char **img)
{
    motivo = "";
    *img   = 0;

    uint64_t tam = tamano_de(fichero);
    if (!tam) { if (!motivo[0]) motivo = "esta vacio"; return 0; }

    unsigned char *p = malloc(tam);
    if (!p) { motivo = "no me cabe en memoria"; return 0; }

    uint64_t total = 0;
    while (total < tam) {
        struct fs_request r;
        r.port = (unsigned long)mi_puerto;
        r.arg  = total;
        ucopy(r.name, fichero, ustrlen(fichero) + 1);

        m.type = FS_READ;
        m.len  = sizeof(r);
        ucopy(m.data, (const char *)&r, sizeof(r));

        if (msg_send(PORT_FILES, &m) < 0 ||
            msg_recv((uint64_t)mi_puerto, &m) < 0) {
            motivo = "el servidor de ficheros se ha ido a mitad";
            break;
        }
        if (m.type != FS_OK || m.len == 0) break;

        uint64_t n = m.len;
        if (total + n > tam) n = tam - total;
        for (uint64_t i = 0; i < n; i++)
            p[total + i] = (unsigned char)m.data[i];
        total += n;
    }

    if (!total) { free(p); motivo = "esta vacio"; return 0; }

    *img = p;
    return total;
}

/* --- Partir una orden ------------------------------------------------ */

/* Quita espacios de los dos extremos, ahi mismo. */
static char *limpiar(char *s)
{
    while (*s == ' ') s++;
    uint64_t n = ustrlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    return s;
}

/* Deja en 'nombre' el fichero que hay que cargar para esa orden. */
static int fichero_de(const char *orden)
{
    char palabra[32];
    uint64_t p = 0;

    while (*orden == ' ') orden++;
    while (*orden && *orden != ' ' && p < sizeof(palabra) - 1)
        palabra[p++] = *orden++;
    palabra[p] = 0;

    if (!p) return 0;
    nombre_de(palabra);
    return 1;
}

static void quejarse(const char *que)
{
    kprint("  ");
    kprint(que);
    kprint(": ");
    kprint(motivo);
    kprint("\n");
}

/* --- Ejecutar -------------------------------------------------------- */

static void una(char *orden)
{
    if (!fichero_de(orden)) return;

    unsigned char *img;
    uint64_t bytes = cargar(nombre, &img);
    if (!bytes) { quejarse(nombre); return; }

    int64_t pid = fork();
    if (pid == 0) {
        exec(img, bytes, orden);
        kprint("  no he podido convertirme en el programa\n");
        exit(1);
    }

    free(img);
    if (pid < 0) { kprint("  no he podido bifurcarme\n"); return; }

    /* El codigo de salida del hijo. Solo se dice si no es cero, que es
     * como se comporta cualquier shell: lo normal no se anuncia. */
    int64_t codigo = waitpid((uint64_t)pid);
    if (codigo != 0) {
        char b[24];
        uint64_t n = udec(b, (uint64_t)(codigo < 0 ? -codigo : codigo));
        b[n] = 0;
        kprint("  [salida ");
        if (codigo < 0) kprint("-");
        kprint(b);
        kprint("]\n");
    }
}

/* Dos programas encadenados.
 *
 * El shell monta la tuberia ANTES de bifurcarse, asi que los dos hijos la
 * heredan ya puesta. Cada uno se queda con su extremo en el descriptor
 * que le toca -el 1 para quien escribe, el 0 para quien lee- y cierra los
 * dos originales.
 *
 * Cerrarlos importa mas de lo que parece: mientras quede UN descriptor de
 * escritura abierto en cualquier proceso, quien lee no vera nunca el final
 * del fichero y se quedara esperando para siempre. Por eso el padre
 * tambien cierra los suyos.
 */
static void tuberia(char *izq, char *der)
{
    if (!fichero_de(izq)) return;

    char nombre_izq[32];
    ucopy(nombre_izq, nombre, ustrlen(nombre) + 1);

    unsigned char *img1;
    uint64_t b1 = cargar(nombre_izq, &img1);
    if (!b1) { quejarse(nombre_izq); return; }

    if (!fichero_de(der)) { free(img1); return; }

    unsigned char *img2;
    uint64_t b2 = cargar(nombre, &img2);
    if (!b2) { quejarse(nombre); free(img1); return; }

    int fds[2];
    if (pipe(fds) < 0) {
        kprint("  no hay tuberias libres\n");
        free(img1); free(img2);
        return;
    }

    int64_t p1 = fork();
    if (p1 == 0) {
        dup2(fds[1], 1);                 /* mi salida es la tuberia */
        closefd(fds[0]);
        closefd(fds[1]);
        exec(img1, b1, izq);
        exit(1);
    }

    int64_t p2 = fork();
    if (p2 == 0) {
        dup2(fds[0], 0);                 /* mi entrada es la tuberia */
        closefd(fds[0]);
        closefd(fds[1]);
        exec(img2, b2, der);
        exit(1);
    }

    /* El padre no usa la tuberia, y si no la suelta el lector nunca vera
     * el final. */
    closefd(fds[0]);
    closefd(fds[1]);
    free(img1);
    free(img2);

    if (p1 > 0) waitpid((uint64_t)p1);
    if (p2 > 0) waitpid((uint64_t)p2);
}

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    (void)argc; (void)argv;

    mi_puerto = port_create(-1);
    if (mi_puerto < 0) { kprint("  [sh] sin puertos\n"); exit(1); }

    kprint("\n  TinyOS. Las ordenes son programas de la tarjeta,\n");
    kprint("  se arrancan con fork + exec, y se encadenan con |\n");
    kprint("  Prueba: ls / cat hola.txt | upper / cat hola.txt | wc / salir\n");

    for (;;) {
        kprint("\n$ ");

        int64_t largo = leer_linea();
        if (largo < 0) {                         /* fin de la entrada */
            kprint("\n  se acabo la entrada, me voy\n");
            exit(0);
        }
        if (largo == 0) continue;

        /* ¿Hay tuberia? Se parte la linea en dos y cada mitad es una orden
         * completa, con sus propios argumentos. */
        char *der = 0;
        for (uint64_t i = 0; linea[i]; i++)
            if (linea[i] == '|') { linea[i] = 0; der = linea + i + 1; break; }

        char *izq = limpiar(linea);
        if (!*izq) continue;

        if (izq[0] == 's' && izq[1] == 'a' && !der) {   /* salir */
            kprint("  hasta luego\n");
            exit(0);
        }

        if (der) tuberia(izq, limpiar(der));
        else     una(izq);
    }
}
