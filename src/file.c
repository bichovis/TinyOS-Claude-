/* file.c - Descriptores de fichero y tuberias
 *
 * Una tuberia es una cola de bytes con dos extremos y una regla que la
 * hace util: quien lee se para si no hay nada, y quien escribe se para si
 * no cabe. Esa espera es todo el mecanismo de sincronizacion que hace
 * falta para encadenar programas, y por eso "a | b" funciona sin que a y
 * b sepan el uno del otro.
 *
 * El detalle que la cierra es el final de fichero: cuando se cierra el
 * ULTIMO extremo de escritura, quien lee deja de esperar y recibe un cero.
 * Sin eso, el segundo programa de una tuberia no terminaria nunca.
 */
#include <stdint.h>
#include "file.h"
#include "sched.h"
#include "mm.h"
#include "uart.h"
#include "spinlock.h"
#include "ipc.h"
#include "fs_abi.h"

/* La consola es una sola para todo el mundo y no se cierra nunca. */
static struct fichero consola = { F_CONSOLA, 1, 0, { 0 }, 0 };

/* Una transaccion con el servidor de ficheros cada vez (ver mas abajo). */
static struct mutex fs_mtx;

struct fichero *file_consola(void) { return &consola; }

void file_init(void) { mutex_init(&fs_mtx); }

/* Copiar entre el espacio del proceso y el kernel con un buffer de por
 * medio.
 *
 * El rodeo no es capricho: las colas de la tuberia se manejan con
 * sched_lock cogido, y tocar memoria de usuario ahi dentro puede provocar
 * un fallo de pagina -la pila crece, una pagina es COW- que acabaria
 * pidiendo ese mismo cerrojo. Un interbloqueo contra uno mismo. Asi que
 * primero se copia fuera, y luego se entra. */
#define BOUNCE 128

static uint64_t copiar_de_usuario(char *dst, uint64_t uva, uint64_t n)
{
    return n - copy_from_user(dst, uva, n);       /* los que si cupieron */
}

static uint64_t copiar_a_usuario(uint64_t uva, const char *src, uint64_t n)
{
    /* Una pagina compartida por un fork es de solo lectura, asi que sttr
     * falla: el manejador ve un fallo de permisos sobre memoria de
     * usuario, hace la copia privada y reintenta. Antes eso se pedia a
     * mano con user_touch_w; ahora sale del mismo camino que todo lo
     * demas. */
    return n - copy_to_user(uva, src, n);
}

/* --- La tuberia ------------------------------------------------------- */

static int64_t pipe_read(struct pipe *p, uint64_t uva, uint64_t n)
{
    char tmp[BOUNCE];
    if (n > BOUNCE) n = BOUNCE;

    uint64_t flags = sched_lock_irqsave();

    while (p->count == 0) {
        /* Nadie va a escribir nunca mas: esto es el final del fichero, y
         * es lo que hace que el segundo programa de una tuberia termine. */
        if (p->escritores == 0) { sched_unlock_irqrestore(flags); return 0; }

        if (wq_wait(&p->hay_datos) < 0) {
            sched_unlock_irqrestore(flags);
            return -1;                      /* nos interrumpio una senyal */
        }
    }

    uint64_t i = 0;
    while (i < n && p->count) {
        tmp[i++] = p->buf[p->tail];
        p->tail  = (p->tail + 1) % PIPE_BUF;
        p->count--;
    }

    wq_wake_all(&p->hay_hueco);
    sched_unlock_irqrestore(flags);

    return (int64_t)copiar_a_usuario(uva, tmp, i);
}

static int64_t pipe_write(struct pipe *p, uint64_t uva, uint64_t n)
{
    char tmp[BOUNCE];
    if (n > BOUNCE) n = BOUNCE;

    uint64_t hay = copiar_de_usuario(tmp, uva, n);
    if (!hay) return -1;

    uint64_t flags = sched_lock_irqsave();

    while (p->count == PIPE_BUF) {
        /* Escribir en una tuberia que ya no lee nadie no tiene sentido.
         * Un Unix manda SIGPIPE; aqui basta con decir que fallo. */
        if (p->lectores == 0) { sched_unlock_irqrestore(flags); return -1; }

        if (wq_wait(&p->hay_hueco) < 0) {
            sched_unlock_irqrestore(flags);
            return -1;
        }
    }

    uint64_t i = 0;
    while (i < hay && p->count < PIPE_BUF) {
        p->buf[p->head] = tmp[i++];
        p->head = (p->head + 1) % PIPE_BUF;
        p->count++;
    }

    wq_wake_all(&p->hay_datos);
    sched_unlock_irqrestore(flags);
    return (int64_t)i;
}

