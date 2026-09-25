/* user/hora.c - Pedirle la hora a la red
 *
 * Este programa no habla NTP: le pide a la pila de red (PORT_RED) que lo
 * haga y que ponga el reloj. Es el mismo reparto que con los ficheros: el
 * que sabe del protocolo y tiene permiso para tocar el reloj es el servidor;
 * el programa solo pregunta y ensenya la respuesta.
 *
 *     hora        sincroniza con NTP y dice la hora
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"
#include "net_abi.h"

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

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int64_t puerto = port_create(-1);
    if (puerto < 0) { printf("\n  no tengo puerto\n"); return 1; }

    struct message m;
    struct umsg_pedir *p = (struct umsg_pedir *)m.data;
    m.type = UMSG_HORA; m.len = sizeof(*p);
    p->port = (unsigned long)puerto;

    if (msg_send(PORT_RED, &m) < 0) {
        printf("\n  no hay pila de red\n");
        return 1;
    }

    /* Esperar la respuesta, pero no para siempre: un segundo de reloj para
     * contar hasta veinte. Si la red no contesta, se dice y se sale. */
    alarma((uint64_t)puerto, 100);
    int segundos = 0;
    for (;;) {
        if (msg_recv((uint64_t)puerto, &m) < 0) { printf("\n  se fue mi puerto\n"); return 1; }
        if (m.type == CMSG_ALARMA) {
            if (++segundos >= 20) { printf("\n  la red no contesta\n"); return 1; }
            continue;
        }
        if (m.type == UMSG_ERROR) {
            m.data[MSG_DATA_MAX - 1] = 0;
            printf("\n  sin hora: %s\n", m.data);
            return 1;
        }
        if (m.type == UMSG_HORA_OK) break;
    }

    const struct umsg_hora *h = (const struct umsg_hora *)m.data;
    char texto[32];
    formatear(h->segundos, texto);
    long d = (long)h->desfase;
    printf("\n  %s   (zona %c%ld, NTP de %lu.%lu.%lu.%lu)\n", texto,
           d < 0 ? '-' : '+', (d < 0 ? -d : d) / 3600,
           (h->servidor >> 24) & 255, (h->servidor >> 16) & 255,
           (h->servidor >> 8) & 255, h->servidor & 255);
    return 0;
}
