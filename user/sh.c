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
#include <errno.h>
#include "syscall.h"
#include "fs_abi.h"

#define MAX_LINEA   128

static char           linea[MAX_LINEA];
static char           nombre[FS_PATH_MAX];



/* --- Leer una linea ---------------------------------------------------
 *
 * Aqui habia cuarenta lineas: un bucle de getchar(), el eco de cada tecla,
 * y el borrado con "\b \b" cuando llegaba un 8 o un 127. Ya no hacen
 * falta, y no porque se hayan movido de sitio: es que nunca fueron del
 * shell.
 *
 * Un fgets. La linea llega entera y ya corregida, porque la disciplina de
 * linea del kernel no suelta nada hasta el Enter y los backspace se
 * comieron su caracter por el camino. El shell no llega a ver que hubo
 * correcciones, igual que no ve que el usuario se lo penso dos veces.
 *
 * Lo que se gana no es codigo: es que ahora CUALQUIER programa que lea del
 * terminal tiene edicion de linea sin pedirla, y que apagar el eco -para
 * una contrasenya- se hace en un sitio y vale para todos. Mientras el eco
 * lo hiciera el shell, solo el shell sabia apagarlo.
 *
 * Devuelve los caracteres leidos, LINEA_FIN si se acabo la entrada -un
 * Ctrl-D sobre una linea vacia, que hasta este paso no habia forma de
 * teclear- o LINEA_CORTE si una senyal corto la lectura. */
#define LINEA_FIN    (-1)
#define LINEA_CORTE  (-2)

