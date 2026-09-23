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
#define MAX_IMG     (32 * 1024)

static struct message m;
static char           linea[MAX_LINEA];
static char           nombre[32];
static unsigned char  imagen[MAX_IMG];
static int64_t        mi_puerto;

static void dec(uint64_t v)
{
    char n[24];
    uint64_t l = udec(n, v);
    n[l] = 0;
    kprint(n);
}

/* --- Leer una linea, con eco y borrado ------------------------------- */
/* El eco lo hace el shell, no el kernel: quien lee es quien decide como
 * se ve lo que se escribe. */
static uint64_t leer_linea(void)
{
    uint64_t n = 0;

    for (;;) {
        char c = kgetc();

        if (c == '\r' || c == '\n') {
            kprint("\n");
            linea[n] = 0;
            return n;
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
 * Devuelve los bytes leidos, o 0. Y deja en 'motivo' POR QUE: decir
 * "no encuentro" cuando lo que pasa es que el servidor no esta arrancado
 * manda a buscar el fallo al sitio equivocado. */
static const char *motivo;

static uint64_t cargar(const char *fichero)
{
    uint64_t total = 0;
    motivo = "";

    while (total < MAX_IMG) {
        struct fs_request r;
        r.port = (unsigned long)mi_puerto;
        r.arg  = total;
        ucopy(r.name, fichero, ustrlen(fichero) + 1);

        m.type = FS_READ;
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
        if (m.type == FS_ERROR) {
            motivo = "no esta en la tarjeta";
            return 0;
        }
        if (m.type != FS_OK || m.len == 0) break;   /* fin del fichero */

        for (uint64_t i = 0; i < m.len; i++)
            imagen[total + i] = (unsigned char)m.data[i];
        total += m.len;
    }

    if (!total) motivo = "esta vacio";
    return total;
}

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    (void)argc; (void)argv;

    mi_puerto = port_create(-1);
    if (mi_puerto < 0) { kprint("  [sh] sin puertos\n"); exit(1); }

    kprint("\n  TinyOS. Las ordenes son programas de la tarjeta.\n");
    kprint("  Prueba: ls / cat HOLA.TXT / hello uno dos / salir\n");

    for (;;) {
        kprint("\n$ ");
        if (leer_linea() == 0) continue;

        /* La primera palabra es la orden. Se copia aparte porque la linea
         * entera se le pasa al programa como sus argumentos. */
        char orden[32];
        uint64_t o = 0;
        while (linea[o] == ' ') o++;
        uint64_t p = 0;
        while (linea[o] && linea[o] != ' ' && p < sizeof(orden) - 1)
            orden[p++] = linea[o++];
        orden[p] = 0;
        if (!p) continue;

        if (orden[0] == 's' && orden[1] == 'a') {      /* salir */
            kprint("  hasta luego\n");
            exit(0);
        }

        nombre_de(orden);

        uint64_t bytes = cargar(nombre);
        if (!bytes) {
            kprint("  ");
            kprint(nombre);
            kprint(": ");
            kprint(motivo);
            kprint("\n");
            continue;
        }

        int64_t pid = spawn(imagen, bytes, linea);
        if (pid < 0) {
            kprint("  el kernel no ha querido arrancarlo\n");
            continue;
        }

        /* Esperar a que acabe. Sin esto el prompt volveria antes de que el
         * programa hubiera abierto la boca. */
        waitpid((uint64_t)pid);
        (void)dec;
    }
}