/* --- La consola ------------------------------------------------------- */

static int64_t consola_write(uint64_t uva, uint64_t n)
{
    char tmp[BOUNCE];
    if (n > BOUNCE) n = BOUNCE;

    uint64_t hay = copiar_de_usuario(tmp, uva, n);
    if (!hay) return -1;

    uint64_t lf = uart_begin();
    for (uint64_t i = 0; i < hay; i++) {
        if (tmp[i] == '\n') uart_putc('\r');
        uart_putc(tmp[i]);
    }
    uart_end(lf);
    return (int64_t)hay;
}

static int64_t consola_read(uint64_t uva, uint64_t n)
{
    if (n == 0) return 0;

    int c = uart_getc_blocking();
    if (c < 0) return -1;                   /* senyal */

    char b = (char)c;
    return (int64_t)copiar_a_usuario(uva, &b, 1);
}

/* --- Ficheros: el kernel como CLIENTE del servidor -------------------
 *
 * Hasta aqui el kernel solo recibia peticiones. Para redirigir con > y <
 * tiene que hacer lo contrario: pedirle algo a un proceso de EL0, y
 * esperar la respuesta.
 *
 * No es tan raro como suena. El hilo que hace read() ya esta dentro del
 * kernel, con su pila y su entrada en la tabla de tareas; puede mandar un
 * mensaje y dormirse esperando la contestacion igual que se duerme
 * esperando una tecla. Lo unico que le falta es un puerto donde recibirla,
 * y de eso se encarga PORT_KERNEL: un puerto de duenyo 0, que es un pid
 * que no existe -los procesos empiezan en CORES- y por tanto solo el
 * kernel puede vaciar.
 *
 * La direccion de la dependencia es la que importa: el kernel no llama al
 * servidor, le escribe. Si el servidor no esta, el mensaje no llega a
 * ninguna parte y read() devuelve error; no hay nada que se cuelgue
 * esperando codigo que no existe.
 *
 * Una transaccion cada vez. Hay un solo puerto de respuesta y las
 * respuestas no llevan marca de a quien pertenecen, asi que dos lecturas a
 * la vez podrian llevarse la contestacion cambiada. Un mutex lo evita, al
 * precio de serializar la tarjeta -que de todas formas es un solo
 * dispositivo y solo sabe atender una cosa a la vez-. */
static int fs_listo;

static int fs_transaccion(uint64_t tipo, const char *nombre, uint64_t off,
                          const char *datos, uint64_t len,
                          struct message *resp)
{
    mutex_lock(&fs_mtx);

    if (!fs_listo) {
        if (port_create(0, PORT_KERNEL) != PORT_KERNEL) {
            mutex_unlock(&fs_mtx);
            return -1;
        }
        fs_listo = 1;
    }

    /* El mensaje, ESTATICO y no en la pila.
     *
     * Son 536 bytes desde que el mensaje crecio a 512 de datos, y la pila
     * de kernel son 4 KB con una pagina de guarda debajo. Con el mensaje
     * aqui y otro en el que llama, mas dos o tres rutas de 256 en el
     * despachador, la suma se acerca peligrosamente.
     *
     * Y es seguro porque ya lo era: fs_mtx garantiza UNA transaccion a la
     * vez. El buffer no se puede compartir mal porque no hay con quien
     * compartirlo. Lo que antes era una consecuencia del disenyo pasa a
     * ser algo que el disenyo aprovecha. */
    static struct message m;

    for (uint64_t i = 0; i < sizeof(m.data); i++) m.data[i] = 0;

    struct fs_request *r = (struct fs_request *)m.data;
    m.from = 0;
    m.type = tipo;

    /* len son los BYTES UTILES DE data[], no el tamanyo de la peticion.
     * Es lo unico que le dice al servidor cuantos bytes escribir; mandarle
     * el sizeof entero hacia que escribiera los 96 del buffer, ceros
     * incluidos, y los ficheros salian con el tamanyo redondeado a
     * multiplos de FS_CHUNK. */
    m.len  = len;
    r->port = PORT_KERNEL;
    r->arg  = off;
    for (int i = 0; i < FICH_NOMBRE && nombre[i]; i++) r->name[i] = nombre[i];
    if (datos)
        for (uint64_t i = 0; i < len && i < FS_CHUNK; i++) r->data[i] = datos[i];