static int64_t leer_linea(void)
{
    errno = 0;

    if (!fgets(linea, MAX_LINEA, stdin)) {
        if (errno == EINTR) { clearerr(stdin); return LINEA_CORTE; }
        return LINEA_FIN;
    }

    /* fgets se trae el salto; la orden no lo lleva. */
    uint64_t n = strlen(linea);
    if (n && linea[n - 1] == '\n') linea[--n] = 0;

    return (int64_t)n;
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

/* Cuanto mide un fichero, o 0 si no esta.
 *
 * Aqui habia veinticinco lineas de componer un fs_request y mandarlo por
 * un puerto. Con stat() el shell deja de ser un cliente del servidor de
 * ficheros y pasa a ser un programa normal: el unico que sabe como se
 * habla con ese servidor es el kernel, y el unico que sabe lo que dicen
 * los mensajes es el propio servidor. */
static uint64_t tamano_de(const char *fichero)
{
    struct estado e;
    if (stat(fichero, &e) < 0) { motivo = "no esta en la tarjeta"; return 0; }
    if (e.flags & FS_ES_DIR)   { motivo = "es un directorio";      return 0; }
    return e.tam;
}

/* --- El PATH ----------------------------------------------------------
 *
 * Donde buscar un programa cuando lo que has escrito no lleva barras.
 *
 * Sale del ENTORNO, no del codigo: "PATH=.:/usr/bin" es una cadena que
 * viene heredada y que se puede cambiar con export. Hasta el paso 40
 * estaba escrita aqui dentro, y cambiarla queria decir recompilar el
 * sistema operativo.
 *
 * EL DIRECTORIO ACTUAL VA EL ULTIMO, y eso se aprendio a golpes.
 *
 * Estaba primero, porque es comodo. En Unix "." ni siquiera esta en el
 * PATH por defecto, y el argumento clasico es la seguridad: entrar en un
 * directorio ajeno y escribir "ls" podria ejecutar el "ls" que haya
 * dejado su duenyo.
 *
 * Aqui no hay varios usuarios, asi que ese argumento no aplicaba... y
 * mordio igual, por otro sitio. En la particion de arranque habian
 * quedado los ejecutables de hace unos pasos, de cuando vivian ahi. Un
 * "cd /boot" seguido de "ls" no ejecutaba /usr/bin/LS.ELF: ejecutaba el
 * viejo, que leia mal las respuestas del servidor y ensenyaba basura.
 *
 * No hacia falta un atacante. Bastaba con una copia vieja. */
static const char *el_path(void)
{
    const char *p = getenv("PATH");
    return (p && *p) ? p : "/usr/bin:.";
}

/* El trozo numero 'n' del PATH, separando por ':'. Devuelve 0 al final. */
static int path_trozo(int n, char *dst, uint64_t max)
{
    const char *p = el_path();

    for (int i = 0; i < n; i++) {
        while (*p && *p != ':') p++;
        if (!*p) return 0;
        p++;
    }
    if (!*p) return 0;

    uint64_t o = 0;
    while (*p && *p != ':' && o < max - 1) dst[o++] = *p++;
    dst[o] = 0;
    return 1;
}

/* Deja en 'ruta' la absoluta que si existe, o devuelve 0.
 *
 * REGLA DE UNIX: si el nombre lleva una barra, no se busca en ningun
 * sitio. "./prog" y "/usr/bin/prog" dicen exactamente donde estan, y
 * ponerse a buscar seria desobedecer. Solo los nombres pelados -"ls"-
 * pasan por el PATH. */
static int buscar_programa(const char *nom, char *ruta)
{
    for (const char *p = nom; *p; p++)
        if (*p == '/')
            return realpath(nom, ruta) == 0 && tamano_de(ruta);

    char dir[FS_PATH_MAX];

    for (int i = 0; path_trozo(i, dir, sizeof(dir)); i++) {
        if (!dir[0]) continue;

        /* Un trozo relativo -"." o "bin"- se resuelve contra donde
         * estamos, igual que cualquier otra ruta. */
        char completa[FS_PATH_MAX];
        uint64_t d = strlen(dir), n = strlen(nom);
        if (d + 1 + n + 1 > FS_PATH_MAX) continue;

        memcpy(completa, dir, d);
        completa[d] = '/';
        memcpy(completa + d + 1, nom, n + 1);

        if (realpath(completa, ruta) == 0 && tamano_de(ruta)) return 1;
    }
    return 0;
}

/* Decir DONDE se ha buscado. Un "no encuentro ls" a secas manda a pensar
 * que el fichero no esta; ensenyar la lista dice que quiza esta, pero en
 * otro sitio. */
static void no_esta(const char *nom)
{
    printf("  %s: no lo encuentro. PATH=%s\n", nom, el_path());
}

/* Decir DONDE se ha buscado. Un "no encuentro ls" a secas manda a pensar
 * que el fichero no esta; enseñar la lista dice que quiza esta, pero en
 * otro sitio. */

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
        /* ">" vacia y ">>" anyade, y la diferencia entera esta en el modo
         * con el que se abre: el shell no hace nada distinto despues. Lo
         * que no hace -y es lo que importa- es abrir con ">" y luego
         * saltar al final, que es lo que uno escribiria si no supiera que
         * existe O_ANYADIR y produciria un fichero correcto justo hasta
         * que dos programas anyadan a la vez. */
        int64_t fd = openf(sal, (hay & 4) ? O_ANYADIR : O_ESCRIBIR);
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
    int   hay;                     /* bit 0 = "<", bit 1 = ">", bit 2 = ">>" */
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

        /* Fichas de signos. ">>" es UNA, y hay que mirarlo antes que ">"
         * o saldrian dos seguidas y la segunda se comeria el nombre. Es la
         * regla del troceador mas larga primero, que en un lenguaje de
         * verdad se llama "maximal munch" y aqui son cuatro lineas. */
        if (*s == '<' || *s == '>') {
            int doble = (s[0] == '>' && s[1] == '>');
            if (escribe + 3 < max) {
                texto[escribe++] = *s;
                if (doble) texto[escribe++] = *s;
                texto[escribe++] = 0;
            }
            s += doble ? 2 : 1;
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

            /* $NOMBRE se sustituye por lo que valga, salvo entre comillas
             * simples. Esa distincion entre ' y " es de las cosas de los
             * shells que parecen caprichosas y no lo son: una manera de
             * decir "esto tal cual" y otra de decir "esto, pero
             * mirandolo". */
            if (*s == '$' && comilla != '\'') {
                s++;
                char nombre[64];
                uint64_t n = 0;
                while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
                       (*s >= '0' && *s <= '9') || *s == '_') {
                    if (n < sizeof(nombre) - 1) nombre[n++] = *s;
                    s++;
                }
                nombre[n] = 0;

                const char *v = n ? getenv(nombre) : 0;
                if (v) while (*v && escribe < max - 1) texto[escribe++] = *v++;
                continue;
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

        int entrada = (t[0] == '<' && t[1] == 0);
        int salida  = (t[0] == '>' && t[1] == 0);
        int anyade  = (t[0] == '>' && t[1] == '>' && t[2] == 0);

        if (entrada || salida || anyade) {
            char *destino = entrada ? o->ent : o->sal;

            if (i + 1 >= n) {
                printf("  falta el fichero despues de %s\n", t);
                return 0;
            }
            ucopiar(destino, o->argv[++i], FS_PATH_MAX);
            o->hay |= entrada ? 1 : (anyade ? 2 | 4 : 2);
            continue;
        }
        o->argv[w++] = t;
    }
    o->argv[w] = 0;
    return w;
}


