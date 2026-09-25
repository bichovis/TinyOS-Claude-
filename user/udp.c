/* user/udp.c - Hablar con la red desde la linea de ordenes
 *
 *     udp                          que direccion tengo, y lo demas
 *     udp resuelve NOMBRE          que direccion tiene ese nombre
 *     udp manda DESTINO PUERTO texto...   mandar un datagrama y esperar respuesta
 *     udp escucha PUERTO           recibir lo que llegue, ensenyarlo y devolverlo
 *
 * No hay nada de red aqui dentro: todo es lib/red.h, que a su vez son
 * mensajes a la pila. Es el programa que demuestra que la red ya es de
 * cualquiera, y la herramienta con la que probarla desde fuera.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "syscall.h"
#include "net_abi.h"
#include "red.h"

static int uso(void)
{
    printf("\n  uso: udp\n"
           "       udp resuelve NOMBRE\n"
           "       udp manda DESTINO PUERTO texto...\n"
           "       udp escucha PUERTO\n");
    return 1;
}

static int numero(const char *s)
{
    int v = 0;
    if (!*s) return -1;
    for (; *s; s++) { if (*s < '0' || *s > '9') return -1; v = v * 10 + (*s - '0'); if (v > 65535) return -1; }
    return v;
}

static void ensenyar(const uint8_t *d, int n)
{
    printf("\"");
    for (int i = 0; i < n; i++) {
        unsigned char c = d[i];
        if (c == '\n') printf("\\n");
        else if (c >= 32 && c < 127) printf("%c", c);
        else printf("\\x%02x", c);
    }
    printf("\"");
}

static int info(void)
{
    struct red_estado e;
    if (red_estado(&e) < 0) { printf("\n  la pila de red no contesta\n"); return 1; }

    char a[16], b[16], c[16], d[16];
    static const char *estados[] = { "sin tarjeta", "buscando direccion (DHCP)",
                                     "pidiendo direccion (DHCP)", "con direccion" };
    printf("\n  tarjeta  %s  %02x:%02x:%02x:%02x:%02x:%02x\n", e.tarjeta,
           e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5]);
    printf("  estado   %s\n", estados[e.estado & 3]);
    if (e.ip) {
        ip_texto(e.ip, a); ip_texto(e.mascara, b); ip_texto(e.router, c); ip_texto(e.dns, d);
        printf("  ip       %s  mascara %s\n  router   %s\n  dns      %s\n", a, b, c, d);
        if (e.ntp) { ip_texto(e.ntp, a); printf("  ntp      %s (del DHCP)\n", a); }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return info();

    if (!strcmp(argv[1], "resuelve") && argc == 3) {
        uint32_t ip;
        if (resolver(argv[2], &ip) < 0) { printf("\n  no puedo resolver %s\n", argv[2]); return 1; }
        char t[16]; ip_texto(ip, t);
        printf("\n  %s es %s\n", argv[2], t);
        return 0;
    }

    if (!strcmp(argv[1], "manda") && argc >= 5) {
        uint32_t ip; int puerto = numero(argv[3]);
        if (puerto <= 0) return uso();
        if (resolver(argv[2], &ip) < 0) { printf("\n  no puedo resolver %s\n", argv[2]); return 1; }

        char texto[UDP_DATOS_MAX]; int n = 0;
        for (int i = 4; i < argc; i++) {
            int l = (int)strlen(argv[i]);
            if (n + l + 1 >= (int)sizeof(texto)) break;
            if (i > 4) texto[n++] = ' ';
            memcpy(texto + n, argv[i], (size_t)l); n += l;
        }

        int s = udp_abrir(0);
        if (s < 0) { printf("\n  no puedo abrir un enchufe\n"); return 1; }
        char t[16]; ip_texto(ip, t);
        printf("\n  %d bytes a %s:%d desde el puerto %d\n", n, t, puerto, udp_puerto(s));
        udp_enviar(s, ip, puerto, texto, n);

        uint8_t r[UDP_DATOS_MAX]; uint32_t de; int dp;
        int rn = udp_recibir(s, &de, &dp, r, sizeof(r), 30);       /* 3 segundos */
        if (rn > 0) { ip_texto(de, t); printf("  respuesta de %s:%d, %d bytes: ", t, dp, rn); ensenyar(r, rn); printf("\n"); }
        else if (rn == 0) printf("  nadie contesta\n");
        else printf("  fallo al recibir\n");
        udp_cerrar(s);
        return rn > 0 ? 0 : 1;
    }

    if (!strcmp(argv[1], "escucha") && argc == 3) {
        int puerto = numero(argv[2]);
        if (puerto <= 0) return uso();
        int s = udp_abrir(puerto);
        if (s < 0) { printf("\n  no puedo quedarme el puerto %d (%s)\n", puerto, strerror(errno)); return 1; }
        printf("\n  escuchando en el puerto UDP %d; lo que llegue se ensenya y se devuelve (Ctrl-C para salir)\n", puerto);

        for (;;) {
            uint8_t d[UDP_DATOS_MAX]; uint32_t de; int dp;
            int n = udp_recibir(s, &de, &dp, d, sizeof(d), RED_SIEMPRE);
            if (n < 0) { printf("  fallo al recibir\n"); break; }
            char t[16]; ip_texto(de, t);
            printf("  de %s:%d, %d bytes: ", t, dp, n); ensenyar(d, n); printf("\n");
            udp_enviar(s, de, dp, d, n);
        }
        udp_cerrar(s);
        return 0;
    }

    return uso();
}
