/* user/red.c - La pila de red: ARP, IPv4, ICMP, UDP y DHCP, en espacio de usuario
 *
 * La tarjeta la lleva el driver de USB; esto es lo que le da sentido a lo
 * que sale y entra por ella. Es un servidor como el de ficheros: vive en
 * el puerto PORT_RED, recibe tramas del driver (NMSG_TRAMA), le manda las
 * suyas (NMSG_ENVIAR) y no toca ningun registro. Si se cuelga, se cuelga
 * el; el teclado y el pendrive, que viven en el driver, siguen.
 *
 * Una pila de red es una cebolla de cabeceras, cada una con su direccion:
 *
 *   Ethernet   14 bytes   de que tarjeta a que tarjeta (MAC)   ---.
 *   IPv4       20 bytes   de que maquina a que maquina (IP)       |  esto
 *   UDP         8 bytes   de que programa a que programa (puerto)  |  es lo
 *   DHCP     ~300 bytes   "alguien me da una direccion?"       ---'  que va
 *
 * Y dos protocolos que no llevan nada dentro, solo sirven a los demas: ARP,
 * que traduce una IP a la MAC que la tiene (sin el no se sabe a que tarjeta
 * mandar una trama), e ICMP, que es como una maquina dice "estoy aqui" (el
 * ping). Con esos cinco -Ethernet, ARP, IP, ICMP, UDP- y un cliente DHCP
 * para conseguir direccion, la Pi es una maquina mas de la red: se le puede
 * hacer ping y puede hablar con cualquiera. Encima de UDP van DNS y NTP
 * -la hora- y los "enchufes": puertos UDP que cualquier programa se queda
 * para hablar con la red por mensajes.
 *
 * Todo es big-endian ("orden de red"): el byte alto primero. El ARM guarda
 * los numeros al reves, asi que aqui no se lee ni escribe un numero
 * directamente de una cabecera nunca; se hace byte a byte, con be16/be32.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "syscall.h"
#include "net_abi.h"

/* --- Lo que sabemos de nosotros ------------------------------------------ */
static uint64_t driver;                  /* puerto del driver; 0 = sin tarjeta */
static uint8_t  mi_mac[6];               /* 'mac' a secas es la llamada al sistema */
static char     tarjeta[16];
static uint32_t ip, mascara, router, dns, ntp;   /* 0 = no lo tenemos */

/* --- Bytes en orden de red ---------------------------------------------- */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void pon16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void pon32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
#define IP4(x) (int)(((x) >> 24) & 255), (int)(((x) >> 16) & 255), (int)(((x) >> 8) & 255), (int)((x) & 255)

/* La suma de comprobacion de Internet: sumar de 16 en 16 bits con acarreo
 * circular y complementar. La misma para IP, ICMP y UDP, que es la gracia. */
static uint32_t suma_parcial(const uint8_t *p, int n, uint32_t s)
{
    for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (n & 1) s += (uint32_t)(p[n - 1] << 8);
    return s;
}
static uint16_t suma_fin(uint32_t s)
{
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static const uint8_t TODOS[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static struct message rx, tx;

/* Una trama al driver. Si el driver ya no esta, la tarjeta tampoco. */
static void enviar(int n)
{
    if (!driver) return;
    tx.type = NMSG_ENVIAR;
    tx.len  = (uint64_t)n;
    if (msg_send(driver, &tx) < 0) {
        printf("  [red] el driver se ha ido; me quedo sin tarjeta\n");
        driver = 0; ip = 0;
    }
}

static int eth_cab(uint8_t *f, const uint8_t *dst, uint16_t tipo)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, mi_mac, 6);
    pon16(f + 12, tipo);
    return 14;
}

/* --- ARP: de una IP a la MAC que la tiene ----------------------------------
 *
 * "Quien tiene 192.168.1.1? Que conteste a 192.168.1.144". A todos, porque
 * no se sabe a quien. La respuesta se guarda en una tabla chica, y de paso
 * se apunta a cualquiera que hable en nuestra red: si el router nos pregunta
 * por nosotros, ya sabemos su MAC sin preguntarle. */
struct arp_entrada { uint32_t ip; uint8_t mac[6]; };
static struct arp_entrada arp_tabla[8];
static int arp_sig;

static void arp_aprender(uint32_t quien, const uint8_t *m)
{
    for (int i = 0; i < 8; i++)
        if (arp_tabla[i].ip == quien) { memcpy(arp_tabla[i].mac, m, 6); return; }
    arp_tabla[arp_sig].ip = quien;
    memcpy(arp_tabla[arp_sig].mac, m, 6);
    arp_sig = (arp_sig + 1) % 8;
}

static const uint8_t *arp_buscar(uint32_t quien)
{
    for (int i = 0; i < 8; i++)
        if (arp_tabla[i].ip == quien && quien) return arp_tabla[i].mac;
    return 0;
}

static int arp_construir(uint8_t *f, const uint8_t *dst_mac, uint16_t op,
                         const uint8_t *tmac, uint32_t tip)
{
    int n = eth_cab(f, dst_mac, 0x0806);
    pon16(f + n, 1); pon16(f + n + 2, 0x0800);        /* Ethernet, IPv4 */
    f[n + 4] = 6; f[n + 5] = 4;
    pon16(f + n + 6, op);
    memcpy(f + n + 8, mi_mac, 6);  pon32(f + n + 14, ip);
    memcpy(f + n + 18, tmac, 6); pon32(f + n + 24, tip);
    return n + 28;
}