/* --- Los trabajos -----------------------------------------------------
 *
 * Un trabajo es lo que el usuario escribio en una linea, que puede ser
 * mas de un proceso: "cat x | wc" son dos. Se le pone un numero pequenyo
 * -[1], [2]- porque el pid no le dice nada a nadie, y se guarda el grupo,
 * que es lo unico que hace falta para hablarle entero.
 */
#define MAX_TRABAJOS  8

/* Lo que hay que contar de un trabajo la proxima vez que se saque un
 * prompt. Ver 'recoger' y 'anunciar': quien se entera y quien lo dice ya no
 * son el mismo. */
#define AVISO_NADA     0
#define AVISO_PARADO   1
#define AVISO_HECHO    2

static struct trabajo {
    int      usado;
    int      numero;                 /* el [1] que se ensenya */
    int      parado;                 /* detenido, que no es terminado */
    int      aviso;                  /* noticia pendiente de contar */
    uint64_t pgid;
    uint64_t pids[2];
    int      npids;
    char     orden[80];
} trabajos[MAX_TRABAJOS];

static int siguiente_numero = 1;
static uint64_t mi_grupo;            /* el del propio shell */

static struct trabajo *anotar(uint64_t pgid, uint64_t p1, uint64_t p2,
                              const char *orden, int parado)
{
    for (int i = 0; i < MAX_TRABAJOS; i++) {
        if (trabajos[i].usado) continue;

        struct trabajo *t = &trabajos[i];
        t->usado  = 1;
        t->parado = parado;
        t->aviso  = AVISO_NADA;      /* las ranuras se reciclan */
        t->numero = siguiente_numero++;
        t->pgid   = pgid;
        t->npids  = 0;
        t->pids[t->npids++] = p1;
        if (p2) t->pids[t->npids++] = p2;
        ucopiar(t->orden, orden, sizeof(t->orden));

        printf("  [%d] %s  %s\n", t->numero,
               parado ? "parado " : "en el fondo", t->orden);
        return t;
    }
    printf("  no caben mas trabajos\n");
    return 0;
}

static struct trabajo *buscar_trabajo(const char *arg)
{
    /* Sin numero, el ULTIMO que se anoto. Es lo que hace cualquier shell y
     * es lo que uno quiere decir cuando escribe "fg" a secas: el de antes. */
    struct trabajo *mejor = 0;

    if (arg && *arg) {
        int n = atoi(arg);
        for (int i = 0; i < MAX_TRABAJOS; i++)
            if (trabajos[i].usado && trabajos[i].numero == n) return &trabajos[i];
        return 0;
    }

    for (int i = 0; i < MAX_TRABAJOS; i++)
        if (trabajos[i].usado && (!mejor || trabajos[i].numero > mejor->numero))
            mejor = &trabajos[i];
    return mejor;
}

/* Recoger los que hayan terminado, SIN esperar a ninguno.
 *
 * Lo llaman DOS: el manejador de SIGCHLD, en cuanto un hijo cambia de
 * estado, y el bucle principal antes de cada prompt. El segundo dejo de ser
 * el que se entera y paso a ser una red: recoge lo que el manejador no
 * pudo -mientras 'no_molestes' estaba puesto- y lo que pasara si algun dia
 * se pierde una senyal.
 *
 * Y NO IMPRIME NADA, que es la unica razon por la que se puede llamar desde
 * un manejador. printf no es reentrante: escribe en el cubo de stdout, y si
 * la senyal llega justo cuando el bucle principal estaba a medias de otro
 * printf, las dos escrituras se pisan. Por eso lo que aqui se decide se
 * APUNTA, y lo cuenta 'anunciar' desde el bucle principal.
 *
 * Es exactamente lo que hace bash: se entera en el acto y lo dice en el
 * prompt siguiente. Parecia una cortesia para no ensuciar la linea que
 * estas escribiendo, y resulta que es lo que hace seguro al manejador.
 */