    int ok = -1;
    if (port_send(PORT_FILES, &m) == 0 &&
        port_recv(PORT_KERNEL, resp, 0) == 0)
        ok = 0;

    mutex_unlock(&fs_mtx);
    return ok;
}

/* Leer un trozo de fichero a memoria del KERNEL.
 *
 * Es lo mismo que hace fichero_read, pero sin usuario de por medio: el
 * destino es una pagina fisica recien pedida, vista por el mapa lineal. La
 * necesita el fallo de pagina de un fichero mapeado, que tiene que
 * rellenar la pagina ANTES de que el proceso pueda verla.
 *
 * Devuelve los bytes leidos, que pueden ser menos de los pedidos si el
 * fichero se acaba. */
int64_t fs_leer_en(const char *ruta, uint64_t off, char *dst, uint64_t n)
{
    uint64_t hechos = 0;

    while (hechos < n) {
        struct message resp;
        if (fs_transaccion(FS_READ, ruta, off + hechos, 0, 0, &resp) < 0)
            return hechos ? (int64_t)hechos : -1;

        if (resp.type == FS_EOF) break;          /* se acabo el fichero */
        if (resp.type != FS_OK)  return hechos ? (int64_t)hechos : -1;

        uint64_t hay = resp.len;
        if (hay == 0) break;
        if (hay > n - hechos) hay = n - hechos;

        for (uint64_t i = 0; i < hay; i++) dst[hechos + i] = resp.data[i];
        hechos += hay;
    }

    return (int64_t)hechos;
}

int64_t fs_tamano(const char *ruta)
{
    struct message resp;
    if (fs_transaccion(FS_SIZE, ruta, 0, 0, 0, &resp) < 0) return -1;
    if (resp.type != FS_OK) return -1;

    struct fs_info *i = (struct fs_info *)resp.data;
    return (i->flags & FS_ES_DIR) ? -1 : (int64_t)i->size;
}

int fs_es_directorio(const char *ruta)
{
    struct message resp;
    if (fs_transaccion(FS_SIZE, ruta, 0, 0, 0, &resp) < 0) return 0;
    if (resp.type != FS_OK) return 0;

    struct fs_info *i = (struct fs_info *)resp.data;
    return (i->flags & FS_ES_DIR) ? 1 : 0;
}

/* Las que solo necesitan el nombre. Todas son la misma frase dicha cuatro
 * veces: manda esta peticion y mira si dijo que si. */
int fs_borrar(const char *ruta)
{
    struct message r;
    if (fs_transaccion(FS_DELETE, ruta, 0, 0, 0, &r) < 0) return -1;
    return r.type == FS_OK ? 0 : -1;
}

int fs_mkdir(const char *ruta)
{
    struct message r;
    if (fs_transaccion(FS_MKDIR, ruta, 0, 0, 0, &r) < 0) return -1;
    return r.type == FS_OK ? 0 : -1;
}

int fs_rmdir(const char *ruta)
{
    struct message r;
    if (fs_transaccion(FS_RMDIR, ruta, 0, 0, 0, &r) < 0) return -1;
    return r.type == FS_OK ? 0 : -1;
}

int fs_renombrar(const char *origen, const char *destino)
{
    uint64_t n = 0;
    while (destino[n] && n < FS_CHUNK - 1) n++;

    struct message r;
    if (fs_transaccion(FS_RENAME, origen, 0, destino, n + 1, &r) < 0) return -1;
    return r.type == FS_OK ? 0 : -1;
}

int fs_estado(const char *ruta, uint64_t *tam, uint64_t *mtime, uint64_t *flags)
{
    struct message r;
    if (fs_transaccion(FS_SIZE, ruta, 0, 0, 0, &r) < 0) return -1;
    if (r.type != FS_OK) return -1;

    struct fs_info *i = (struct fs_info *)r.data;
    if (tam)   *tam   = i->size;
    if (mtime) *mtime = i->mtime;
    if (flags) *flags = i->flags;
    return 0;
}

