/* user/fecha.c - Ver y poner la hora
 *
 * La Pi no tiene reloj de tiempo real: no hay pila, no hay nada que siga
 * contando con la maquina apagada. Al arrancar, el sistema cree que son
 * las de cuando se compilo el kernel, que es lo unico que sabe con
 * certeza sobre el paso del tiempo.
 *
 * Con "fecha AAAA-MM-DD hh:mm" se le dice la de verdad. Y eso solo lo
 * puede hacer init, porque la hora es de la maquina entera: dejar que
 * cualquiera la cambie seria dejar que cualquiera mueva las fechas de
 * todos los ficheros.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

static uint64_t numero(const char *s, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (uint64_t)(s[i] - '0');
    return v;
}

static uint64_t a_unix(uint64_t anyo, uint64_t mes, uint64_t dia,
                       uint64_t h, uint64_t m)
{
    uint64_t dias = 0;
    for (uint64_t a = 1970; a < anyo; a++)
        dias += ((a % 4 == 0 && a % 100 != 0) || a % 400 == 0) ? 366 : 365;

    static const uint64_t meses[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int bis = (anyo % 4 == 0 && anyo % 100 != 0) || anyo % 400 == 0;
    for (uint64_t k = 1; k < mes; k++)
        dias += meses[k - 1] + ((k == 2 && bis) ? 1u : 0u);

    return (dias + dia - 1) * 86400 + h * 3600 + m * 60;
}

int main(int argc, char **argv)
{
    if (argc >= 2) {
        /* "2026-09-23" y, si viene, "07:14" */
        if (argv[1][4] != '-' || argv[1][7] != '-') {
            printf("\n  uso: fecha AAAA-MM-DD [hh:mm]\n");
            return 1;
        }

        uint64_t anyo = numero(argv[1], 4);
        uint64_t mes  = numero(argv[1] + 5, 2);
        uint64_t dia  = numero(argv[1] + 8, 2);
        uint64_t h = 0, m = 0;

        if (argc >= 3 && argv[2][2] == ':') {
            h = numero(argv[2], 2);
            m = numero(argv[2] + 3, 2);
        }

        if (anyo < 1980 || mes < 1 || mes > 12 || dia < 1 || dia > 31) {
            printf("\n  esa fecha no existe\n");
            return 1;
        }

        if (poner_hora(a_unix(anyo, mes, dia, h, m)) < 0) {
            printf("\n  solo init puede cambiar la hora\n");
            return 1;
        }
    }

    uint64_t t = ahora();
    uint64_t dias = t / 86400, resto = t % 86400;

    uint64_t anyo = 1970;
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

    printf("\n  %04lu-%02lu-%02lu %02lu:%02lu:%02lu   (%lu segundos desde 1970)\n",
           anyo, mes + 1, dias + 1,
           resto / 3600, (resto % 3600) / 60, resto % 60, t);
    return 0;
}