static void arp_pedir(uint32_t quien)
{
    static const uint8_t nadie[6] = { 0 };
    enviar(arp_construir((uint8_t *)tx.data, TODOS, 1, nadie, quien));
}

/* Una trama IP esperando a que ARP diga a que MAC va. Una sola: si llega
 * otra antes de la respuesta, la primera se pierde, como en cualquier pila
 * pequenya. Los protocolos de arriba reintentan. */
static uint8_t  pendiente[TRAMA_MAX];
static int      pendiente_n;
static uint32_t pendiente_salto;

static void arp_llego(const uint8_t *f, int n)
{
    if (n < 42 || be16(f + 14) != 1 || be16(f + 16) != 0x0800) return;
    uint16_t op = be16(f + 20);
    uint32_t sip = be32(f + 28), tip = be32(f + 38);

    if (sip) arp_aprender(sip, f + 22);

    if (op == 1 && ip && tip == ip) {                 /* preguntan por nosotros */
        enviar(arp_construir((uint8_t *)tx.data, f + 22, 2, f + 22, sip));
        return;
    }
    if (op == 2 && pendiente_n && sip == pendiente_salto) {
        memcpy(pendiente, f + 22, 6);                 /* ya sabemos a quien */
        memcpy(tx.data, pendiente, (size_t)pendiente_n);
        enviar(pendiente_n);
        pendiente_n = 0;
    }
}

/* --- IPv4: de una maquina a otra ------------------------------------------ */
static uint16_t ip_id = 1;

static int ip_construir(uint8_t *f, const uint8_t *dmac, uint32_t dst, uint8_t proto,
                        const uint8_t *carga, int n)
{
    int o = eth_cab(f, dmac, 0x0800);
    uint8_t *h = f + o;
    h[0] = 0x45; h[1] = 0;                            /* v4, 20 bytes, sin TOS */
    pon16(h + 2, (uint16_t)(20 + n));
    pon16(h + 4, ip_id++); pon16(h + 6, 0x4000);      /* no fragmentar */
    h[8] = 64; h[9] = proto; h[10] = h[11] = 0;
    pon32(h + 12, ip); pon32(h + 16, dst);
    pon16(h + 10, suma_fin(suma_parcial(h, 20, 0)));
    memcpy(h + 20, carga, (size_t)n);
    return o + 20 + n;
}

/* A donde va una trama IP: a todos si es broadcast; si no, a la MAC del
 * destino si esta en nuestra red, o a la del router si no. Y si esa MAC no
 * se sabe, se pregunta y la trama espera. */
static void ip_enviar(uint32_t dst, uint8_t proto, const uint8_t *carga, int n)
{
    if (n + 34 > TRAMA_MAX) return;

    if (dst == 0xFFFFFFFF || (ip && mascara && dst == (ip | ~mascara))) {
        enviar(ip_construir((uint8_t *)tx.data, TODOS, dst, proto, carga, n));
        return;
    }
    uint32_t salto = (ip && mascara && (dst & mascara) == (ip & mascara)) ? dst : router;
    if (!salto) { printf("  [red] no se por donde salir hacia %d.%d.%d.%d\n", IP4(dst)); return; }

    const uint8_t *dmac = arp_buscar(salto);
    if (dmac) {
        enviar(ip_construir((uint8_t *)tx.data, dmac, dst, proto, carga, n));
        return;
    }
    pendiente_n = ip_construir(pendiente, TODOS, dst, proto, carga, n);
    pendiente_salto = salto;
    arp_pedir(salto);
}

/* --- UDP: de un programa a otro ------------------------------------------- */
static void udp_enviar(uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *d, int n)
{
    static uint8_t seg[1480];
    if (n + 8 > (int)sizeof(seg)) return;
    pon16(seg, sport); pon16(seg + 2, dport); pon16(seg + 4, (uint16_t)(8 + n)); pon16(seg + 6, 0);
    memcpy(seg + 8, d, (size_t)n);

    /* La suma de UDP cubre tambien una "pseudocabecera" con las dos IP: asi
     * un datagrama que llegue a la maquina equivocada no pasa por bueno. */
    uint8_t pseudo[12];
    pon32(pseudo, ip); pon32(pseudo + 4, dst); pseudo[8] = 0; pseudo[9] = 17; pon16(pseudo + 10, (uint16_t)(8 + n));
    uint16_t c = suma_fin(suma_parcial(seg, 8 + n, suma_parcial(pseudo, 12, 0)));
    pon16(seg + 6, c ? c : 0xFFFF);
    ip_enviar(dst, 17, seg, 8 + n);
}

/* --- ICMP: "estas ahi?" "aqui estoy" ------------------------------------- */
static unsigned pings;

static void icmp_llego(uint32_t src, const uint8_t *d, int n)
{
    static uint8_t r[1480];
    if (n < 8 || n > (int)sizeof(r) || d[0] != 8) return;   /* solo echo request */
    memcpy(r, d, (size_t)n);
    r[0] = 0; r[2] = r[3] = 0;                                /* echo reply */
    pon16(r + 2, suma_fin(suma_parcial(r, n, 0)));
    if (pings++ < 3) printf("  [red] ping de %d.%d.%d.%d (%d bytes): contesto\n", IP4(src), n - 8);
    ip_enviar(src, 1, r, n);
}

static void hora_iniciar(void);
static void hora_tick(void);
static void dns_tick(void);
static void enchufes_barrer(void);

