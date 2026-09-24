/* user/vi.c - Un editor de pantalla, y por que es modal
 *
 * Este es el primer programa del proyecto que se queda con el terminal
 * ENTERO: apaga la disciplina de linea del paso 53, se pone en modo crudo y
 * a partir de ahi cada tecla es suya. Y en cuanto haces eso, aparece un
 * problema que ningun programa anterior tenia.
 *
 * Un editor necesita muchas mas ordenes que teclas. Mover el cursor en
 * cuatro direcciones, insertar, borrar, buscar, guardar, salir... y lo unico
 * que llega por la linea serie son bytes. No hay Ctrl ni Alt que valgan
 * -bueno, Ctrl si, pero son 31 combinaciones y ya hay quien las usa-, no hay
 * teclas de funcion en las que confiar, y desde luego no hay raton.
 *
 * Asi que las teclas tienen que significar cosas distintas en momentos
 * distintos. Eso es un MODO, y de ahi sale vi: no es una rareza historica ni
 * cabezoneria de los viejos, es lo unico que se puede hacer cuando tienes 26
 * letras y necesitas cincuenta ordenes.
 *
 * Y hay una segunda razon, que en esta maquina se puede medir. La linea va a
 * 115200 baudios, o sea unos 11.520 bytes por segundo. Una pantalla de 80x24
 * son 1.920 caracteres, que con los escapes de posicionamiento se van a unos
 * 2.000 bytes: 174 MILISEGUNDOS. Si el editor redibujara todo en cada tecla,
 * escribir seria como escribir debajo del agua.
 *
 * Bill Joy escribio el vi original en una linea de 300 baudios, o sea 30
 * bytes por segundo. Ahi la misma pantalla tarda SESENTA Y CUATRO SEGUNDOS.
 * Todo lo que le parece raro a la gente de vi -que no haya menus, que las
 * ordenes sean una letra, que no se refresque la pantalla a lo tonto- sale de
 * ese numero. Nosotros tenemos 384 veces mas ancho de banda y la leccion
 * sigue valiendo: este editor redibuja UNA LINEA cuando cambia una linea, y
 * la pantalla entera solo cuando de verdad cambia la pantalla entera.
 *
 * Con Ctrl-G te dice cuantos bytes ha mandado al terminal desde que arranco,
 * que es la unica forma honesta de saber si eso que acabo de decir es verdad.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "syscall.h"

/* --- Limites, y por que son fijos ------------------------------------
 *
 * La tabla de lineas es un array fijo, como la de tareas o la de trabajos.
 * 8192 lineas dan para cualquier fuente de este proyecto -el mas largo es
 * sched.c con 2.591- y no para el README, que tiene 5.005... bueno, si da.
 * Lo que NO da es para un fichero de verdad grande, y ahi la respuesta seria
 * no tener las lineas en un array: seria un "gap buffer", que es un solo
 * bloque de texto con un hueco movil donde esta el cursor. Eso convierte
 * insertar un caracter en mover el hueco en vez de en mover media linea, y es
 * lo que usan los editores que aguantan ficheros de megabytes. */
#define MAX_LINEAS   8192
#define MAX_COL      1024    /* la linea mas larga que se puede editar */
#define SALIDA_CAP   8192    /* el cubo de salida hacia el terminal    */

/* Una linea es su texto y su longitud. El texto se pide al monton y crece
 * cuando hace falta; no se guarda la capacidad porque se redondea siempre a
 * multiplos de 64 y con eso se sabe. */
struct linea {
    char *t;
    int   n;
};

static struct linea buf[MAX_LINEAS];
static int nlineas;

static char ruta[FS_PATH_MAX];
static int  tocado;                  /* hay cambios sin guardar */

/* Y si el [+] que lo anuncia esta ya en la pantalla. Vive en la parte
 * izquierda del estado, que no se toca en los redibujados baratos; sin llevar
 * la cuenta, el aviso de "tienes cosas sin guardar" no aparecia hasta el
 * primer redibujado completo, que es tarde. */
static int  tocado_pintado;

/* --- Donde estamos ---------------------------------------------------
 * 'fila' y 'col' son la posicion en el FICHERO. 'arriba' e 'izq' son que
 * trozo del fichero se esta viendo. Separarlas es lo que permite que mover
 * el cursor por la pantalla no cueste ni un byte de redibujado. */
