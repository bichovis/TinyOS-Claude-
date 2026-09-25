/* lib/red.c - La red, para un programa cualquiera (ver red.h)
 *
 * Un programa tiene UN puerto de IPC para todo lo de red, que se crea la
 * primera vez que hace falta. Por el llegan las respuestas de la pila, los
 * datagramas de todos sus enchufes y las alarmas con que se miden las
 * esperas. Como todo entra por el mismo sitio, lo que no es lo que se
 * estaba esperando no se tira: un datagrama para otro enchufe se guarda
 * -uno por enchufe- hasta que alguien pregunte por el.
 */
#include <stdint.h>
#include <string.h>
#include "errno.h"
#include "syscall.h"
#include "net_abi.h"
#include "red.h"

#define ENCHUFES 4

static int64_t  puerto = -1;
static struct message m;

static struct {
    int      abierto;
    int      local;
    int      guardado;                 /* hay un datagrama esperando */
    uint32_t g_ip; int g_puerto; int g_n;
    uint8_t  g_datos[UDP_DATOS_MAX];
} ench[ENCHUFES];

static int mi_puerto(void)
{
    if (puerto < 0) puerto = port_create(-1);
    return puerto >= 0 ? 0 : -1;
}

/* Guardar un datagrama que ha llegado para un enchufe que no es el que se
 * estaba escuchando. Si ya habia uno, el nuevo tapa al viejo: UDP no promete
 * nada. */
static void guardar(const struct umsg_dgrama *g)
{
    for (int i = 0; i < ENCHUFES; i++) {
        if (!ench[i].abierto || ench[i].local != (int)g->local) continue;
        ench[i].guardado = 1;
        ench[i].g_ip = (uint32_t)g->ip; ench[i].g_puerto = (int)g->puerto;
        ench[i].g_n = (int)g->n;
        if (ench[i].g_n > UDP_DATOS_MAX) ench[i].g_n = UDP_DATOS_MAX;
        memcpy(ench[i].g_datos, g->datos, (size_t)ench[i].g_n);
        return;
    }
}

/* Esperar una respuesta de la pila de un tipo concreto, hasta 'decimas'
 * decimas de segundo (0 = sin limite). Lo demas que llegue se atiende. */
static int red_esperar(unsigned long tipo_ok, int decimas)
{
    if (decimas > 0 && alarma((uint64_t)puerto, (uint64_t)decimas * 10) < 0) decimas = 0;
    int r = -1;
    for (;;) {
        if (msg_recv((uint64_t)puerto, &m) < 0) { errno = EIO; break; }
        if (m.type == tipo_ok) { r = 0; break; }
        if (m.type == UMSG_ERROR) { errno = EIO; break; }
        if (m.type == CMSG_ALARMA) { errno = EAGAIN; r = 1; break; }
        if (m.type == UMSG_DATAGRAMA) guardar((const struct umsg_dgrama *)m.data);
    }
    if (decimas > 0) alarma((uint64_t)puerto, 0);
    return r;
}

int udp_abrir(int puerto_local)
{
    if (mi_puerto() < 0) { errno = EIO; return -1; }
    int i = 0;
    while (i < ENCHUFES && ench[i].abierto) i++;
    if (i == ENCHUFES) { errno = EAGAIN; return -1; }

    struct umsg_abrir *a = (struct umsg_abrir *)m.data;
    m.type = UMSG_ABRIR; m.len = sizeof(*a);
    a->port = (unsigned long)puerto; a->local = (unsigned long)puerto_local;
    if (msg_send(PORT_RED, &m) < 0) { errno = EIO; return -1; }
    if (red_esperar(UMSG_ABIERTO, 50) != 0) return -1;

    ench[i].abierto = 1;
    ench[i].local = (int)((const struct umsg_abrir *)m.data)->local;
    ench[i].guardado = 0;
    return i;
}

int udp_puerto(int s)
{
    return (s >= 0 && s < ENCHUFES && ench[s].abierto) ? ench[s].local : -1;
}

int udp_enviar(int s, uint32_t ip, int puerto_dst, const void *datos, int n)
{
    if (s < 0 || s >= ENCHUFES || !ench[s].abierto) { errno = EBADF; return -1; }
    if (n < 0 || n > UDP_DATOS_MAX) { errno = EINVAL; return -1; }

    static struct message t;
    struct umsg_dgrama *g = (struct umsg_dgrama *)t.data;
    t.type = UMSG_ENVIAR;
    t.len  = sizeof(*g) - UDP_DATOS_MAX + (uint64_t)n;
    g->local = (unsigned long)ench[s].local; g->ip = ip; g->puerto = (unsigned long)puerto_dst;
    g->n = (unsigned long)n;
    memcpy(g->datos, datos, (size_t)n);
    if (msg_send(PORT_RED, &t) < 0) { errno = EIO; return -1; }
    return n;
}