/* --- DHCP: pedir una direccion --------------------------------------------
 *
 * Cuatro mensajes, todos a 255.255.255.255 porque al principio no se sabe
 * ni quien es el servidor:
 *
 *   DISCOVER  "soy esta MAC, alguien me da una direccion?"     (nosotros)
 *   OFFER     "te ofrezco esta"                                 (el servidor)
 *   REQUEST   "vale, quiero esa, la que me ofrecio ese"         (nosotros)
 *   ACK       "es tuya durante tanto tiempo; y de paso: la mascara, el
 *              router, el DNS y, si lo hay, el servidor de hora" (el servidor)
 *
 * Y un reloj: si nadie contesta se vuelve a preguntar, cada vez esperando
 * mas; y cuando pasa la mitad del alquiler se renueva. El formato es el de
 * BOOTP, de 1985, con una "galleta magica" que dice que detras vienen
 * opciones DHCP. Es feo -236 bytes fijos, la mayoria a cero- y es lo que
 * hay: todas las redes del mundo lo hablan. */
static uint32_t xid, ofrecida, servidor;
static uint32_t alquiler, quedan;
static int      estado;             /* 0 sin tarjeta, 1 buscando, 2 pidiendo, 3 con direccion */
static unsigned segundos, siguiente, espera = 2, intentos;

static int dhcp_construir(uint8_t *b, int tipo)
{
    memset(b, 0, 300);
    b[0] = 1; b[1] = 1; b[2] = 6; b[3] = 0;           /* peticion, Ethernet, 6 bytes de MAC */
    pon32(b + 4, xid);
    pon16(b + 10, 0x8000);                            /* contestadme a todos */
    if (estado == 3) pon32(b + 12, ip);               /* renovando: esta es la mia */
    memcpy(b + 28, mi_mac, 6);
    b[236] = 99; b[237] = 130; b[238] = 83; b[239] = 99;

    int o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = (uint8_t)tipo;
    if (tipo == 3 && estado != 3) {
        b[o++] = 50; b[o++] = 4; pon32(b + o, ofrecida); o += 4;
        b[o++] = 54; b[o++] = 4; pon32(b + o, servidor); o += 4;
    }
    b[o++] = 55; b[o++] = 4; b[o++] = 1; b[o++] = 3; b[o++] = 6; b[o++] = 42;
    b[o++] = 12; b[o++] = 6; memcpy(b + o, "tinyos", 6); o += 6;
    b[o++] = 255;
    return o < 300 ? 300 : o;                         /* el minimo de BOOTP */
}

static void dhcp_mandar(int tipo)
{
    static uint8_t b[320];
    int n = dhcp_construir(b, tipo);
    udp_enviar(0xFFFFFFFF, 68, 67, b, n);
}

static void dhcp_buscar(void)
{
    estado = 1; ip = 0;
    xid = (uint32_t)(uptime() ^ ((uint32_t)mi_mac[5] << 24) ^ ((uint32_t)mi_mac[4] << 16));
    espera = 2; intentos = 1;
    siguiente = segundos + espera;
    dhcp_mandar(1);
}

static int bits(uint32_t m) { int b = 0; while (m & 0x80000000) { b++; m <<= 1; } return b; }

static void dhcp_llego(const uint8_t *d, int n)
{
    if (n < 240 || d[0] != 2 || be32(d + 4) != xid || memcmp(d + 28, mi_mac, 6)) return;
    if (d[236] != 99 || d[237] != 130 || d[238] != 83 || d[239] != 99) return;

    int tipo = 0;
    uint32_t msk = 0, rtr = 0, dn = 0, nt = 0, srv = 0, lease = 0;
    for (int o = 240; o + 1 < n && d[o] != 255; ) {
        if (d[o] == 0) { o++; continue; }
        int len = d[o + 1]; const uint8_t *v = d + o + 2;
        if (o + 2 + len > n) break;
        switch (d[o]) {
        case 53: if (len >= 1) tipo = v[0]; break;
        case 1:  if (len >= 4) msk = be32(v); break;
        case 3:  if (len >= 4) rtr = be32(v); break;
        case 6:  if (len >= 4) dn  = be32(v); break;
        case 42: if (len >= 4) nt  = be32(v); break;
        case 51: if (len >= 4) lease = be32(v); break;
        case 54: if (len >= 4) srv = be32(v); break;
        }
        o += 2 + len;
    }
    uint32_t yi = be32(d + 16);
    if (!srv) srv = be32(d + 20);

    if (tipo == 2 && estado == 1) {                   /* OFFER */
        ofrecida = yi; servidor = srv;
        printf("  [red] DHCP: %d.%d.%d.%d me ofrece %d.%d.%d.%d; la pido\n", IP4(srv), IP4(yi));
        estado = 2; intentos = 1; siguiente = segundos + 4;
        dhcp_mandar(3);
        return;
    }
    if (tipo == 5 && (estado == 2 || estado == 3)) { /* ACK */
        int renovaba = (estado == 3);
        ip = yi; mascara = msk ? msk : 0xFFFFFF00; router = rtr; dns = dn; ntp = nt;
        alquiler = lease ? lease : 3600; quedan = alquiler;
        estado = 3;
        if (!renovaba) {
            printf("  [red] DHCP: tengo la %d.%d.%d.%d/%d, router %d.%d.%d.%d, DNS %d.%d.%d.%d, "
                   "NTP %d.%d.%d.%d, alquiler %u s\n",
                   IP4(ip), bits(mascara), IP4(router), IP4(dns), IP4(ntp), (unsigned)alquiler);
            hora_iniciar();                           /* ya se puede preguntar la hora */
        }
        return;
    }
    if (tipo == 6) {                                  /* NAK: desde el principio */
        printf("  [red] DHCP: el servidor dice que no; vuelvo a empezar\n");
        dhcp_buscar();
    }
}

