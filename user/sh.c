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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include "fs_abi.h"

#define MAX_LINEA   128

static struct message m;
static char           linea[MAX_LINEA];
static char           nombre[FS_PATH_MAX];
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
        int k = getchar();
        if (k < 0) return -1;                    /* se acabo la entrada */
        char c = (char)k;

        if (c == '\r' || c == '\n') {
            printf("\n");
            linea[n] = 0;
            return (int64_t)n;
        }

        if (c == 8 || c == 127) {            /* retroceso */
            if (n) {
                n--;
                printf("\b \b");             /* borrar en pantalla */
            }
            continue;
        }

        if (c >= ' ' && n < MAX_LINEA - 1) {
            linea[n++] = c;
            char eco[2] = { c, 0 };
            printf("%s", eco);
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
    for (int i = 0; i < FS_PATH_MAX; i++) r.name[i] = 0;
    memcpy(r.name, fichero, strlen(fichero) + 1);

    m.type = FS_SIZE;
    m.len  = sizeof(r);
    memcpy(m.data, (const char *)&r, sizeof(r));

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

/* --- El PATH ----------------------------------------------------------
 *
 * Donde buscar un programa cuando lo que has escrito no lleva barras.
 *
 * Primero el directorio actual, y luego /usr/bin, que es donde viven los
 * ejecutables del sistema. Hace falta desde que hay subdirectorios: sin
 * esto, "cd docs" te dejaria sin ordenes, porque "ls" ya no estaria donde
 * estas.
 *
 * Es una lista escrita aqui y no una variable de entorno, porque este
 * sistema todavia no tiene entorno. La idea es la misma: un programa se
 * llama por su nombre y alguien decide donde se busca.
 *
 * Que el directorio actual vaya PRIMERO es comodo y en Unix no se hace:
 * ahi "." no esta en el PATH por defecto, porque entrar en un directorio
 * ajeno y escribir "ls" podria ejecutar el "ls" que haya dejado el duenyo
 * del directorio. Aqui no hay varios usuarios, asi que no hay a quien
 * enganyar. */
static const char *PATH[] = { 0, "/usr/bin" };   /* el 0 es "donde estoy" */

/* Deja en 'ruta' la absoluta que si existe, o devuelve 0. */
static int buscar_programa(const char *nom, char *ruta)
{
    for (uint64_t i = 0; i < sizeof(PATH) / sizeof(PATH[0]); i++) {
        if (!PATH[i]) {
            /* El directorio actual: de eso ya se encarga realpath. */
            if (realpath(nom, ruta) == 0 && tamano_de(ruta)) return 1;
            continue;
        }

        uint64_t d = strlen(PATH[i]), n = strlen(nom);
        if (d + 1 + n + 1 > FS_PATH_MAX) continue;

        memcpy(ruta, PATH[i], d);
        ruta[d] = '/';
        memcpy(ruta + d + 1, nom, n + 1);

        if (tamano_de(ruta)) return 1;
    }
    return 0;
}

/* Decir DONDE se ha buscado. Un "no encuentro ls" a secas manda a pensar
 * que el fichero no esta; enseñar la lista dice que quiza esta, pero en
 * otro sitio. */
static void no_esta(const char *nom)
{
    printf("  %s: no lo encuentro. He mirado en:\n", nom);
    for (uint64_t i = 0; i < sizeof(PATH) / sizeof(PATH[0]); i++) {
        if (PATH[i]) printf("    %s\n", PATH[i]);
        else {
            char aqui[FS_PATH_MAX];
            if (getcwd(aqui, sizeof(aqui)) == 0) printf("    %s\n", aqui);
        }
    }
}

/* Cargar un programa es ahora MAPEARLO.
 *
 * Aqui habia cuarenta lineas: pedir el tamanyo, reservar ese tamanyo con
 * malloc, y dar vueltas pidiendole trozos al servidor de ficheros hasta
 * llenarlo. Todo eso lo hace ya el kernel, y no porque se haya movido de
 * sitio: es que ya no hace falta. mmap devuelve una direccion sin leer
 * nada, y los trozos llegan cuando alguien los toca. El que los toca es el
 * cargador de ELF del kernel, dentro de exec.
 *
 * Lo que el shell se ahorra no es codigo, es una COPIA: antes el fichero
 * entero pasaba por un malloc suyo para que el kernel lo copiara de ahi a
 * las paginas del programa nuevo. Ahora va del servidor a esas paginas.
 */
static uint64_t cargar(const char *fichero, unsigned char **img)
{
    uint64_t tam = 0;
    const char *p = mmap(fichero, &tam);

    if (!p) { motivo = "no esta en la tarjeta"; *img = 0; return 0; }
    if (!tam) { munmap(p); motivo = "esta vacio"; *img = 0; return 0; }

    *img = (unsigned char *)p;
    return tam;
}

/* --- Partir una orden ------------------------------------------------ */

/* Quita espacios de los dos extremos, ahi mismo. */
static char *limpiar(char *s)
{
    while (*s == ' ') s++;
    uint64_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    return s;
}

/* Deja en 'nombre' el fichero que hay que cargar para esa orden. */
/* Separar la redireccion del resto de la orden.
 *
 * "cat hola.txt > salida.txt" se convierte en la orden "cat hola.txt" y el
 * destino "SALIDA.TXT". Se hace CORTANDO la cadena en el < o el >: a
 * partir de ahi el programa no vera nada, y eso es justo lo que se quiere.
 * El programa no recibe ">" como argumento porque la redireccion no es
 * asunto suyo; es un acuerdo entre el shell y el kernel sobre que hay
 * detras del 0 y del 1.
 *
 * Los nombres los pone el que llama en buffers suyos, y no en 'nombre',
 * que lo usa cargar() para el ejecutable: en "cat < a > b" hay tres
 * ficheros en juego a la vez. */



/* Aplicar la redireccion. Se llama YA EN EL HIJO, despues del fork y antes
 * del exec: si se hiciera en el padre, el shell se quedaria con el 0 o el
 * 1 apuntando al fichero y no volveria a hablar con la consola nunca. */
static int aplicar(int hay, const char *ent, const char *sal)
{
    if (hay & 1) {
        int64_t fd = openf(ent, O_LEER);
        if (fd < 0) { printf("  no puedo leer "); printf("%s", ent); printf("\n"); return -1; }
        dup2((int)fd, 0);
        closefd((int)fd);
    }
    if (hay & 2) {
        int64_t fd = openf(sal, O_ESCRIBIR);
        if (fd < 0) { printf("  no puedo escribir %s\n", sal); return -1; }
        dup2((int)fd, 1);
        closefd((int)fd);
    }
    return 0;
}

/* Trocear una orden en argumentos, quitando las comillas.
 *
 * Esto es lo que antes hacia el KERNEL, y estaba mal puesto: partir una
 * linea escrita por una persona es trabajo del interprete, no del sistema
 * operativo. Desde que exec recibe un array, cada uno hace lo suyo.
 *
 * Se escribe en un buffer APARTE y no sobre la propia linea. Compactar
 * sobre uno mismo parece seguro -el que escribe nunca adelanta al que
 * lee- hasta que te das cuenta de que el cero que cierra una palabra cae
 * justo encima del espacio que ibas a leer. Costo un argv[1] vacio.
 *
 * Deja argv terminado en 0, como manda el convenio, y devuelve cuantos
 * hay. */
#define MAX_ARGV  16

/* Copiar una cadena con tope, que hace falta en varios sitios. */
static void ucopiar(char *dst, const char *src, uint64_t max)
{
    uint64_t i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Una orden ya preparada: sus argumentos, y a donde van su entrada y su
 * salida. Todo en la misma estructura porque todo sale del mismo troceo.
 *
 * Tiene su propio almacen de texto, y eso hace falta: una tuberia son dos
 * ordenes vivas a la vez, y con un buffer compartido la segunda pisaria a
 * la primera. */
struct orden {
    char *argv[MAX_ARGV + 1];
    char  texto[MAX_LINEA];
    char  ent[FS_PATH_MAX];        /* fichero para "<", vacio si no hay */
    char  sal[FS_PATH_MAX];        /* fichero para ">"                  */
    int   hay;                     /* bit 0 = hay "<", bit 1 = hay ">"  */
};

/* Trocear, con dos reglas: las comillas agrupan, y "<" y ">" son fichas
 * SUELTAS aunque vayan pegadas a una palabra.
 *
 * Esa segunda regla es la que permite que la redireccion se resuelva
 * mirando el array en vez de cortando la cadena. Antes habia tres trozos
 * de codigo distintos que tenian que saber lo que es una comilla -este,
 * el que buscaba el < o el >, y el que copiaba el nombre de detras- y los
 * tres podian discrepar. Ahora la regla esta una vez.
 *
 * Se escribe en un buffer APARTE y no sobre la propia linea. Compactar
 * sobre uno mismo parece seguro -el que escribe nunca adelanta al que
 * lee- hasta que te das cuenta de que el cero que cierra una palabra cae
 * justo encima del espacio que ibas a leer. Costo un argv[1] vacio. */
static int trocear(const char *s, char **argv, char *texto, uint64_t max)
{
    uint64_t escribe = 0;
    int n = 0;

    while (*s && n < MAX_ARGV) {
        while (*s == ' ') s++;
        if (!*s) break;

        argv[n++] = texto + escribe;

        if (*s == '<' || *s == '>') {          /* ficha de un solo signo */
            if (escribe + 2 < max) { texto[escribe++] = *s; texto[escribe++] = 0; }
            s++;
            continue;
        }

        char comilla = 0;
        while (*s) {
            if (comilla) {
                if (*s == comilla) { comilla = 0; s++; continue; }
            } else {
                if (*s == ' ' || *s == '<' || *s == '>') break;
                if (*s == '"' || *s == '\'') { comilla = *s++; continue; }
            }
            if (escribe < max - 1) texto[escribe++] = *s;
            s++;
        }
        if (escribe < max) texto[escribe++] = 0;
    }

    argv[n] = 0;
    return n;
}

/* Sacar del array las fichas de redireccion y lo que venga detras.
 *
 * El programa no llega a ver ni el ">" ni el nombre, y eso es lo
 * correcto: la redireccion es un acuerdo entre el shell y el kernel sobre
 * que hay detras del 0 y del 1, no asunto del programa. */
static int preparar(struct orden *o, const char *linea)
{
    int n = trocear(linea, o->argv, o->texto, sizeof(o->texto));

    o->hay = 0;
    o->ent[0] = o->sal[0] = 0;

    int w = 0;
    for (int i = 0; i < n; i++) {
        char *t = o->argv[i];

        if ((t[0] == '<' || t[0] == '>') && t[1] == 0) {
            char *destino = (t[0] == '<') ? o->ent : o->sal;

            if (i + 1 >= n) {
                printf("  falta el fichero despues de %s\n", t);
                return 0;
            }
            ucopiar(destino, o->argv[++i], FS_PATH_MAX);
            o->hay |= (t[0] == '<') ? 1 : 2;
            continue;
        }
        o->argv[w++] = t;
    }
    o->argv[w] = 0;
    return w;
}


static void quejarse(const char *que)
{
    printf("  ");
    printf("%s", que);
    printf(": ");
    printf("%s", motivo);
    printf("\n");
}

/* --- Ejecutar -------------------------------------------------------- */

/* Las que cambian algo del PROPIO shell. Devuelve 1 si se ha ocupado. */
static int interna(char *orden, const char *der)
{
    if (der) return 0;                       /* "cd x | y" no tiene sentido */

    if (orden[0] == 'c' && orden[1] == 'd' &&
        (orden[2] == 0 || orden[2] == ' ')) {

        const char *a = orden + 2;
        while (*a == ' ') a++;
        if (!*a) a = "/";                    /* "cd" a secas: al raiz */

        if (chdir(a) < 0) printf("  no puedo entrar en %s\n", a);
        return 1;
    }

    if (orden[0] == 'p' && orden[1] == 'w' && orden[2] == 'd' && !orden[3]) {
        char aqui[FS_PATH_MAX];
        getcwd(aqui, sizeof(aqui));
        printf("  %s\n", aqui);
        return 1;
    }

    return 0;
}

static void una(char *orden)
{
    /* Un solo troceo, del que sale todo: los argumentos, el nombre del
     * programa y a donde van la entrada y la salida. */
    static struct orden o;
    if (preparar(&o, orden) < 1) return;

    nombre_de(o.argv[0]);

    unsigned char *img;
    char ruta[FS_PATH_MAX];
    if (!buscar_programa(nombre, ruta)) { no_esta(nombre); return; }

    uint64_t bytes = cargar(ruta, &img);
    if (!bytes) { quejarse(ruta); return; }

    int64_t pid = fork();
    if (pid == 0) {
        if (aplicar(o.hay, o.ent, o.sal) < 0) exit(1);
        exec(img, bytes, o.argv);
        printf("  no he podido convertirme en el programa\n");
        exit(1);
    }

    munmap((const char *)img);
    if (pid < 0) { printf("  no he podido bifurcarme\n"); return; }

    /* El codigo de salida del hijo. Solo se dice si no es cero, que es
     * como se comporta cualquier shell: lo normal no se anuncia. */
    int64_t codigo = waitpid((uint64_t)pid);
    if (codigo != 0)
        printf("  [salida %ld]\n", (long)codigo);
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
    /* Dos ordenes preparadas a la vez, cada una con su propio almacen de
     * texto. De ahi que 'struct orden' lo lleve dentro y no comparta
     * buffer con nadie. */
    static struct orden o1, o2;
    if (preparar(&o1, izq) < 1) return;
    if (preparar(&o2, der) < 1) return;

    char nombre_izq[FS_PATH_MAX];
    nombre_de(o1.argv[0]);
    ucopiar(nombre_izq, nombre, sizeof(nombre_izq));

    unsigned char *img1;
    char ruta1[FS_PATH_MAX];
    if (!buscar_programa(nombre_izq, ruta1)) { no_esta(nombre_izq); return; }

    uint64_t b1 = cargar(ruta1, &img1);
    if (!b1) { quejarse(ruta1); return; }

    nombre_de(o2.argv[0]);

    unsigned char *img2;
    char ruta2[FS_PATH_MAX];
    if (!buscar_programa(nombre, ruta2)) { no_esta(nombre); munmap((const char *)img1); return; }

    uint64_t b2 = cargar(ruta2, &img2);
    if (!b2) { quejarse(ruta2); munmap((const char *)img1); return; }

    int fds[2];
    if (pipe(fds) < 0) {
        printf("  no hay tuberias libres\n");
        munmap((const char *)img1); munmap((const char *)img2);
        return;
    }

    int64_t p1 = fork();
    if (p1 == 0) {
        dup2(fds[1], 1);                 /* mi salida es la tuberia */
        closefd(fds[0]);
        closefd(fds[1]);
        /* La redireccion va DESPUES de la tuberia, y por eso gana: en
         * "a > f | b", la salida de a acaba en el fichero y b no ve nada.
         * Es lo que hace cualquier shell, y sale solo de respetar el
         * orden en que se escribieron las dos cosas. */
        if (aplicar(o1.hay, o1.ent, o1.sal) < 0) exit(1);
        exec(img1, b1, o1.argv);
        exit(1);
    }

    int64_t p2 = fork();
    if (p2 == 0) {
        dup2(fds[0], 0);                 /* mi entrada es la tuberia */
        closefd(fds[0]);
        closefd(fds[1]);
        if (aplicar(o2.hay, o2.ent, o2.sal) < 0) exit(1);
        exec(img2, b2, o2.argv);
        exit(1);
    }

    /* El padre no usa la tuberia, y si no la suelta el lector nunca vera
     * el final. */
    closefd(fds[0]);
    closefd(fds[1]);
    munmap((const char *)img1);
    munmap((const char *)img2);

    if (p1 > 0) waitpid((uint64_t)p1);
    if (p2 > 0) waitpid((uint64_t)p2);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    mi_puerto = port_create(-1);
    if (mi_puerto < 0) { printf("  [sh] sin puertos\n"); exit(1); }

    printf("\n  TinyOS. Las ordenes son programas de la tarjeta,\n");
    printf("  se arrancan con fork + exec, se encadenan con | y se\n");
    printf("  redirigen con < y >\n");
    printf("  Hay directorios: cd, pwd y mkdir. Las ordenes viven en /usr/bin\n");
    printf("  Prueba: ls / mkdir docs / cd docs / cat /hola.txt > copia.txt\n");
    printf("          cd .. / ls docs / wc < hola.txt / salir\n");

    for (;;) {
        /* El prompt lleva el directorio: sin eso, con subdirectorios, se
         * pierde uno a la segunda orden. */
        char aqui[FS_PATH_MAX];
        if (getcwd(aqui, sizeof(aqui)) < 0) aqui[0] = 0;
        printf("\n%s $ ", aqui);

        int64_t largo = leer_linea();
        if (largo < 0) {                         /* fin de la entrada */
            printf("\n  se acabo la entrada, me voy\n");
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
            printf("  hasta luego\n");
            exit(0);
        }

        /* --- Ordenes internas ---
         *
         * "cd" TIENE que ser interna, y no por comodidad. Si fuera un
         * programa, el shell se bifurcaria, el hijo cambiaria SU
         * directorio actual -que se hereda, pero hacia abajo- y al morir
         * se lo llevaria con el. El shell seguiria donde estaba.
         *
         * Es la unica orden de este interprete que no puede ser un
         * fichero en la tarjeta, y el motivo es exactamente el mismo por
         * el que en cualquier Unix "cd" tampoco lo es. */
        if (interna(izq, der)) continue;

        if (der) tuberia(izq, limpiar(der));
        else     una(izq);
    }
}