static int fila, col;
static int arriba, izq;

static int filas = 24, columnas = 80;
#define TEXTO  (filas - 1)           /* la ultima fila es la de estado */

/* --- El cubo de salida -----------------------------------------------
 *
 * Dos economias distintas, y conviene no confundirlas:
 *
 *   - Juntar la salida en un cubo y hacer UN write ahorra LLAMADAS AL
 *     SISTEMA. Redibujar una linea con veinte printf son veinte viajes a
 *     EL1; con el cubo es uno.
 *   - Redibujar poco ahorra BAUDIOS, que es lo caro: cada byte son 87
 *     microsegundos en el cable y eso no lo arregla ningun cubo.
 *
 * El primero se nota en el perfil, el segundo se nota con los ojos. */
static char  salida[SALIDA_CAP];
static int   salida_n;
static uint64_t bytes_enviados;      /* el instrumento: ver Ctrl-G */

static void suelta(void);

static void pon(const char *s, int n)
{
    /* Si no cabe se VACIA, no se tira. Tirar en silencio es la clase de
     * decision que se paga una hora despues buscando por que la pantalla
     * sale a medias. */
    while (n > 0) {
        if (salida_n == SALIDA_CAP) suelta();
        int hueco = SALIDA_CAP - salida_n;
        int cuanto = n < hueco ? n : hueco;
        memcpy(salida + salida_n, s, (uint64_t)cuanto);
        salida_n += cuanto;
        s += cuanto;
        n -= cuanto;
    }
}

static void pons(const char *s) { pon(s, (int)strlen(s)); }