/* Cada segundo: reintentos y renovacion. */
static void cada_segundo(void)
{
    segundos++;
    if (!driver) return;

    if (estado == 1 && segundos >= siguiente) {
        if (intentos <= 4)
            printf("  [red] DHCP: nadie contesta; vuelvo a preguntar (intento %u)\n", intentos + 1);
        intentos++;
        if (espera < 32) espera *= 2;
        siguiente = segundos + espera;
        dhcp_mandar(1);
    } else if (estado == 2 && segundos >= siguiente) {
        if (++intentos > 3) { dhcp_buscar(); return; }
        siguiente = segundos + 4;
        dhcp_mandar(3);
    } else if (estado == 3) {
        if (quedan) quedan--;
        if (quedan == alquiler / 2 || (quedan && quedan < 60 && (quedan % 10) == 0))
            dhcp_mandar(3);                           /* renovar */
        if (!quedan) { printf("  [red] DHCP: se acabo el alquiler\n"); dhcp_buscar(); }
    }
    hora_tick();
    dns_tick();
    enchufes_barrer();
}

/* --- La hora: dos programas encima de UDP ----------------------------------
 *
 * Ya somos una maquina de la red. Lo que David queria era la hora, y la hora
 * son dos preguntas, las dos por UDP:
 *
 *   DNS  "que direccion tiene pool.ntp.org?"  al servidor de nombres que
 *        dio el DHCP, puerto 53. La pregunta es el nombre partido en trozos
 *        con su longitud delante ("4pool3ntp3org0"), y la respuesta repite
 *        la pregunta y anyade registros; el que interesa es el de tipo A,
 *        cuatro bytes. Los nombres pueden venir "comprimidos": un byte que
 *        empieza por 11 es un puntero a otro sitio del mensaje, y hay que
 *        saltarlo sin seguirlo.
 *
 *   NTP  "que hora es?" a esa direccion, puerto 123. 48 bytes casi todos a
 *        cero -el primero dice "version 4, soy cliente"- y vuelven 48 con
 *        cuatro marcas de tiempo. Se usa la de transmision: segundos desde
 *        1900, con 32 bits de fraccion que aqui se tiran. Le sobra precision
 *        a un reloj que FAT guarda de dos en dos segundos.
 *
 * El resultado es UTC. El reloj del kernel cuenta hora LOCAL -es lo que FAT
 * guarda y lo que `ls` ensenya- asi que aqui se suma la zona: ZONA=CET en
 * /etc/rc es Europa central, +1 en invierno y +2 entre el ultimo domingo de
 * marzo y el ultimo domingo de octubre, a la una de la madrugada UTC. Y la
 * pone la pila, no un programa: SYS_settime es de init y de quien esta en
 * PORT_RED, que es por donde llega la hora de verdad. */
#define PUERTO_DNS_LOCAL 5353
#define PUERTO_NTP        123
#define NTP_1970   2208988800UL          /* segundos entre 1900 y 1970 */

static const char *ntp_nombre = "pool.ntp.org";
static long zona_base;                   /* segundos sobre UTC, en invierno */
static int  zona_eu;                     /* con cambio de hora europeo */

static int      hora_estado;             /* 0 nada, 1 resolviendo, 2 preguntando */
static unsigned hora_siguiente, hora_intentos;
static int      hora_auto;               /* volver a intentarlo cuando toque */
static uint32_t ntp_ip, hora_servidor;
static uint16_t dns_id;
static uint64_t hora_puesta;             /* la ultima que se puso; 0 = nunca */
static uint64_t clientes[4];             /* puertos esperando un UMSG_HORA */
static int      n_clientes;
static struct message resp;

