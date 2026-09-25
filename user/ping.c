/* user/ping.c - "Estas ahi?"
 *
 *     ping DESTINO [veces]        4 por defecto; 0 = hasta que lo pares
 *
 * El programa de red mas antiguo que sigue en uso, y el que mas dice con
 * menos: si contesta, hay cable, hay direccion, hay camino de ida Y de
 * vuelta, y el otro esta vivo. Si no contesta, ninguna de esas cosas se
 * puede dar por buena.
 *
 * Aqui no hay ICMP: ICMP no tiene puertos y no cabe en un enchufe, asi que
 * lo habla la pila (ver red.c) y este programa se lo pide con ping(), de
 * lib/red.h. Lo que si es suyo es lo de siempre en un ping: contar, medir,
 * y el resumen al final -tambien si lo cortas con Ctrl-C, que es cuando el
 * resumen mas se agradece-.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "syscall.h"
#include "net_abi.h"
#include "red.h"

/* Lo unico que hace el manejador es levantar una bandera: printf no es
 * reentrante y un manejador no puede llamarlo. Lo que ademas hace la senyal,
 * y es lo que hace falta aqui, es INTERRUMPIR la espera: el msg_recv de
 * dentro de ping() vuelve con error y el bucle llega a mirar la bandera. */
static volatile int parar;
static void al_interrumpir(int sig) { (void)sig; parar = 1; }

static int numero(const char *s)
{
    int v = 0;
    if (!*s) return -1;
    for (; *s; s++) { if (*s < '0' || *s > '9') return -1; v = v * 10 + (*s - '0'); }
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("\n  uso: ping DESTINO [veces]   (0 veces = hasta que lo pares)\n");
        return 1;
    }

    int veces = 4;
    if (argc == 3) {
        veces = numero(argv[2]);
        if (veces < 0) { printf("\n  '%s' no es un numero de veces\n", argv[2]); return 1; }
    }

    uint32_t ip;
    if (resolver(argv[1], &ip) < 0) {
        printf("\n  no se quien es %s\n", argv[1]);
        return 1;
    }

    char destino[16];
    ip_texto(ip, destino);
    if (strcmp(destino, argv[1])) printf("\n  PING %s (%s): 56 bytes de datos\n", argv[1], destino);
    else                          printf("\n  PING %s: 56 bytes de datos\n", destino);

    signal(SIGINT, al_interrumpir);

    int mandados = 0, recibidos = 0;
    int minimo = -1, maximo = 0;
    unsigned suma = 0;

    for (int i = 0; (!veces || i < veces) && !parar; i++) {
        uint64_t t0 = uptime();
        int ms = 0, ttl = 0;

        mandados++;
        int r = ping(ip, i, 56, &ms, &ttl, 10);          /* un segundo de espera */

        if (r == 1) {
            recibidos++;
            suma += (unsigned)ms;
            if (minimo < 0 || ms < minimo) minimo = ms;
            if (ms > maximo) maximo = ms;
            printf("  64 bytes de %s: seq=%d ttl=%d tiempo=%d ms\n", destino, i, ttl, ms);
        } else if (r == 0) {
            if (!parar) printf("  seq=%d: sin respuesta\n", i);
        } else {
            if (!parar) printf("  seq=%d: %s\n", i, strerror(errno));
        }

        /* Uno por segundo, como manda la tradicion: se descuenta lo que ya
         * ha tardado la respuesta. */
        if ((!veces || i + 1 < veces) && !parar) {
            uint64_t pasado = uptime() - t0;
            if (pasado < 1000) sleep((1000 - pasado) / 10);
        }
    }

    printf("\n  --- %s ---\n", destino);
    printf("  %d mandados, %d recibidos, %d%% perdidos", mandados, recibidos,
           mandados ? (mandados - recibidos) * 100 / mandados : 0);
    if (recibidos)
        printf("; viaje min/medio/max = %d/%u/%d ms", minimo, suma / (unsigned)recibidos, maximo);
    printf("\n");

    return recibidos ? 0 : 1;
}