/* Un directorio abierto es un descriptor con un INDICE dentro, no un
 * desplazamiento en bytes. El protocolo del servidor pide las entradas de
 * una en una por numero, asi que el descriptor solo tiene que acordarse
 * de por cual iba.
 *
 * Que eso quepa en la misma struct fichero que un fichero normal no es
 * casualidad: "lo que un proceso tiene abierto" es un concepto, y los
 * tipos son variaciones suyas. */
struct fichero *file_opendir(const char *ruta)
{
    uint64_t flags = 0;
    if (fs_estado(ruta, 0, 0, &flags) < 0) return 0;
    if (!(flags & FS_ES_DIR)) return 0;

    struct fichero *f = kmalloc(sizeof(struct fichero));
    if (!f) return 0;

    f->tipo = F_DIRECTORIO;
    f->refs = 1;
    f->p    = 0;
    f->off  = 0;                          /* aqui es el indice */
    for (int i = 0; i < FICH_NOMBRE; i++) f->nombre[i] = 0;
    for (int i = 0; i < FICH_NOMBRE - 1 && ruta[i]; i++) f->nombre[i] = ruta[i];
    return f;
}

/* Devuelve 1 si hay entrada, 0 si se acabo, -1 si algo fue mal. */
int file_readdir(struct fichero *f, void *info)
{
    if (!f || f->tipo != F_DIRECTORIO) return -1;

    struct message r;
    if (fs_transaccion(FS_LIST, f->nombre, f->off, 0, 0, &r) < 0) return -1;
    if (r.type == FS_EOF) return 0;
    if (r.type != FS_OK)  return -1;

    f->off++;
    /* Un bucle y no kcopy: kcopy vive en sched.c y no esta declarada aqui.
     * Son 88 bytes; el bucle dice lo mismo y no ata dos ficheros que no
     * tienen por que conocerse. */
    for (uint64_t i = 0; i < sizeof(struct fs_info); i++)
        ((char *)info)[i] = r.data[i];
    return 1;
}

/* Mover el punto por donde va un fichero.
 *
 * Solo tiene sentido en un fichero: una tuberia no se puede rebobinar
 * -los bytes ya no estan- y la consola tampoco. Devolver un error ahi no
 * es una carencia, es la verdad. */
int64_t file_seek(struct fichero *f, int64_t desplazamiento, int desde)
{
    if (!f || f->tipo != F_FICHERO) return -1;

    int64_t base;
    switch (desde) {
    case DESDE_INICIO: base = 0; break;
    case DESDE_ACTUAL: base = (int64_t)f->off; break;
    case DESDE_FINAL: {
        uint64_t tam = 0;
        if (fs_estado(f->nombre, &tam, 0, 0) < 0) return -1;
        base = (int64_t)tam;
        break;
    }
    default: return -1;
    }

    int64_t nuevo = base + desplazamiento;
    if (nuevo < 0) return -1;              /* antes del principio no hay nada */

    f->off = (uint64_t)nuevo;
    return nuevo;
}

struct fichero *file_open(const char *nombre, int modo)
{
    struct message resp;

    /* Leer exige que exista; escribir exige lo contrario: crearlo, y
     * vaciarlo si ya estaba. Eso es exactamente lo que significa ">". */
    if (modo == O_ESCRIBIR) {
        if (fs_transaccion(FS_CREATE, nombre, 0, 0, 0, &resp) < 0) return 0;
        if (resp.type != FS_OK) return 0;
    } else {
        if (fs_transaccion(FS_SIZE, nombre, 0, 0, 0, &resp) < 0) return 0;
        if (resp.type != FS_OK) return 0;
    }

    struct fichero *f = kmalloc(sizeof(struct fichero));
    if (!f) return 0;

    f->tipo = F_FICHERO;
    f->refs = 1;
    f->p    = 0;
    f->off  = 0;
    for (int i = 0; i < FICH_NOMBRE; i++) f->nombre[i] = 0;
    for (int i = 0; i < FICH_NOMBRE - 1 && nombre[i]; i++) f->nombre[i] = nombre[i];
    return f;
}

static int64_t fichero_read(struct fichero *f, uint64_t uva, uint64_t n)
{
    if (n > FS_CHUNK) n = FS_CHUNK;

    struct message resp;
    if (fs_transaccion(FS_READ, f->nombre, f->off, 0, 0, &resp) < 0) return -1;
    if (resp.type == FS_EOF) return 0;            /* se acabo el fichero */
    if (resp.type != FS_OK)  return -1;

    uint64_t hay = resp.len;
    if (hay > n) hay = n;

    uint64_t puestos = copiar_a_usuario(uva, resp.data, hay);
    f->off += puestos;
    return (int64_t)puestos;
}