static uint64_t a_unix(uint64_t anyo, uint64_t mes, uint64_t dia)
{
    static const uint64_t meses[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    uint64_t dias = 0;
    for (uint64_t a = 1970; a < anyo; a++)
        dias += ((a % 4 == 0 && a % 100 != 0) || a % 400 == 0) ? 366 : 365;
    int bis = (anyo % 4 == 0 && anyo % 100 != 0) || anyo % 400 == 0;
    for (uint64_t k = 1; k < mes; k++) dias += meses[k - 1] + ((k == 2 && bis) ? 1u : 0u);
    return (dias + dia - 1) * 86400;
}

static void formatear(uint64_t t, char *dst)
{
    uint64_t dias = t / 86400, resto = t % 86400, anyo = 1970;
    for (;;) {
        int bis = (anyo % 4 == 0 && anyo % 100 != 0) || anyo % 400 == 0;
        uint64_t largo = bis ? 366 : 365;
        if (dias < largo) break;
        dias -= largo; anyo++;
    }
    static const uint64_t meses[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int bis = (anyo % 4 == 0 && anyo % 100 != 0) || anyo % 400 == 0;
    uint64_t mes = 0;
    for (; mes < 12; mes++) {
        uint64_t largo = meses[mes] + ((mes == 1 && bis) ? 1u : 0u);
        if (dias < largo) break;
        dias -= largo;
    }
    snprintf(dst, 32, "%04lu-%02lu-%02lu %02lu:%02lu:%02lu",
             anyo, mes + 1, dias + 1, resto / 3600, (resto % 3600) / 60, resto % 60);
}

/* Cuanto sumar a UTC para esa fecha. Con ZONA=CET, el cambio de hora: el
 * ultimo domingo de marzo y el de octubre, a la 01:00 UTC. El dia de la
 * semana sale de contar dias desde el 1 de enero de 1970, que fue jueves. */
static long desfase_de(uint64_t utc)
{
    if (!zona_eu) return zona_base;

    uint64_t anyo = 1970, dias = utc / 86400;
    for (;;) {
        uint64_t largo = ((anyo % 4 == 0 && anyo % 100 != 0) || anyo % 400 == 0) ? 366 : 365;
        if (dias < largo) break;
        dias -= largo; anyo++;
    }
    uint64_t m31 = a_unix(anyo, 3, 31), o31 = a_unix(anyo, 10, 31);
    uint64_t inicio = m31 - ((m31 / 86400 + 4) % 7) * 86400 + 3600;
    uint64_t fin    = o31 - ((o31 / 86400 + 4) % 7) * 86400 + 3600;
    return zona_base + ((utc >= inicio && utc < fin) ? 3600 : 0);
}

static void zona_leer(void)
{
    const char *n = getenv("NTP");
    const char *z = getenv("ZONA");
    if (n && *n) ntp_nombre = n;

    if (!z || !*z || (z[0] == 'U' && z[1] == 'T' && z[2] == 'C')) { zona_base = 0; return; }
    if (z[0] == 'C' && z[1] == 'E' && z[2] == 'T') { zona_base = 3600; zona_eu = 1; return; }
    if (z[0] == '+' || z[0] == '-') {
        long h = 0, m = 0; int i = 1;
        while (z[i] >= '0' && z[i] <= '9') h = h * 10 + (z[i++] - '0');
        if (z[i] == ':') { i++; while (z[i] >= '0' && z[i] <= '9') m = m * 10 + (z[i++] - '0'); }
        zona_base = (z[0] == '-' ? -1 : 1) * (h * 3600 + m * 60);
        return;
    }
    printf("  [red] ZONA=%s: no la entiendo, uso UTC\n", z);
}

static void contestar_clientes(int ok, const char *motivo)
{
    for (int i = 0; i < n_clientes; i++) {
        if (ok) {
            struct umsg_hora *h = (struct umsg_hora *)resp.data;
            resp.type = UMSG_HORA_OK; resp.len = sizeof(*h);
            h->segundos = hora_puesta;
            h->desfase  = (unsigned long)desfase_de(hora_puesta - desfase_de(hora_puesta));
            h->servidor = hora_servidor;
        } else {
            resp.type = UMSG_ERROR;
            int k = 0; for (; motivo[k] && k < 200; k++) resp.data[k] = motivo[k];
            resp.data[k] = 0; resp.len = (uint64_t)k + 1;
        }
        msg_send(clientes[i], &resp);
    }
    n_clientes = 0;
}

static void hora_fallo(const char *motivo)
{
    printf("  [red] sin hora: %s; lo intento en un minuto\n", motivo);
    contestar_clientes(0, motivo);
    hora_estado = 0; hora_auto = 1; hora_siguiente = segundos + 60;
}

/* --- DNS: para la hora y para cualquiera --------------------------------------
 *
 * Una tabla chica de consultas en vuelo: quien pregunta (0 = la propia pila,
 * para la hora; si no, el puerto de un programa), por que nombre, con que
 * identificador, y cuantas veces se ha insistido. La respuesta se casa por
 * el identificador, que es lo unico que el servidor devuelve intacto. */
struct consulta { uint16_t id; uint64_t cliente; char nombre[64]; unsigned intentos, siguiente; };
static struct consulta consultas[4];

static void hora_resuelto(uint32_t quien);

static void dns_mandar(struct consulta *c)
{
    static uint8_t q[300];
    pon16(q, c->id); pon16(q + 2, 0x0100);            /* quiero recursion */
    pon16(q + 4, 1); pon16(q + 6, 0); pon16(q + 8, 0); pon16(q + 10, 0);
    int n = 12;
    const char *t = c->nombre;
    while (*t) {
        int l = 0;
        while (t[l] && t[l] != '.') l++;
        if (l > 63 || n + l + 6 > (int)sizeof(q)) break;
        q[n++] = (uint8_t)l;
        memcpy(q + n, t, (size_t)l); n += l; t += l;
        if (*t == '.') t++;
    }
    q[n++] = 0;
    pon16(q + n, 1); pon16(q + n + 2, 1); n += 4;     /* tipo A, clase IN */
    udp_enviar(dns, PUERTO_DNS_LOCAL, 53, q, n);
    c->intentos++;
    c->siguiente = segundos + 2;
}

static void dns_contestar(struct consulta *c, uint32_t quien, const char *motivo)
{
    if (!c->cliente) {                                /* era para la hora */
        if (quien) hora_resuelto(quien); else hora_fallo(motivo);
    } else {
        struct umsg_resolver *r = (struct umsg_resolver *)resp.data;
        if (quien) {
            resp.type = UMSG_RESUELTO; resp.len = sizeof(*r);
            r->port = c->cliente; r->ip = quien;
            memcpy(r->nombre, c->nombre, sizeof(r->nombre));
        } else {
            resp.type = UMSG_ERROR;
            int k = 0; for (; motivo[k] && k < 200; k++) resp.data[k] = motivo[k];
            resp.data[k] = 0; resp.len = (uint64_t)k + 1;
        }
        msg_send(c->cliente, &resp);
    }
    c->id = 0;
}

/* Un numero con puntos no se pregunta: se lee. Devuelve 0 si no lo es. */
static uint32_t ip_de_texto(const char *t)
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

static void dns_consultar(const char *nombre, uint64_t cliente)
{
    struct consulta *c = 0;
    for (int i = 0; i < 4 && !c; i++) if (!consultas[i].id) c = &consultas[i];

    struct consulta tmp = { 0 };
    if (!c) c = &tmp;
    c->id = ++dns_id ? dns_id : ++dns_id;
    c->cliente = cliente;
    int k = 0; for (; nombre[k] && k < 63; k++) c->nombre[k] = nombre[k];
    c->nombre[k] = 0;
    c->intentos = 0;

    uint32_t directa = ip_de_texto(c->nombre);
    if (directa)  { dns_contestar(c, directa, 0); return; }
    if (c == &tmp) { dns_contestar(c, 0, "demasiadas consultas DNS a la vez"); return; }
    if (!dns)     { dns_contestar(c, 0, "el DHCP no me dio servidor de nombres"); return; }
    if (!ip)      { dns_contestar(c, 0, "todavia sin direccion IP (DHCP)"); return; }
    dns_mandar(c);
}

static void dns_tick(void)
{
    for (int i = 0; i < 4; i++) {
        struct consulta *c = &consultas[i];
        if (!c->id || segundos < c->siguiente) continue;
        if (c->intentos >= 3) dns_contestar(c, 0, "el DNS no contesta");
        else dns_mandar(c);
    }
}

static int nombre_saltar(const uint8_t *d, int n, int o)
{
    while (o < n) {
        int l = d[o];
        if (!l) return o + 1;
        if (l & 0xC0) return o + 2;                   /* puntero: dos bytes y fuera */
        o += l + 1;
    }
    return -1;
}

static void dns_llego(const uint8_t *d, int n)
{
    if (n < 12 || !(d[2] & 0x80)) return;
    struct consulta *c = 0;
    for (int i = 0; i < 4 && !c; i++) if (consultas[i].id && consultas[i].id == be16(d)) c = &consultas[i];
    if (!c) return;
    if (d[3] & 0xF) { dns_contestar(c, 0, "el DNS dice que ese nombre no existe"); return; }

    int preguntas = be16(d + 4), respuestas = be16(d + 6), o = 12;
    for (int i = 0; i < preguntas && o > 0; i++) { o = nombre_saltar(d, n, o); if (o > 0) o += 4; }
    for (int i = 0; i < respuestas && o > 0 && o + 10 <= n; i++) {
        o = nombre_saltar(d, n, o);
        if (o < 0 || o + 10 > n) break;
        int tipo = be16(d + o), largo = be16(d + o + 8);
        if (tipo == 1 && largo == 4 && o + 14 <= n) {
            uint32_t quien = be32(d + o + 10);
            if (!c->cliente) printf("  [red] DNS: %s es %d.%d.%d.%d\n", c->nombre, IP4(quien));
            dns_contestar(c, quien, 0);
            return;
        }
        o += 10 + largo;
    }
    dns_contestar(c, 0, "el DNS no trae ninguna direccion");
}

/* --- NTP --- */
static void ntp_pedir(void)
{
    static uint8_t b[48];
    memset(b, 0, 48);
    b[0] = 0x23;                                      /* LI 0, version 4, modo 3: cliente */
    udp_enviar(ntp_ip, PUERTO_NTP, 123, b, 48);
}

static void ntp_llego(uint32_t src, const uint8_t *d, int n)
{
    if (hora_estado != 2 || n < 48 || (d[0] & 7) != 4) return;
    if (d[1] == 0) { hora_fallo("el servidor NTP no esta sincronizado"); return; }
    uint32_t seg = be32(d + 40);
    if (seg < NTP_1970) return;

    uint64_t utc = seg - NTP_1970;
    long desf = desfase_de(utc);
    uint64_t local = utc + (uint64_t)desf;

    if (poner_hora(local) < 0) printf("  [red] el kernel no me deja poner la hora\n");
    hora_puesta = local; hora_servidor = src; hora_estado = 0;
    hora_auto = 1; hora_siguiente = segundos + 3600;   /* y dentro de una hora, otra vez */

    char txt[32];
    formatear(local, txt);
    printf("  [red] hora: %s (NTP de %d.%d.%d.%d, zona %c%ld)\n", txt, IP4(src),
           desf < 0 ? '-' : '+', (desf < 0 ? -desf : desf) / 3600);
    contestar_clientes(1, 0);
}

static void hora_resuelto(uint32_t quien)
{
    if (hora_estado != 1) return;
    ntp_ip = quien;
    hora_estado = 2; hora_intentos = 0; hora_siguiente = segundos + 2;
    ntp_pedir();
}

static void hora_iniciar(void)
{
    if (!ip) return;
    hora_intentos = 0; hora_auto = 0;
    hora_siguiente = segundos + 2;
    if (ntp) { ntp_ip = ntp; hora_estado = 2; ntp_pedir(); }
    else     { hora_estado = 1; dns_consultar(ntp_nombre, 0); }   /* el DNS llama de vuelta */
}

static void hora_tick(void)
{
    if (hora_estado == 2 && segundos >= hora_siguiente) {
        if (++hora_intentos >= 4) { hora_fallo("el servidor NTP no contesta"); return; }
        hora_siguiente = segundos + 2;
        ntp_pedir();
    } else if (!hora_estado && hora_auto && ip && segundos >= hora_siguiente) {
        hora_iniciar();
    }
}

/* Un programa pide la hora: se apunta su puerto y se pregunta, si no se
 * estaba preguntando ya. */
static void hora_pedida(void)
{
    const struct umsg_pedir *p = (const struct umsg_pedir *)rx.data;
    if (n_clientes < 4) clientes[n_clientes++] = p->port;

    if (!driver)      { contestar_clientes(0, "no hay tarjeta de red"); return; }
    if (!ip)          { contestar_clientes(0, "todavia sin direccion IP (DHCP)"); return; }
    if (!hora_estado) hora_iniciar();
}

/* --- Enchufes: la red para cualquier programa ---------------------------------
 *
 * Hasta aqui la pila solo hablaba para si misma: DHCP, DNS y NTP eran suyos.
 * Un enchufe es un puerto UDP que un programa se queda, y a partir de ahi
 * la pila hace de cartero: lo que llega a ese puerto va al programa como
 * mensaje, y lo que el programa manda sale por el con su direccion. La
 * identidad es el pid que el kernel pone en cada mensaje -nadie manda por
 * el enchufe de otro- y la vida la marca su puerto de IPC: si el programa
 * muere, la entrega falla y el enchufe se cierra solo. */
struct enchufe { uint64_t pid, port; uint16_t local; };
static struct enchufe enchufes[8];
static uint16_t efimero = 49152;

static int puerto_reservado(uint16_t p)
{
    return p == 68 || p == PUERTO_DNS_LOCAL || p == PUERTO_NTP;
}

/* Un enchufe cuyo duenyo ha muerto sin cerrarlo -un Ctrl-C- seguiria
 * ocupando su puerto hasta que una entrega fallara. kill(pid, 0) es la
 * pregunta "sigue ahi?", y se hace al abrir uno nuevo y una vez por segundo. */
static void enchufes_barrer(void)
{
    for (int i = 0; i < 8; i++)
        if (enchufes[i].pid && kill(enchufes[i].pid, 0) < 0) enchufes[i].pid = 0;
}

static struct enchufe *enchufe_por_puerto(uint16_t local)
{
    for (int i = 0; i < 8; i++) if (enchufes[i].pid && enchufes[i].local == local) return &enchufes[i];
    return 0;
}

static void error_a(uint64_t port, const char *motivo)
{
    resp.type = UMSG_ERROR;
    int k = 0; for (; motivo[k] && k < 200; k++) resp.data[k] = motivo[k];
    resp.data[k] = 0; resp.len = (uint64_t)k + 1;
    msg_send(port, &resp);
}

static void enchufe_abrir(void)
{
    const struct umsg_abrir *a = (const struct umsg_abrir *)rx.data;
    uint16_t local = (uint16_t)a->local;

    enchufes_barrer();
    if (local && (puerto_reservado(local) || enchufe_por_puerto(local))) {
        error_a(a->port, "ese puerto ya esta cogido"); return;
    }
    if (!local) {
        for (int v = 0; v < 8192; v++) {
            if (++efimero < 49152) efimero = 49152;
            if (!enchufe_por_puerto(efimero)) { local = efimero; break; }
        }
    }
    struct enchufe *e = 0;
    for (int i = 0; i < 8 && !e; i++) if (!enchufes[i].pid) e = &enchufes[i];
    if (!e || !local) { error_a(a->port, "no quedan enchufes"); return; }

    e->pid = rx.from; e->port = a->port; e->local = local;

    struct umsg_abrir *r = (struct umsg_abrir *)resp.data;
    resp.type = UMSG_ABIERTO; resp.len = sizeof(*r);
    r->port = a->port; r->local = local;
    msg_send(a->port, &resp);
}

static void enchufe_cerrar(void)
{
    const struct umsg_abrir *a = (const struct umsg_abrir *)rx.data;
    struct enchufe *e = enchufe_por_puerto((uint16_t)a->local);
    if (e && e->pid == rx.from) e->pid = 0;
}

static void enchufe_enviar(void)
{
    const struct umsg_dgrama *d = (const struct umsg_dgrama *)rx.data;
    struct enchufe *e = enchufe_por_puerto((uint16_t)d->local);
    if (!e || e->pid != rx.from) return;              /* no es tuyo: ni caso */
    if (!ip) { error_a(e->port, "todavia sin direccion IP (DHCP)"); return; }
    int n = (int)d->n;
    if (n < 0 || n > UDP_DATOS_MAX) return;
    udp_enviar((uint32_t)d->ip, e->local, (uint16_t)d->puerto, d->datos, n);
}

/* Ha llegado algo para un enchufe: al programa. */
static void enchufe_entregar(struct enchufe *e, uint32_t src, uint16_t sport, const uint8_t *d, int n)
{
    struct umsg_dgrama *g = (struct umsg_dgrama *)resp.data;
    if (n > UDP_DATOS_MAX) n = UDP_DATOS_MAX;
    resp.type = UMSG_DATAGRAMA;
    resp.len  = sizeof(*g) - UDP_DATOS_MAX + (uint64_t)n;
    g->local = e->local; g->ip = src; g->puerto = sport; g->n = (unsigned long)n;
    memcpy(g->datos, d, (size_t)n);
    if (msg_send(e->port, &resp) < 0) e->pid = 0;    /* se fue: el enchufe, con el */
}

static void resolver_pedido(void)
{
    const struct umsg_resolver *r = (const struct umsg_resolver *)rx.data;
    char nombre[64];
    memcpy(nombre, r->nombre, 63); nombre[63] = 0;
    dns_consultar(nombre, r->port);
}

static void info_pedida(void)
{
    const struct umsg_pedir *p = (const struct umsg_pedir *)rx.data;
    struct umsg_info *i = (struct umsg_info *)resp.data;
    resp.type = UMSG_INFO_OK; resp.len = sizeof(*i);
    i->ip = ip; i->mascara = mascara; i->router = router; i->dns = dns; i->ntp = ntp;
    i->estado = (unsigned long)(driver ? estado : 0);
    memcpy(i->mac, mi_mac, 6);
    memcpy(i->tarjeta, tarjeta, sizeof(i->tarjeta));
    msg_send(p->port, &resp);
}

/* --- Lo que entra --------------------------------------------------------- */
static void udp_llego(uint32_t src, uint16_t sport, uint16_t dport, const uint8_t *d, int n)
{
    if (dport == 68)                                    { dhcp_llego(d, n); return; }
    if (dport == PUERTO_DNS_LOCAL && sport == 53)       { dns_llego(d, n); return; }
    if (dport == PUERTO_NTP && sport == 123)            { ntp_llego(src, d, n); return; }

    /* Los demas puertos son de los programas, si alguno se lo ha quedado. */
    struct enchufe *e = enchufe_por_puerto(dport);
    if (e) enchufe_entregar(e, src, sport, d, n);
}

static void ip_llego(const uint8_t *f, int n)
{
    const uint8_t *h = f + 14;
    if (n < 34 || (h[0] >> 4) != 4) return;
    int ihl = (h[0] & 0xF) * 4;
    int total = be16(h + 2);
    if (ihl < 20 || 14 + ihl > n) return;
    if (total < ihl || 14 + total > n) total = n - 14;    /* tramas rellenadas */
    if (be16(h + 6) & 0x3FFF) return;                     /* fragmentos: no */

    uint32_t src = be32(h + 12), dst = be32(h + 16);
    uint8_t proto = h[9];
    const uint8_t *d = h + ihl;
    int dn = total - ihl;

    /* De paso, aprender la MAC de quien nos habla desde nuestra red. */
    if (ip && mascara && src && (src & mascara) == (ip & mascara)) arp_aprender(src, f + 6);

    int es_nuestro = (ip && dst == ip) || dst == 0xFFFFFFFF ||
                     (ip && mascara && dst == (ip | ~mascara));
    /* Sin direccion todavia, lo unico que puede venir para nosotros es la
     * respuesta del DHCP, y algunos servidores la mandan a la IP que van a
     * darnos en vez de a todos. */
    if (!es_nuestro && !(proto == 17 && dn >= 8 && be16(d + 2) == 68)) return;

    if (proto == 1) { icmp_llego(src, d, dn); return; }
    if (proto == 17 && dn >= 8) {
        int ul = be16(d + 4);
        if (ul < 8 || ul > dn) ul = dn;
        udp_llego(src, be16(d), be16(d + 2), d + 8, ul - 8);
    }
}

static unsigned tramas;

static void trama_llego(const uint8_t *f, int n)
{
    tramas++;
    if (n < 14) return;
    uint16_t tipo = be16(f + 12);
    if (tipo == 0x0806) arp_llego(f, n);
    else if (tipo == 0x0800) ip_llego(f, n);
}

static void tarjeta_llego(void)
{
    const struct net_tarjeta *t = (const struct net_tarjeta *)rx.data;
    driver = t->port;
    memcpy(mi_mac, t->mac, 6);
    memcpy(tarjeta, t->nombre, sizeof(tarjeta) - 1); tarjeta[sizeof(tarjeta) - 1] = 0;
    printf("  [red] tarjeta %s, MAC %02x:%02x:%02x:%02x:%02x:%02x: pido direccion (DHCP)\n",
           t->nombre, mi_mac[0], mi_mac[1], mi_mac[2], mi_mac[3], mi_mac[4], mi_mac[5]);
    dhcp_buscar();
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("\n  [red] pila de red viva en EL0, puerto %d\n", PORT_RED);

    if (port_create(PORT_RED) != PORT_RED) {
        printf("  [red] ya hay una pila de red: me voy\n");
        exit(1);
    }

    /* El reloj, como mensaje: uno por segundo basta para DHCP. */
    if (alarma(PORT_RED, 100) < 0)
        printf("  [red] sin reloj: si nadie contesta al DHCP no podre insistir\n");

    /* La zona horaria y el servidor de hora, de /etc/rc via el entorno. */
    zona_leer();

    for (;;) {
        if (msg_recv(PORT_RED, &rx) < 0) break;

        switch (rx.type) {
        case NMSG_TARJETA: tarjeta_llego(); break;
        case NMSG_TRAMA:   trama_llego((const uint8_t *)rx.data, (int)rx.len); break;
        case CMSG_ALARMA:  cada_segundo(); break;
        case UMSG_HORA:    hora_pedida(); break;
        case UMSG_ABRIR:    enchufe_abrir(); break;
        case UMSG_CERRAR:   enchufe_cerrar(); break;
        case UMSG_ENVIAR:   enchufe_enviar(); break;
        case UMSG_RESOLVER: resolver_pedido(); break;
        case UMSG_INFO:     info_pedida(); break;
        default: break;
        }
    }
    return 0;
}