int udp_recibir(int s, uint32_t *ip, int *puerto_src, void *datos, int max, int decimas)
{
    if (s < 0 || s >= ENCHUFES || !ench[s].abierto) { errno = EBADF; return -1; }

    if (ench[s].guardado) {                 /* ya habia llegado uno */
        ench[s].guardado = 0;
        int n = ench[s].g_n < max ? ench[s].g_n : max;
        memcpy(datos, ench[s].g_datos, (size_t)n);
        if (ip) *ip = ench[s].g_ip;
        if (puerto_src) *puerto_src = ench[s].g_puerto;
        return n;
    }

    if (decimas > 0 && alarma((uint64_t)puerto, (uint64_t)decimas * 10) < 0) decimas = 0;
    int r = -1;
    for (;;) {
        if (msg_recv((uint64_t)puerto, &m) < 0) { errno = EIO; break; }
        if (m.type == CMSG_ALARMA) { r = 0; break; }
        if (m.type != UMSG_DATAGRAMA) continue;
        const struct umsg_dgrama *g = (const struct umsg_dgrama *)m.data;
        if ((int)g->local != ench[s].local) { guardar(g); continue; }
        int n = (int)g->n;
        if (n > UDP_DATOS_MAX) n = UDP_DATOS_MAX;
        if (n > max) n = max;
        memcpy(datos, g->datos, (size_t)n);
        if (ip) *ip = (uint32_t)g->ip;
        if (puerto_src) *puerto_src = (int)g->puerto;
        r = n;
        break;
    }
    if (decimas > 0) alarma((uint64_t)puerto, 0);
    return r;
}

void udp_cerrar(int s)
{
    if (s < 0 || s >= ENCHUFES || !ench[s].abierto) return;
    struct umsg_abrir *a = (struct umsg_abrir *)m.data;
    m.type = UMSG_CERRAR; m.len = sizeof(*a);
    a->port = (unsigned long)puerto; a->local = (unsigned long)ench[s].local;
    msg_send(PORT_RED, &m);
    ench[s].abierto = 0;
}

int resolver(const char *nombre, uint32_t *ip)
{
    uint32_t directa = ip_leer(nombre);
    if (directa) { *ip = directa; return 0; }
    if (mi_puerto() < 0) { errno = EIO; return -1; }

    struct umsg_resolver *r = (struct umsg_resolver *)m.data;
    m.type = UMSG_RESOLVER; m.len = sizeof(*r);
    r->port = (unsigned long)puerto; r->ip = 0;
    int k = 0; for (; nombre[k] && k < 63; k++) r->nombre[k] = nombre[k];
    r->nombre[k] = 0;
    if (msg_send(PORT_RED, &m) < 0) { errno = EIO; return -1; }
    if (red_esperar(UMSG_RESUELTO, 100) != 0) return -1;      /* diez segundos */
    *ip = (uint32_t)((const struct umsg_resolver *)m.data)->ip;
    return 0;
}

int red_estado(struct red_estado *e)
{
    if (mi_puerto() < 0) { errno = EIO; return -1; }
    struct umsg_pedir *p = (struct umsg_pedir *)m.data;
    m.type = UMSG_INFO; m.len = sizeof(*p);
    p->port = (unsigned long)puerto;
    if (msg_send(PORT_RED, &m) < 0) { errno = EIO; return -1; }
    if (red_esperar(UMSG_INFO_OK, 20) != 0) return -1;
    const struct umsg_info *i = (const struct umsg_info *)m.data;
    e->ip = (uint32_t)i->ip; e->mascara = (uint32_t)i->mascara; e->router = (uint32_t)i->router;
    e->dns = (uint32_t)i->dns; e->ntp = (uint32_t)i->ntp; e->estado = (int)i->estado;
    memcpy(e->mac, i->mac, 6);
    memcpy(e->tarjeta, i->tarjeta, sizeof(e->tarjeta));
    return 0;
}

uint32_t ip_leer(const char *t)
{
    uint32_t v = 0; int partes = 0;
    while (*t) {
        if (*t < '0' || *t > '9') return 0;
        unsigned x = 0;
        while (*t >= '0' && *t <= '9') { x = x * 10 + (unsigned)(*t - '0'); if (x > 255) return 0; t++; }
        v = (v << 8) | x; partes++;
        if (*t == '.') { t++; if (!*t) return 0; }
        else if (*t) return 0;
    }
    return partes == 4 ? v : 0;
}

void ip_texto(uint32_t ip, char *dst)
{
    int n = 0;
    for (int k = 3; k >= 0; k--) {
        unsigned b = (ip >> (8 * k)) & 255;
        if (b >= 100) dst[n++] = (char)('0' + b / 100);
        if (b >= 10)  dst[n++] = (char)('0' + (b / 10) % 10);
        dst[n++] = (char)('0' + b % 10);
        if (k) dst[n++] = '.';
    }
    dst[n] = 0;
}