/* --- Y los hijos que la tabla no conoce -------------------------------
 *
 * Que existan es un descuido con nombre: 'anotar' devuelve 0 cuando ya no
 * caben mas trabajos, y ninguno de los cuatro sitios que la llaman mira ese
 * valor. Asi que el noveno trabajo de fondo se bifurca, corre, termina... y
 * se queda de zombi para siempre, porque el unico que podia enterrarlo era
 * el que no se acordo de apuntarlo. Un slot de la tabla de tareas y su
 * memoria, perdidos hasta que muera el shell, que no muere nunca.
 *
 * Esto es lo que PID_CUALQUIERA sabe hacer y un pid concreto no: recoger
 * sin saber a quien. No hace falta preguntar quien era, porque de estos no
 * hay nada que contar -nadie los apunto- y lo unico que se les debe es el
 * entierro.
 *
 * Va DESPUES del recorrido de la tabla, y el orden importa: "cualquiera"
 * incluye a los que si conocemos, y si se barriera primero, los trabajos
 * apuntados perderian su codigo de salida en manos de quien no sabe de
 * quien era. */
static void barrer_ajenos(void)
{
    /* Con tope, que es la regla de cualquier bucle cuya condicion de salida
     * la decide otro. Y SIN WUNTRACED, que es la parte sutil: un hijo
     * parado contestaria en cada vuelta -nadie lleva la cuenta de si ya se
     * aviso de esa parada- y el bucle no acabaria nunca. Aqui solo se
     * entierra; parar no es morirse. */
    for (int v = 0; v < MAX_TRABAJOS * 2 + 4; v++) {
        int64_t r = waitpid_ya(PID_CUALQUIERA);
        if (r == -EAGAIN || r == -ECHILD) return;
    }
}

static void recoger(void)
{
    for (int i = 0; i < MAX_TRABAJOS; i++) {
        struct trabajo *t = &trabajos[i];
        if (!t->usado) continue;

        /* Los parados TAMBIEN se miran.
         *
         * Saltarselos parecia razonable -no han terminado, estan
         * esperando permiso- y dejaba un agujero: a un proceso parado se
         * le puede mandar un SIGKILL, y de hecho es lo unico aparte de
         * SIGCONT que lo saca de ahi. Si el shell no vuelve a preguntar,
         * la lista sigue diciendo "parado" de algo que ya no existe.
         *
         * Lo que no se repite es el ANUNCIO, que es lo unico que molestaba
         * de preguntar: se dice al cambiar de estado, no en cada prompt. */
        int quedan = 0, se_paro = 0;

        for (int k = 0; k < t->npids; k++) {
            if (!t->pids[k]) continue;

            /* WUNTRACED tambien aqui, y no solo al esperar en primer
             * plano. Sin el, un trabajo del fondo que se detiene -porque
             * intento leer del teclado, que es lo normal- se cuenta como
             * "sigue vivo" y la lista dice que corre. Y es peor que un
             * error de contabilidad: el usuario ve "corriendo", se queda
             * esperando a que avance, y no va a avanzar nunca.
             *
             * Una lista de trabajos que no sabe distinguir parado de
             * corriendo no es una lista de trabajos. */
            int que = W_SALIDA;
            if (waitpid_que(t->pids[k], &que, WNOHANG | WUNTRACED) == -EAGAIN)
                quedan++;
            else if (que == W_PARADO) { quedan++; se_paro = 1; }
            else t->pids[k] = 0;            /* recogido */
        }

        if (se_paro) {
            if (!t->parado) {
                t->parado = 1;
                t->aviso  = AVISO_PARADO;
            }
        } else if (!quedan) {
            /* La ranura NO se libera aqui: hace falta viva para poder
             * contar que ha terminado. La suelta 'anunciar'. */
            t->aviso = AVISO_HECHO;
        } else {
            t->parado = 0;               /* alguien le dio un SIGCONT */
        }
    }

    barrer_ajenos();
}

/* Y contarlo, ya en el bucle principal, donde se puede imprimir.
 *
 * Va justo antes del prompt y no en otro sitio, asi que ninguna noticia
 * aparece en medio de la linea que estas escribiendo. */
static void anunciar(void)
{
    for (int i = 0; i < MAX_TRABAJOS; i++) {
        struct trabajo *t = &trabajos[i];
        if (!t->usado || t->aviso == AVISO_NADA) continue;

        if (t->aviso == AVISO_PARADO) {
            printf("  [%d] parado   %s\n", t->numero, t->orden);
            t->aviso = AVISO_NADA;
        } else {
            printf("  [%d] hecho    %s\n", t->numero, t->orden);
            t->usado = 0;                /* ahora si */
        }
    }
}

