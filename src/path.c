/* path.c - Juntar el directorio actual con una ruta relativa
 *
 * POR QUE ESTO ESTA EN EL KERNEL. El servidor de ficheros solo entiende
 * rutas absolutas, y es a proposito: no tiene estado, y un "directorio
 * actual" es estado de cada proceso. Quien lo guarda es el kernel, que ya
 * guarda todo lo demas que es de un proceso y no de nadie mas.
 *
 * Asi que la union hay que hacerla aqui. Y hacerla UNA vez: el kernel la
 * necesita para la redireccion (">" sobre una ruta relativa) y los
 * programas la necesitan para hablar con el servidor. Si hubiera dos
 * implementaciones, una en el kernel y otra en la libc, acabarian
 * discrepando en algun caso raro -"/a/../.." o "a//b"- y el sintoma seria
 * que el shell y el programa no ven el mismo fichero. De ahi
 * SYS_realpath: los programas usan la del kernel.
 *
 * Lo que hace es puramente TEXTUAL. No toca el disco, no comprueba que
 * nada exista: "." se tira, ".." quita la componente anterior, y las
 * barras de mas se ignoran. Que la ruta resultante nombre algo o no es
 * asunto de quien la use despues.
 *
 * Ese "puramente textual" tiene una consecuencia que conviene saber: en un
 * sistema con enlaces simbolicos NO seria correcto, porque "/a/b/.." no
 * tiene por que ser "/a" si b es un enlace. FAT16 no los tiene, asi que
 * aqui no hay mentira; en otro sitio la habria.
 */
#include <stdint.h>
#include "file.h"

static void quitar_ultima(char *out, uint64_t *n)
{
    while (*n > 1 && out[*n - 1] != '/') (*n)--;
    if (*n > 1) (*n)--;              /* y la barra, salvo la del raiz */
    out[*n] = 0;
}

static int anyadir(char *out, uint64_t cap, uint64_t *n,
                   const char *comp, uint64_t len)
{
    if (*n > 1) {                    /* no duplicar la barra del raiz */
        if (*n + 1 >= cap) return -1;
        out[(*n)++] = '/';
    }
    if (*n + len >= cap) return -1;
    for (uint64_t i = 0; i < len; i++) out[(*n)++] = comp[i];
    out[*n] = 0;
    return 0;
}

int path_resolve(const char *base, const char *rel, char *out, uint64_t cap)
{
    if (cap < 2) return -1;

    uint64_t n = 0;

    if (rel[0] == '/') {
        out[n++] = '/';              /* absoluta: el base no pinta nada */
    } else {
        /* Relativa: se parte del directorio actual, que ya esta
         * normalizado porque salio de aqui mismo la vez anterior. */
        while (base[n] && n + 1 < cap) { out[n] = base[n]; n++; }
        if (n == 0) out[n++] = '/';
    }
    out[n] = 0;

    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *ini = p;
        while (*p && *p != '/') p++;
        uint64_t len = (uint64_t)(p - ini);

        if (len == 1 && ini[0] == '.')
            continue;                            /* "." no lleva a ninguna parte */

        if (len == 2 && ini[0] == '.' && ini[1] == '.') {
            quitar_ultima(out, &n);              /* ".." en el raiz es el raiz */
            continue;
        }

        if (anyadir(out, cap, &n, ini, len) < 0) return -1;
    }

    if (n == 0) { out[0] = '/'; out[1] = 0; }
    return 0;
}