/* Escribir TODO lo que pidan, dando tantas vueltas como haga falta.
 *
 * Un mensaje solo lleva FS_CHUNK bytes utiles, asi que la tentacion es
 * escribir 96 y devolver 96. Es legal -write() puede devolver menos de lo
 * que se le pide, y el que llama esta obligado a repetir- pero es una
 * trampa: casi nadie repite, y el fallo no se ve. Sale un fichero de 110
 * bytes donde tenia que haber 116, con un trozo de en medio ausente, y
 * como el principio y el final estan bien parece correcto.
 *
 * Que una interfaz PERMITA algo no quiere decir que convenga hacerlo. */
static int64_t fichero_write(struct fichero *f, uint64_t uva, uint64_t n)
{
    uint64_t puestos = 0;

    while (puestos < n) {
        uint64_t trozo = n - puestos;
        if (trozo > FS_CHUNK) trozo = FS_CHUNK;

        char tmp[FS_CHUNK];
        uint64_t hay = copiar_de_usuario(tmp, uva + puestos, trozo);
        if (hay == 0) break;                  /* memoria ilegible */

        struct message resp;
        if (fs_transaccion(FS_WRITE, f->nombre, f->off, tmp, hay, &resp) < 0 ||
            resp.type != FS_OK)
            return puestos ? (int64_t)puestos : -1;

        f->off  += hay;
        puestos += hay;
    }

    return (int64_t)puestos;
}

/* --- La interfaz comun ------------------------------------------------ */

int64_t file_read(struct fichero *f, uint64_t uva, uint64_t n)
{
    if (!f) return -1;
    if (f->tipo == F_CONSOLA) return consola_read(uva, n);
    if (f->tipo == F_PIPE_R)  return pipe_read(f->p, uva, n);
    if (f->tipo == F_FICHERO) return fichero_read(f, uva, n);
    return -1;                              /* el extremo que no toca */
}

int64_t file_write(struct fichero *f, uint64_t uva, uint64_t n)
{
    if (!f) return -1;
    if (f->tipo == F_CONSOLA) return consola_write(uva, n);
    if (f->tipo == F_PIPE_W)  return pipe_write(f->p, uva, n);
    if (f->tipo == F_FICHERO) return fichero_write(f, uva, n);
    return -1;
}

void file_dup(struct fichero *f)
{
    if (!f || f == &consola) return;        /* la consola no se cuenta */

    uint64_t flags = sched_lock_irqsave();
    f->refs++;
    sched_unlock_irqrestore(flags);
}

void file_close(struct fichero *f)
{
    if (!f || f == &consola) return;

    uint64_t flags = sched_lock_irqsave();

    if (--f->refs > 0) { sched_unlock_irqrestore(flags); return; }

    /* Este era el ultimo descriptor de este extremo. Avisar al otro lado:
     * si era el de escritura, quien lee tiene que despertarse para
     * enterarse de que se acabo. */
    struct pipe *p = f->p;
    if (p) {
        if (f->tipo == F_PIPE_R) { p->lectores--;   wq_wake_all(&p->hay_hueco); }
        else                     { p->escritores--; wq_wake_all(&p->hay_datos); }
    }

    int muerta = p && p->lectores == 0 && p->escritores == 0;
    sched_unlock_irqrestore(flags);

    kfree(f);
    if (muerta) kfree(p);
}

int file_pipe(struct fichero **lectura, struct fichero **escritura)
{
    struct pipe    *p = kmalloc(sizeof(struct pipe));
    struct fichero *r = kmalloc(sizeof(struct fichero));
    struct fichero *w = kmalloc(sizeof(struct fichero));

    if (!p || !r || !w) { kfree(p); kfree(r); kfree(w); return -1; }

    p->head = p->tail = p->count = 0;
    p->lectores = p->escritores = 1;
    p->hay_datos.head = p->hay_datos.tail = 0;
    p->hay_hueco.head = p->hay_hueco.tail = 0;

    r->tipo = F_PIPE_R; r->refs = 1; r->p = p;
    w->tipo = F_PIPE_W; w->refs = 1; w->p = p;

    *lectura = r;
    *escritura = w;
    return 0;
}