/* --- El manejador, y la unica ventana en que no le toca ----------------
 *
 * Mientras el bucle principal espera a un trabajo de primer plano, esta
 * esperando a un hijo CONCRETO y quiere el codigo de salida de ese. Si el
 * manejador se pusiera a recoger por su cuenta con PID_CUALQUIERA, podria
 * llevarse justo a ese, y el waitpid de arriba volveria con -ECHILD: el
 * shell se quedaria sin saber como acabo lo que acaba de ejecutar.
 *
 * En Unix esto se arregla bloqueando la senyal alrededor del trozo
 * delicado, con sigprocmask. Aqui no hay mascaras -son un paso que no se
 * ha dado- asi que lo unico que se puede hacer es que el manejador sepa
 * cuando no le toca. La senyal se pierde, y no pasa nada: el 'recoger' del
 * prompt siguiente encuentra lo que quedara.
 *
 * Que una senyal se pueda perder asi es, por si hacia falta el argumento,
 * la razon de que sigprocmask exista. */
static volatile int no_molestes;

static void sigchld(int sig)
{
    (void)sig;
    if (no_molestes) return;
    recoger();
}

static void listar_trabajos(void)
{
    int hay = 0;
    for (int i = 0; i < MAX_TRABAJOS; i++)
        if (trabajos[i].usado) {
            printf("  [%d] %-9s %lu  %s\n", trabajos[i].numero,
                   trabajos[i].parado ? "parado" : "corriendo",
                   (unsigned long)trabajos[i].pgid, trabajos[i].orden);
            hay = 1;
        }
    if (!hay) printf("  no hay trabajos\n");
}

/* Ceder la consola a un grupo y esperarlo, y recuperarla pase lo que
 * pase. Esto es lo que hace que Ctrl-C alcance al trabajo entero y no al
 * shell, y lo que devuelve el teclado al shell cuando el trabajo acaba.
 *
 * Es un prestamo, y como todo prestamo lo importante es la linea de
 * despues: si el shell se olvidara de recuperarla, el teclado se quedaria
 * apuntando a un grupo que ya no existe y Ctrl-C no volveria a servir. */
static int64_t en_primer_plano(uint64_t pgid, uint64_t p1, uint64_t p2,
                               int *parado)
{
    no_molestes = 1;                 /* de estos me encargo yo */
    consola(pgid);

    int64_t codigo = 0;
    int     que    = W_SALIDA;
    *parado = 0;

    uint64_t quienes[2] = { p1, p2 };
    for (int i = 0; i < 2 && quienes[i]; i++) {
        codigo = waitpid_que(quienes[i], &que, WUNTRACED);

        /* Si uno se paro, se pararon todos: Ctrl-Z va al grupo entero.
         * Seguir esperando al otro seria esperar a algo que no va a
         * moverse, que es la forma mas facil de colgar un shell. */
        if (que == W_PARADO) { *parado = 1; break; }
    }

    /* La consola vuelve pase lo que pase. Terminado o parado, el que
     * manda a partir de ahora es el shell otra vez, y si se le olvidara
     * recuperarla el teclado apuntaria a un grupo que no la va a usar. */
    consola(mi_grupo);
    no_molestes = 0;
    return codigo;
}

/* Traer un trabajo al frente: cederle la consola, decirle que siga, y
 * esperarlo.
 *
 * El ORDEN de esas dos primeras cosas importa, y equivocarse sale caro:
 * si se manda el SIGCONT antes de cederle la consola, el proceso arranca,
 * intenta leer del teclado, ve que no es el de primer plano, se gana un
 * SIGTTIN y se vuelve a parar en el acto. "fg" parpadearia y no haria
 * nada. Primero el testigo, luego el pistoletazo. */
static void al_frente(struct trabajo *t)
{
    printf("  %s\n", t->orden);

    no_molestes = 1;
    consola(t->pgid);
    kill(-(int64_t)t->pgid, SIGCONT);

    int     parado = 0;
    int     que    = W_SALIDA;
    int64_t codigo = 0;

    for (int i = 0; i < t->npids; i++) {
        if (!t->pids[i]) continue;
        codigo = waitpid_que(t->pids[i], &que, WUNTRACED);
        if (que == W_PARADO) { parado = 1; break; }
    }

    consola(mi_grupo);
    no_molestes = 0;

    if (parado) { t->parado = 1; printf("\n  [%d] parado   %s\n", t->numero, t->orden); }
    else {
        t->usado = 0;
        if (codigo != 0) printf("  [salida %ld]\n", (long)codigo);
    }
}

