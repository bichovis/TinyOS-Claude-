/* user/deep.c - Comerse la pila a proposito
 *
 * Cada nivel de recursion se lleva medio KB. Con la pila fija de una
 * pagina que habia hasta ahora, esto moria en el septimo nivel y sin
 * avisar: el proceso escribia por debajo de su pila, encima de lo que
 * hubiera, y el desastre aparecia mucho despues y en otro sitio.
 *
 * Ahora cada vez que baja de una pagina salta un fallo de traduccion, el
 * kernel ve que cae justo debajo de la pila, le da otra y reintenta la
 * instruccion. El proceso no se entera de nada: para el, la pila
 * simplemente es lo bastante grande.
 */
#include "syscall.h"

static uint64_t sp_ahora(void)
{
    uint64_t v;
    __asm__ volatile("mov %0, sp" : "=r"(v));
    return v;
}

static void num(const char *antes, uint64_t v, const char *despues)
{
    char b[24];
    uint64_t n = udec(b, v);
    b[n] = 0;
    kprint(antes); kprint(b); kprint(despues);
}

static uint64_t hondo;

/* El 'noinline' y el usar 'relleno' DESPUES de la llamada no son manias.
 *
 * En el primer intento esto no gastaba pila: escribia el buffer, llamaba,
 * y no volvia a mirarlo. Al compilador le basto con eso para ver que el
 * buffer estaba muerto durante la llamada, reaprovechar el mismo sitio en
 * todos los niveles y convertir la recursion en un bucle. Cuarenta
 * niveles, medio kilobyte de pila gastado en total.
 *
 * Para que un nivel tenga que conservar su marco, el marco tiene que
 * seguir haciendo falta cuando vuelve la llamada. */
__attribute__((noinline))
static uint64_t bajar(uint64_t n)
{
    volatile char relleno[512];      /* medio KB por nivel */
    relleno[0]   = (char)n;
    relleno[511] = (char)(n + 1);

    uint64_t sp = sp_ahora();
    if (sp < hondo) hondo = sp;

    if (n == 0) return (uint64_t)relleno[0];

    uint64_t r = bajar(n - 1);
    return r + (uint64_t)relleno[511];   /* vivo despues de la llamada */
}

void _start(int argc, char **argv) __attribute__((section(".text.start")));

void _start(int argc, char **argv)
{
    uint64_t niveles = 200;

    /* "deep 50" para bajar menos */
    if (argc > 1) {
        niveles = 0;
        for (const char *s = argv[1]; *s >= '0' && *s <= '9'; s++)
            niveles = niveles * 10 + (uint64_t)(*s - '0');
        if (!niveles) niveles = 200;
    }

    uint64_t arriba = sp_ahora();
    hondo = arriba;

    num("\n  pila al empezar : ", arriba, "\n");
    num("  bajando ", niveles, " niveles, medio KB cada uno...\n");

    bajar(niveles);

    num("  pila mas abajo  : ", hondo, "\n");
    num("  se ha comido    : ", (arriba - hondo) / 1024, " KB\n");
    kprint("  y he vuelto entero\n");

    exit(0);
}
