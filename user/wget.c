/* user/wget.c - Traerse una pagina de la web
 *
 *     wget http://sitio/ruta [fichero]
 *     wget sitio[:puerto][/ruta] [fichero]
 *
 * Sin fichero, saca el contenido por la pantalla; con fichero, lo guarda.
 *
 * HTTP es de las pocas cosas de este proyecto que se puede escribir de
 * memoria sin miedo, porque es texto: una linea con el metodo y la ruta, unas
 * cuantas cabeceras, una linea vacia, y lo que venga detras es la respuesta.
 * Eso es todo lo que hace este programa. Lo dificil -que los bytes lleguen
 * todos, en orden y una sola vez- lo hace TCP, y TCP esta en la pila (red.c).
 *
 * De HTTPS no hay nada: cifrar pide criptografia que no existe aqui, asi que
 * solo se pueden traer paginas que se sirvan sin cifrar.
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
    printf("\n  uso: wget http://sitio/ruta [fichero]\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) return uso();

    /* --- Partir la direccion: sitio, puerto y ruta ------------------- */
    const char *u = argv[1];
    if (!strncmp(u, "http://", 7)) u += 7;
    else if (!strncmp(u, "https://", 8)) {
        printf("\n  https pide cifrado, y aqui no hay: prueba con http://\n");
        return 1;
    }

    char sitio[128], ruta[256];
    int puerto = 80, n = 0;
    while (u[n] && u[n] != '/' && u[n] != ':' && n < (int)sizeof(sitio) - 1) { sitio[n] = u[n]; n++; }
    sitio[n] = 0;
    if (!n) return uso();

    if (u[n] == ':') {
        n++;
        puerto = 0;
        while (u[n] >= '0' && u[n] <= '9') puerto = puerto * 10 + (u[n++] - '0');
        if (puerto <= 0 || puerto > 65535) return uso();
    }
    if (u[n] == '/') { int k = 0; while (u[n] && k < (int)sizeof(ruta) - 1) ruta[k++] = u[n++]; ruta[k] = 0; }
    else             { ruta[0] = '/'; ruta[1] = 0; }

    uint32_t ip;
    if (resolver(sitio, &ip) < 0) { printf("\n  no se quien es %s\n", sitio); return 1; }

    char texto[16];
    ip_texto(ip, texto);
    printf("\n  %s es %s; conectando al puerto %d...\n", sitio, texto, puerto);

    uint64_t t0 = uptime();
    int c = tcp_conectar(ip, puerto, 100);          /* diez segundos */
    if (c < 0) { printf("  no se puede conectar: %s\n", red_motivo()); return 1; }
    printf("  conectado\n");

    /* --- La peticion -------------------------------------------------
     *
     * 'Host' es obligatorio desde HTTP/1.1 y es lo que permite que muchos
     * sitios vivan en la misma direccion. 'Connection: close' es lo que le
     * pide al servidor que cierre al terminar: asi el final de la respuesta
     * es el final de la conexion y no hay que entender el troceado de
     * HTTP/1.1 para saber donde acaba. */
    char pet[512];
    int pn = snprintf(pet, sizeof(pet),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: TinyOS/1.0\r\n"
                      "Accept: */*\r\n"
                      "Connection: close\r\n"
                      "\r\n", ruta, sitio);

    if (tcp_enviar_todo(c, pet, pn, 100) != pn) {
        printf("  no he podido mandar la peticion: %s\n", red_motivo());
        tcp_cerrar(c);
        return 1;
    }

    /* --- La respuesta ------------------------------------------------
     *
     * Primero las cabeceras, hasta la linea vacia. Se van leyendo trozos y
     * se busca el "\r\n\r\n", que puede caer partido entre dos trozos: por
     * eso el estado -cuantos bytes del corte se llevan vistos- esta fuera
     * del bucle y no dentro. */
    int64_t fd = -1;
    if (argc == 3) {
        fd = openf(argv[2], O_ESCRIBIR);
        if (fd < 0) { printf("  no puedo escribir %s\n", argv[2]); tcp_cerrar(c); return 1; }
    }

    uint8_t buf[TCP_DATOS_MAX];
    int en_cuerpo = 0, cuerpo = 0, primera = 1, en_linea = 0;
    char estado[64]; int en = 0;

    for (;;) {
        int r = tcp_recibir(c, buf, sizeof(buf), 300);      /* 30 s */
        if (r < 0) { printf("\n  se corto la descarga: %s\n", red_motivo()); break; }
        if (r == 0) break;                                  /* el otro cerro: fin */

        /* Las cabeceras acaban en una LINEA VACIA. Se cuenta lo que lleva la
         * linea actual y se ignoran los retornos de carro: asi vale tanto el
         * "\r\n" de la norma como el "\n" a secas que usan algunos
         * servidores y casi todos los proxys. Ser estricto al mandar y
         * tolerante al recibir es una regla vieja de internet, y aqui se gana
         * un fichero descargado en vez de una pagina que parece vacia. */
        int i = 0;
        while (i < r && !en_cuerpo) {
            char ch = (char)buf[i++];
            if (ch == '\n') {
                if (!en_linea) en_cuerpo = 1;          /* linea vacia: se acabo */
                else { primera = 0; en_linea = 0; }
            } else if (ch != '\r') {
                if (primera && en < (int)sizeof(estado) - 1) estado[en++] = ch;
                en_linea++;
            }
        }
        if (!primera && en) { estado[en] = 0; printf("  %s\n", estado); en = 0; }

        if (en_cuerpo && i < r) {
            int m = r - i;
            cuerpo += m;
            if (fd >= 0) write((uint64_t)fd, buf + i, (uint64_t)m);
            else         write(1, buf + i, (uint64_t)m);
        }
    }

    tcp_cerrar(c);

    /* Y la velocidad, que es lo que dice un wget de verdad y aqui ademas
     * ensenya donde se va el tiempo: traer los bytes y guardarlos no cuestan
     * lo mismo ni de lejos. */
    uint64_t ms = uptime() - t0;
    if (!ms) ms = 1;
    if (fd >= 0) {
        closefd((int)fd);
        printf("\n  %d bytes en %s", cuerpo, argv[2]);
    } else {
        printf("\n  --- %d bytes", cuerpo);
    }
    uint64_t bs = (uint64_t)cuerpo * 1000 / ms;
    printf("  (%lu.%lu s, ", ms / 1000, (ms % 1000) / 100);
    if (bs >= 1024) printf("%lu KB/s)\n", bs / 1024);
    else            printf("%lu B/s)\n", bs);
    return cuerpo ? 0 : 1;
}