/* Y soltarlo en el fondo: el mismo SIGCONT, pero sin darle la consola.
 * Toda la diferencia entre fg y bg es esa linea que aqui no esta. */
static void al_fondo(struct trabajo *t)
{
    t->parado = 0;
    kill(-(int64_t)t->pgid, SIGCONT);
    printf("  [%d] %s &\n", t->numero, t->orden);
}

/* No hace nada, y eso es lo que tiene que hacer: lo unico que se busca es
 * que no ocurra la accion por defecto. La linea a medias se pierde y sale un
 * prompt nuevo.
 *
 * Lo comparten Ctrl-C y Ctrl-Z, que piden lo mismo del shell por motivos
 * distintos: la primera para que no lo mate, la segunda para que no lo pare.
 * Ver donde se instalan. */
static void tragar(int sig) { (void)sig; }

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

    /* export, y por el mismo motivo que cd: setenv cambia el entorno del
     * proceso que llama, y nada mas. Si export fuera un programa, el
     * shell se bifurcaria, el hijo cambiaria SU entorno y al morir se lo
     * llevaria consigo. El entorno se hereda hacia abajo, nunca hacia
     * arriba. */
    if (orden[0] == 'e' && orden[1] == 'x' && orden[2] == 'p' &&
        orden[3] == 'o' && orden[4] == 'r' && orden[5] == 't' &&
        (orden[6] == 0 || orden[6] == ' ')) {

        const char *a = orden + 6;
        while (*a == ' ') a++;

        char nombre[64];
        uint64_t n = 0;
        while (*a && *a != '=' && n < sizeof(nombre) - 1) nombre[n++] = *a++;
        nombre[n] = 0;

        if (!n || *a != '=') {
            printf("  uso: export NOMBRE=valor\n");
            return 1;
        }

        if (setenv(nombre, a + 1) < 0)
            printf("  no cabe una variable mas\n");
        return 1;
    }

    if (orden[0] == 'j' && orden[1] == 'o' && orden[2] == 'b' &&
        orden[3] == 's' && !orden[4]) {
        listar_trabajos();
        return 1;
    }

    /* fg y bg TIENEN que ser internas, y por el mismo motivo que cd: la
     * tabla de trabajos es del shell, y un programa aparte no la ve. */
    if ((orden[0] == 'f' || orden[0] == 'b') && orden[1] == 'g' &&
        (orden[2] == 0 || orden[2] == ' ')) {
        struct trabajo *t = buscar_trabajo(limpiar(orden + 2));
        if (!t) { printf("  no hay ese trabajo\n"); return 1; }

        if (orden[0] == 'f') al_frente(t);
        else if (!t->parado) printf("  [%d] ya estaba corriendo\n", t->numero);
        else al_fondo(t);
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

static void una(char *orden, int fondo, const char *entera)
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
        /* El hijo se pone en SU grupo antes de hacer nada mas.
         *
         * Y el padre hace lo mismo justo despues del fork, con la misma
         * llamada y el mismo efecto. No es una de las dos veces escrita
         * dos veces: son dos carreras distintas, y cada linea tapa una.
         *
         * Si solo lo hiciera el hijo, el padre podria llegar a ceder la
         * consola -consola(pid)- antes de que el hijo se hubiera
         * colocado, y el Ctrl-C iria a un grupo vacio.
         *
         * Si solo lo hiciera el padre, el hijo podria llegar al exec y
         * hasta terminar antes de que el padre lo moviera, y setpgid
         * sobre alguien que ya no esta devuelve -ESRCH.
         *
         * Escrito en los dos sitios, gane quien gane la carrera el
         * resultado es el mismo. Es de las pocas veces en que repetir una
         * llamada es lo correcto y no un descuido. */
        setpgid(0, 0);
        if (aplicar(o.hay, o.ent, o.sal) < 0) exit(1);
        exec(img, bytes, o.argv, environ);
        printf("  no he podido convertirme en el programa\n");
        exit(1);
    }

    munmap((const char *)img);
    if (pid < 0) { printf("  no he podido bifurcarme\n"); return; }

    setpgid((uint64_t)pid, (uint64_t)pid);      /* la otra mitad de la carrera */

    if (fondo) { anotar((uint64_t)pid, (uint64_t)pid, 0, entera, 0); return; }

    /* El codigo de salida del hijo. Solo se dice si no es cero, que es
     * como se comporta cualquier shell: lo normal no se anuncia. */
    int parado = 0;
    int64_t codigo = en_primer_plano((uint64_t)pid, (uint64_t)pid, 0, &parado);

    /* Un Ctrl-Z convierte un trabajo de primer plano en uno de la lista.
     * No ha terminado, asi que no se puede olvidar: alguien tiene que
     * acordarse de el o se queda parado para siempre sin nadie que lo
     * sepa. */
    if (parado) { printf("\n"); anotar((uint64_t)pid, (uint64_t)pid, 0, entera, 1); return; }

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
static void tuberia(char *izq, char *der, int fondo, const char *entera)
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
        setpgid(0, 0);                   /* el primero FORMA el grupo */
        dup2(fds[1], 1);                 /* mi salida es la tuberia */
        closefd(fds[0]);
        closefd(fds[1]);
        /* La redireccion va DESPUES de la tuberia, y por eso gana: en
         * "a > f | b", la salida de a acaba en el fichero y b no ve nada.
         * Es lo que hace cualquier shell, y sale solo de respetar el
         * orden en que se escribieron las dos cosas. */
        if (aplicar(o1.hay, o1.ent, o1.sal) < 0) exit(1);
        exec(img1, b1, o1.argv, environ);
        exit(1);
    }

    setpgid((uint64_t)p1, (uint64_t)p1);

    int64_t p2 = fork();
    if (p2 == 0) {
        /* Y el segundo se mete en el del primero. Los dos procesos son UN
         * trabajo: lo que el usuario escribio es "cat x | wc", no dos
         * cosas, y cuando pulse Ctrl-C quiere parar eso. */
        setpgid(0, (uint64_t)p1);
        dup2(fds[0], 0);                 /* mi entrada es la tuberia */
        closefd(fds[0]);
        closefd(fds[1]);
        if (aplicar(o2.hay, o2.ent, o2.sal) < 0) exit(1);
        exec(img2, b2, o2.argv, environ);
        exit(1);
    }

    /* El padre no usa la tuberia, y si no la suelta el lector nunca vera
     * el final. */
    closefd(fds[0]);
    closefd(fds[1]);
    munmap((const char *)img1);
    munmap((const char *)img2);

    if (p1 <= 0 || p2 <= 0) return;

    setpgid((uint64_t)p2, (uint64_t)p1);

    if (fondo) { anotar((uint64_t)p1, (uint64_t)p1, (uint64_t)p2, entera, 0); return; }

    int parado = 0;
    en_primer_plano((uint64_t)p1, (uint64_t)p1, (uint64_t)p2, &parado);
    if (parado) { printf("\n"); anotar((uint64_t)p1, (uint64_t)p1, (uint64_t)p2, entera, 1); }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n  TinyOS. Las ordenes son programas de la tarjeta,\n");
    printf("  se arrancan con fork + exec, se encadenan con | y se\n");
    printf("  redirigen con <, > y >>\n");
    printf("  Con & van al fondo; Ctrl-Z los para, y jobs/fg/bg los manejan\n");
    printf("  Hay directorios (cd, pwd, mkdir) y entorno (export, env, $VAR)\n");
    printf("  Prueba: ls / mkdir docs / cd docs / cat /hola.txt > copia.txt\n");
    printf("          cd .. / ls docs / wc < hola.txt / salir\n");

    /* El shell tiene que saber cual es su propio grupo para recuperar la
     * consola cuando un trabajo de primer plano termina. */
    mi_grupo = (uint64_t)getpgid(0);

    /* Y tiene que sobrevivir a su propio Ctrl-C.
     *
     * Ahora que la senyal va al GRUPO de primer plano y el shell es quien
     * esta delante mientras escribes, un Ctrl-C en el prompt le llegaria a
     * el, y la accion por defecto lo mataria. Antes no pasaba porque la
     * senyal se la mandaban al hijo; el mecanismo nuevo es mas correcto y
     * por eso destapa esto.
     *
     * Atraparla y no hacer nada es exactamente lo que hace cualquier
     * shell: la linea a medias se pierde y sale un prompt limpio. */
    signal(SIGINT, tragar);

    /* Y tiene que sobrevivir tambien a su propio Ctrl-Z, que es la otra
     * mitad de la misma idea y faltaba desde el paso 52.
     *
     * Un Ctrl-Z en el prompt le llegaba al shell, que no lo atrapaba, y la
     * accion por defecto lo DETENIA. Ahi se acababa la sesion: init espera a
     * que su interprete TERMINE, no a que se pare, asi que nadie lo reanimaba
     * y la maquina se quedaba muda para siempre. Se arreglaba apagando.
     *
     * Quien reparte el teclado no puede dejarse quitar por el. Es la misma
     * regla que ya valia para SIGINT, y por eso cualquier shell de verdad
     * ignora las senyales del terminal: no es que no le importen, es que el
     * es el arbitro.
     *
     * Y se ignora con un manejador vacio en vez de con un SIG_IGN de verdad
     * -que aqui no existe- porque en este caso concreto es lo CORRECTO y no
     * un atajo. Un manejador vacio interrumpe la lectura y SIG_IGN no, y
     * aqui hace falta que la interrumpa: el terminal ya tiro la linea que
     * estabas escribiendo -eso lo hace el driver, antes de saber quien va a
     * atender la senyal- asi que el shell TIENE que volver a dibujar el
     * prompt. Con un SIG_IGN quedaria en pantalla una orden a medias que ya
     * no existe en ningun sitio.
     *
     * SIGTTIN y SIGTTOU no se tocan, y esa es la diferencia con bash. Alli
     * hacen falta porque a bash se le puede arrancar desde el fondo de otro
     * shell; aqui el interprete esta siempre en primer plano -init le da la
     * consola al nacer, y el la recupera despues de cada trabajo- asi que
     * nunca va a leer el teclado sin tener derecho. Y atraparlas seria peor
     * que no hacerlo: con un manejador vacio, un shell del fondo que leyera
     * se quedaria girando -lectura, EINTR, prompt, lectura- en vez de
     * pararse, que es justo la proteccion que SIGTTIN existe para dar. */
    signal(SIGTSTP, tragar);

    /* Y ahora el aviso de que un hijo ha cambiado de estado.
     *
     * Con SIG_REANUDAR, y eso es la mitad del paso: sin la bandera, cada
     * hijo que terminara mientras el usuario escribe le romperia la lectura
     * de la linea al shell. Con ella, el manejador se cuela, recoge, y la
     * lectura sigue donde estaba: el usuario no se entera de que ha pasado
     * nada, que es lo correcto, porque no ha pasado nada que le incumba a
     * la linea que esta escribiendo. */
    signal_banderas(SIGCHLD, sigchld, SIG_REANUDAR);

    for (;;) {
        /* Recoger sigue estando aqui, pero ya no como forma de enterarse:
         * de eso se ocupa el manejador en cuanto pasa. Esto es la red que
         * pilla lo que ocurriera mientras 'no_molestes' estaba puesto. */
        recoger();

        /* Y contar lo que haya, que es lo que el manejador no podia hacer. */
        anunciar();

        /* El prompt lleva el directorio: sin eso, con subdirectorios, se
         * pierde uno a la segunda orden. */
        char aqui[FS_PATH_MAX];
        if (getcwd(aqui, sizeof(aqui)) < 0) aqui[0] = 0;
        printf("\n%s $ ", aqui);

        int64_t largo = leer_linea();

        /* Un Ctrl-C mientras escribias: la linea a medias se tira y sale
         * un prompt nuevo. Es lo que hace cualquier shell, y es lo unico
         * que puede hacer, porque no sabe si ibas a terminar la frase. */
        if (largo == LINEA_CORTE) { printf("\n"); continue; }

        if (largo < 0) {                         /* fin de la entrada */
            printf("\n  se acabo la entrada, me voy\n");
            exit(0);
        }
        if (largo == 0) continue;

        /* ¿Acaba en "&"? Se mira ANTES de partir por la tuberia, porque
         * "a | b &" manda al fondo el trabajo entero y no solo la mitad
         * de la derecha. El & no es de una orden, es de la linea. */
        int fondo = 0;
        {
            char *fin = linea + largo;
            while (fin > linea && (fin[-1] == ' ' || fin[-1] == '\n')) fin--;
            if (fin > linea && fin[-1] == '&') { fondo = 1; fin[-1] = 0; }
            else *fin = 0;
        }

        /* Una copia de la linea tal y como se escribio, para poder
         * ensenyarla luego en la lista de trabajos: lo que viene ahora la
         * parte en trozos con ceros por el medio. */
        char entera[80];
        ucopiar(entera, limpiar(linea), sizeof(entera));

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

        if (der) tuberia(izq, limpiar(der), fondo, entera);
        else     una(izq, fondo, entera);
    }
}