static void ponf(const char *fmt, ...)
{
    char t[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if (n > 0) pon(t, n);
}

/* --- Y aqui la leccion que me costo una hora --------------------------
 *
 * write() NO escribe siempre todo lo que le pides. La consola del kernel
 * copia a un buffer de rebote de 128 bytes -BOUNCE en src/file.c, pequenyo
 * porque la pila de kernel es UNA pagina- se queda con los primeros 128 y
 * devuelve cuantos ha cogido. Es una escritura parcial, y es perfectamente
 * legal: lo dice POSIX y lo hace cualquier Unix en cuanto hay una tuberia o
 * un socket por medio.
 *
 * El editor es el primer programa de este proyecto que escribe mas de 128
 * bytes de una vez -antes nadie pasaba de una linea de texto- asi que es el
 * primero que podia descubrirlo. Sin el bucle, una pantalla de 2.000 bytes
 * salia cortada exactamente a los 128 y el resto desaparecia sin un error.
 * La libc ya lo hacia bien desde el paso 49; el codigo nuevo, no.
 *
 * De paso: con BOUNCE en 128, un redibujado completo son 16 llamadas al
 * sistema por mucho cubo que se le ponga delante. El cubo sigue valiendo -sin
 * el serian cien- pero el suelo lo pone el kernel. */
static void suelta(void)
{
    int o = 0;
    while (o < salida_n) {
        int64_t k = write(1, salida + o, (uint64_t)(salida_n - o));
        if (k > 0)                   { o += (int)k; continue; }
        if (k < 0 && errno == EINTR) continue;    /* un Ctrl-C: reintentar */
        break;                                    /* no hay nada que hacer */
    }
    bytes_enviados += (uint64_t)o;
    salida_n = 0;
}

/* --- Escapes ANSI, los cuatro que hacen falta ------------------------
 *
 * Y no mas. Un editor de verdad consulta terminfo para no dar por hecho que
 * el terminal entiende estos; aqui hay un solo terminal al otro lado del
 * cable y entiende ANSI, asi que la tabla sobra.
 *
 * Ojo con una cosa: la salida del kernel convierte cada '\n' en "\r\n", asi
 * que este editor NO usa saltos de linea para colocarse. Coloca el cursor
 * siempre a mano, con fila y columna. Es mas bytes por movimiento y es lo
 * unico que no depende de lo que haga el driver por su cuenta. */
static void ir_a(int f, int c)      { ponf("\033[%d;%dH", f + 1, c + 1); }
static void borrar_linea(void)      { pons("\033[K"); }
static void limpiar(void)           { pons("\033[2J"); }
static void inverso(int si)         { pons(si ? "\033[7m" : "\033[0m"); }

/* --- Pintar ----------------------------------------------------------- */

/* Una linea del fichero en una fila de la pantalla, recortada al trozo que
 * se esta viendo. */
static void pintar_linea(int f)
{
    int i = arriba + f;

    ir_a(f, 0);
    borrar_linea();

    if (i >= nlineas) {
        /* Mas alla del final del fichero. vi pone una tilde, y no es
         * decoracion: distingue "una linea vacia" de "aqui no hay linea". */
        pons("~");
        return;
    }

    int n = buf[i].n - izq;
    if (n > 0) {
        if (n > columnas) n = columnas;
        pon(buf[i].t + izq, n);
    }
}

static void pintar_estado(const char *aviso)
{
    char izqs[128];

    if (aviso && *aviso)
        snprintf(izqs, sizeof(izqs), "%s", aviso);
    else
        snprintf(izqs, sizeof(izqs), "%s%s  %d lineas",
                 ruta[0] ? ruta : "[sin nombre]", tocado ? " [+]" : "",
                 nlineas);

    /* La posicion va pegada al borde derecho, asi que la linea se monta a
     * mano: este printf entiende anchura fija ("%-20s") pero no anchura por
     * argumento ("%-*s"), y la pantalla puede tener el ancho que quiera. */
    char pos[32];
    int np = snprintf(pos, sizeof(pos), " %d,%d ", fila + 1, col + 1);

    char l[640];
    int n = snprintf(l, sizeof(l), " %s", izqs);
    int tope = columnas - np;
    if (tope > (int)sizeof(l) - 1) tope = (int)sizeof(l) - 1;
    if (n > tope) n = tope;
    while (n < tope) l[n++] = ' ';

    ir_a(filas - 1, 0);
    borrar_linea();
    inverso(1);

    /* El modo, SIEMPRE visible. El vi original no lo ensenyaba -no le
     * sobraban ni filas ni baudios- y es la queja mas repetida que ha tenido
     * un programa en la historia. Un estado que el usuario no puede ver es un
     * estado que el usuario va a adivinar mal. */
    pon(l, n);
    pon(pos, np);
    inverso(0);
}

/* Solo el contador de posicion, en su esquina.
 *
 * Existe porque el instrumento me llamo mentiroso. Habia escrito que mover el
 * cursor no costaba redibujado, y Ctrl-G decia 113 bytes por tecla: era la
 * linea de estado ENTERA, con sus 80 caracteres de relleno y sus escapes de
 * video inverso, repintada para que cambiara un numero de dos cifras.
 *
 * Repintar solo el numero son 22 bytes. Y aun asi es cinco veces mas que no
 * tener contador: el vi original no ensenyaba la posicion salvo que se la
 * pidieras con Ctrl-G, y ahora se por que. En vim la "regla" es una opcion, y
 * tambien viene apagada. */
static void pintar_posicion(void)
{
    char pos[32];
    int np = snprintf(pos, sizeof(pos), " %d,%d ", fila + 1, col + 1);

    ir_a(filas - 1, columnas - np);
    inverso(1);
    pon(pos, np);
    inverso(0);
}

/* Colocar el cursor donde esta de verdad, que es lo ultimo que se hace
 * siempre: si no, se queda donde lo dejo el ultimo dibujo. */
static void poner_cursor(void)
{
    ir_a(fila - arriba, col - izq);
}

static void pintar_todo(const char *aviso)
{
    limpiar();
    for (int f = 0; f < TEXTO; f++) pintar_linea(f);
    pintar_estado(aviso);
    tocado_pintado = tocado;
    poner_cursor();
    suelta();
}

static void estado_si_cambio(void)
{
    if (tocado != tocado_pintado) { pintar_estado(0); tocado_pintado = tocado; }
    else                           pintar_posicion();
}

/* Solo la linea del cursor. Esto es el 90% de los redibujados de una sesion
 * de edicion, y es la diferencia entre 60 bytes y 2.000. */
static void pintar_actual(void)
{
    pintar_linea(fila - arriba);
    estado_si_cambio();
    poner_cursor();
    suelta();
}

/* Y esto, cuando no ha cambiado ni una letra: la posicion y el cursor. */
static void solo_cursor(void)
{
    estado_si_cambio();
    poner_cursor();
    suelta();
}

/* --- Desplazar ---------------------------------------------------------
 *
 * Devuelve 1 si la VENTANA ha cambiado, o sea si hay que repintar todo. Que
 * esta funcion conteste eso es lo que permite que quien la llama sepa cuanto
 * tiene que dibujar sin tener que adivinarlo. */
static int ajustar(void)
{
    int antes_a = arriba, antes_i = izq;

    if (fila < arriba)            arriba = fila;
    if (fila >= arriba + TEXTO)   arriba = fila - TEXTO + 1;

    /* Desplazamiento horizontal. Hace falta de verdad: la linea mas larga de
     * este proyecto tiene 134 caracteres y la pantalla 80, asi que sin esto
     * no se podrian editar sus propias fuentes. */
    if (col < izq)                izq = col;
    if (col >= izq + columnas)    izq = col - columnas + 1;

    return arriba != antes_a || izq != antes_i;
}

/* --- El monton de una linea ------------------------------------------- */

static int cabe(int i, int n)
{
    if (n > MAX_COL) return 0;

    int cap = (buf[i].n + 64) & ~63;
    int nec = (n + 64) & ~63;

    if (nec <= cap && buf[i].t) return 1;

    char *nuevo = realloc(buf[i].t, (uint64_t)nec);
    if (!nuevo) return 0;
    buf[i].t = nuevo;
    return 1;
}

static int meter_linea(int donde, const char *t, int n)
{
    if (nlineas >= MAX_LINEAS) return 0;

    memmove(&buf[donde + 1], &buf[donde],
            (uint64_t)(nlineas - donde) * sizeof(struct linea));
    buf[donde].t = 0;
    buf[donde].n = 0;
    nlineas++;

    if (!cabe(donde, n)) return 0;
    if (n) memcpy(buf[donde].t, t, (uint64_t)n);
    buf[donde].n = n;
    return 1;
}

static void quitar_linea(int donde)
{
    free(buf[donde].t);
    memmove(&buf[donde], &buf[donde + 1],
            (uint64_t)(nlineas - donde - 1) * sizeof(struct linea));
    nlineas--;
    if (!nlineas) { meter_linea(0, "", 0); }
}

/* --- Cargar y guardar ------------------------------------------------- */

static int cargar(const char *r)
{
    FILE *f = fopen(r, "r");
    if (!f) return 0;                 /* fichero nuevo: se empieza vacio */

    char l[MAX_COL + 2];
    while (fgets(l, (int)sizeof(l), f)) {
        int n = (int)strlen(l);
        while (n && (l[n - 1] == '\n' || l[n - 1] == '\r')) n--;
        if (!meter_linea(nlineas, l, n)) break;
    }
    fclose(f);
    return 1;
}

static int guardar(const char *r)
{
    FILE *f = fopen(r, "w");
    if (!f) return 0;

    for (int i = 0; i < nlineas; i++) {
        if (buf[i].n && fwrite(buf[i].t, 1, (uint64_t)buf[i].n, f) != (uint64_t)buf[i].n)
            { fclose(f); return 0; }
        if (fputs("\n", f) < 0) { fclose(f); return 0; }
    }
    return fclose(f) == 0;
}

/* --- Leer teclas ------------------------------------------------------
 *
 * De una en una y con read() a pelo, no con getchar(). El cubo de stdio
 * guardaria teclas que el editor necesita AHORA: en modo crudo no hay lineas,
 * asi que no hay nada que le diga al cubo cuando parar de acumular. */
static int tecla(void)
{
    char c;
    for (;;) {
        int64_t r = read(0, &c, 1);
        if (r == 1)  return (unsigned char)c;
        if (r == 0)  return -1;               /* se acabo la entrada */
        if (errno == EINTR) continue;         /* un Ctrl-C: no es el fin */
        return -1;
    }
}

/* --- El terminal, que es prestado ------------------------------------- */
static int modo_antes;

static void soltar_terminal(void)
{
    ir_a(filas - 1, 0);
    borrar_linea();
    suelta();
    termios(modo_antes);
}

/* Ctrl-C. No hace nada, y es la primera vez en el proyecto que "no hacer
 * nada" evita perder TRABAJO y no solo un proceso: sin esto, la accion por
 * defecto mata al editor y se lleva consigo todo lo que no hubieras
 * guardado. Y llega aunque estemos en modo crudo, porque Ctrl-C se lo queda
 * el driver en la interrupcion, antes de la disciplina de linea. */
static void nada(int sig) { (void)sig; }

/* --- La linea de ordenes (los ":") ----------------------------------- */

/* Devuelve la orden escrita, o 0 si se cancelo con Ctrl-C o borrando todo. */
static const char *pedir(char inicial)
{
    static char orden[128];
    int n = 0;

    orden[n++] = inicial;

    for (;;) {
        ir_a(filas - 1, 0);
        borrar_linea();
        pon(orden, n);
        suelta();

        int c = tecla();

        if (c < 0) return 0;
        if (c == '\r' || c == '\n') { orden[n] = 0; return orden; }
        if (c == 27) return 0;                       /* Escape: cancelar */
        if (c == 8 || c == 127) {                    /* borrar */
            if (--n == 0) return 0;                  /* borraste los dos puntos */
            continue;
        }
        if (n < (int)sizeof(orden) - 1 && c >= 32 && c < 127) orden[n++] = (char)c;
    }
}

/* --- Buscar ----------------------------------------------------------
 * Sin expresiones regulares: texto tal cual. Las expresiones regulares son
 * el otro programa que habria que escribir, y no es este. */
static char patron[128];

static int buscar(int desde_fila, int desde_col)
{
    if (!patron[0]) return 0;

    for (int v = 0; v <= nlineas; v++) {
        int i = (desde_fila + v) % nlineas;
        int c = (v == 0) ? desde_col : 0;

        for (; c + (int)strlen(patron) <= buf[i].n; c++)
            if (!strncmp(buf[i].t + c, patron, strlen(patron))) {
                fila = i;
                col  = c;
                return 1;
            }
    }
    return 0;
}

/* --- Insertar --------------------------------------------------------- */

static void meter_char(int c)
{
    if (!cabe(fila, buf[fila].n + 1)) return;

    memmove(buf[fila].t + col + 1, buf[fila].t + col,
            (uint64_t)(buf[fila].n - col));
    buf[fila].t[col] = (char)c;
    buf[fila].n++;
    col++;
    tocado = 1;
}

/* Partir la linea en el cursor, que es lo que hace un Enter. */
static int partir(void)
{
    if (nlineas >= MAX_LINEAS) return 0;

    int resto = buf[fila].n - col;
    if (!meter_linea(fila + 1, buf[fila].t + col, resto)) return 0;

    buf[fila].n = col;
    fila++;
    col = 0;
    tocado = 1;
    return 1;
}

/* El modo insercion. Vuelve cuando se pulsa Escape.
 *
 * Que sea un bucle aparte y no un caso del switch de arriba es el modo hecho
 * codigo: mientras se esta aqui dentro, las letras son letras. Fuera, las
 * mismas letras son ordenes. */
static void insertar(void)
{
    int todo = ajustar();
    if (todo) pintar_todo("-- INSERTAR --");
    else { pintar_estado("-- INSERTAR --"); poner_cursor(); suelta(); }

    for (;;) {
        int c = tecla();

        if (c < 0 || c == 27) break;                 /* Escape */

        if (c == '\r' || c == '\n') {
            if (!partir()) break;
            pintar_todo("-- INSERTAR --");           /* cambio estructural */
            continue;
        }

        if (c == 8 || c == 127) {                    /* borrar hacia atras */
            if (col > 0) {
                memmove(buf[fila].t + col - 1, buf[fila].t + col,
                        (uint64_t)(buf[fila].n - col));
                buf[fila].n--;
                col--;
                tocado = 1;
                if (ajustar()) pintar_todo("-- INSERTAR --");
                else { pintar_linea(fila - arriba); pintar_posicion();
                       poner_cursor(); suelta(); }
            }
            continue;
        }

        if (c == 9) { for (int k = 0; k < 4; k++) meter_char(' '); }
        else if (c >= 32 && c < 127) meter_char(c);
        else continue;                                /* control: se ignora */

        /* Una tecla escrita repinta SU linea y el contador, y nada mas. El
         * "-- INSERTAR --" ya esta puesto desde que entramos y no cambia. */
        if (ajustar()) pintar_todo("-- INSERTAR --");
        else { pintar_linea(fila - arriba); pintar_posicion();
               poner_cursor(); suelta(); }
    }

    /* Al salir del modo insercion el cursor retrocede uno, como en vi: en
     * modo ordenes el cursor esta SOBRE un caracter, no entre dos. */
    if (col > 0) col--;

    /* Y la linea de estado se repinta ENTERA, no solo la posicion. Es la
     * unica salida del modo insercion, asi que es el unico sitio donde se
     * puede borrar el "-- INSERTAR --", y dejarlo puesto es el peor de los
     * fallos posibles en un programa modal: la pantalla diciendo que estas en
     * un modo en el que ya no estas. Un estado invisible se adivina mal; uno
     * visible y falso se cree. */
    if (ajustar()) {
        pintar_todo(0);
    } else {
        pintar_estado(0);
        tocado_pintado = tocado;
        poner_cursor();
        suelta();
    }
}

/* --- El bucle de ordenes --------------------------------------------- */

static int es_palabra(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

int main(int argc, char **argv)
{
    /* El tamanyo del terminal. No hay forma de preguntarselo -eso seria un
     * TIOCGWINSZ, y aqui no hay ioctl- asi que se da por hecho 80x24 y se
     * deja cambiarlo por el entorno, que ya existe desde el paso 40. */
    const char *l = getenv("LINES");
    const char *c = getenv("COLUMNS");
    if (l && atoi(l) > 4)  filas    = atoi(l);
    if (c && atoi(c) > 20) columnas = atoi(c);
    if (filas > 200) filas = 200;
    if (columnas > 512) columnas = 512;

    if (argc > 1) {
        snprintf(ruta, sizeof(ruta), "%s", argv[1]);
        cargar(ruta);
    }
    if (!nlineas) meter_linea(0, "", 0);

    /* El terminal, prestado: se apunta como estaba y se deja igual al irse.
     * Lo mismo que hace 'clave' con el eco, y por el mismo motivo: el
     * terminal es uno y lo comparten todos. */
    modo_antes = (int)termios(-1);
    termios(modo_antes & ~(T_ECO | T_CANONICO));

    signal(SIGINT, nada);

    /* Y Ctrl-Z tambien se traga, aunque duela: si el editor se detuviera,
     * dejaria el terminal en modo crudo y sin eco, y el shell se quedaria
     * escribiendo a ciegas. Suspender un editor de verdad exige devolver el
     * terminal al pararse y volver a cogerlo al seguir, y eso es un paso
     * aparte porque necesita SIGCONT y redibujar. */
    signal(SIGTSTP, nada);

    pintar_todo(0);

    for (;;) {
        int t = tecla();
        if (t < 0) break;

        switch (t) {

        /* --- Movimiento. Ni un byte de redibujado: solo el cursor. --- */
        case 'h': if (col > 0) col--; break;
        case 'l': if (col < buf[fila].n - 1) col++; break;
        case 'k': if (fila > 0) fila--; break;
        case 'j': if (fila < nlineas - 1) fila++; break;
        case '0': col = 0; break;
        case '$': col = buf[fila].n ? buf[fila].n - 1 : 0; break;
        case 'G': fila = nlineas - 1; col = 0; break;

        case 'g':
            if (tecla() == 'g') { fila = 0; col = 0; }
            break;

        case 'w':
            while (col < buf[fila].n && es_palabra(buf[fila].t[col]))  col++;
            while (col < buf[fila].n && !es_palabra(buf[fila].t[col])) col++;
            break;

        case 'b':
            if (col > 0) col--;
            while (col > 0 && !es_palabra(buf[fila].t[col])) col--;
            while (col > 0 && es_palabra(buf[fila].t[col - 1])) col--;
            break;

        /* --- Entrar en insercion, que son cuatro puertas al mismo sitio --- */
        case 'i': insertar(); continue;
        case 'a': if (buf[fila].n) col++; insertar(); continue;
        case 'A': col = buf[fila].n; insertar(); continue;

        case 'o':
            if (!meter_linea(fila + 1, "", 0)) break;
            fila++; col = 0; tocado = 1;
            pintar_todo("-- INSERTAR --");
            insertar();
            continue;

        case 'O':
            if (!meter_linea(fila, "", 0)) break;
            col = 0; tocado = 1;
            pintar_todo("-- INSERTAR --");
            insertar();
            continue;

        /* --- Borrar --- */
        case 'x':
            if (buf[fila].n) {
                memmove(buf[fila].t + col, buf[fila].t + col + 1,
                        (uint64_t)(buf[fila].n - col - 1));
                buf[fila].n--;
                if (col >= buf[fila].n && col > 0) col--;
                tocado = 1;
                pintar_actual();
                continue;
            }
            break;

        case 'D':
            if (col < buf[fila].n) {
                buf[fila].n = col;
                if (col > 0) col--;
                tocado = 1;
                pintar_actual();
                continue;
            }
            break;

        case 'd':
            if (tecla() == 'd') {
                quitar_linea(fila);
                if (fila >= nlineas) fila = nlineas - 1;
                col = 0;
                tocado = 1;
                ajustar();
                pintar_todo(0);          /* se ha movido todo lo de abajo */
                continue;
            }
            break;

        /* --- Buscar --- */
        case '/': {
            const char *o = pedir('/');
            if (o && o[1]) {
                snprintf(patron, sizeof(patron), "%s", o + 1);
                if (!buscar(fila, col + 1)) {
                    pintar_todo("no esta");
                    continue;
                }
            }
            ajustar();
            pintar_todo(0);
            continue;
        }

        case 'n':
            if (buscar(fila, col + 1)) { ajustar(); pintar_todo(0); }
            else pintar_todo("no esta");
            continue;

        /* --- El instrumento --- */
        case 7:                          /* Ctrl-G */
            { char m[128];
              snprintf(m, sizeof(m), "%s: %d lineas, %lu bytes al terminal",
                       ruta[0] ? ruta : "[sin nombre]", nlineas,
                       (unsigned long)bytes_enviados);
              pintar_estado(m); poner_cursor(); suelta(); }
            continue;

        case 12: pintar_todo(0); continue;    /* Ctrl-L: repinta todo */

        /* --- Los dos puntos --- */
        case ':': {
            const char *o = pedir(':');
            if (!o) { pintar_todo(0); continue; }

            if (!strcmp(o, ":q")) {
                if (tocado) { pintar_todo("hay cambios sin guardar (:q! para tirarlos)"); continue; }
                goto fin;
            }
            if (!strcmp(o, ":q!")) goto fin;

            if (!strcmp(o, ":w") || !strcmp(o, ":wq") || !strncmp(o, ":w ", 3)) {
                const char *destino = (o[2] == ' ') ? o + 3 : ruta;
                if (!destino[0]) { pintar_todo("no hay nombre: usa :w fichero"); continue; }
                if (!guardar(destino)) {
                    char m[160];
                    snprintf(m, sizeof(m), "no he podido escribir %s: %s",
                             destino, strerror(errno));
                    pintar_todo(m);
                    continue;
                }
                if (destino != ruta) snprintf(ruta, sizeof(ruta), "%s", destino);
                tocado = 0;
                if (!strcmp(o, ":wq")) goto fin;
                char m[160];
                snprintf(m, sizeof(m), "escrito %s: %d lineas", ruta, nlineas);
                pintar_todo(m);
                continue;
            }

            pintar_todo("no entiendo esa orden");
            continue;
        }

        default:
            continue;                    /* lo que no se entiende, se ignora */
        }

        /* Todo lo que cae aqui es movimiento: la linea no ha cambiado. */
        if (col > buf[fila].n - 1) col = buf[fila].n ? buf[fila].n - 1 : 0;
        if (ajustar()) pintar_todo(0);
        else           solo_cursor();
    }

fin:
    soltar_terminal();
    printf("\n");
    return 0;
}
