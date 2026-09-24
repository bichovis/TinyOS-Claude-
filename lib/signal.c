/* user/signal.c - El lado de usuario de las senyales
 *
 * Aqui solo hay dos cosas, y la interesante es la segunda.
 *
 * El TRAMPOLIN es por donde vuelve un manejador. El kernel no puede
 * inventarse codigo en el espacio del proceso -la pila no es ejecutable, y
 * mejor que siga sin serlo-, asi que la direccion de retorno tiene que
 * apuntar a algo que ya exista en el programa. Esto es ese algo: una
 * funcion minuscula que no hace mas que decirle al kernel "ya he
 * terminado, devuelveme donde estaba".
 *
 * El programa nunca la llama ni la ve. Se la pasa al kernel al registrar
 * el primer manejador, y el kernel la deja en x30 cada vez que entrega una
 * senyal.
 */
#include "syscall.h"

static void trampolin(void)
{
    /* No vuelve: sigreturn restaura el contexto entero, incluido el
     * puntero de pila, asi que lo que haga el prologo de esta funcion da
     * exactamente igual. */
    syscall2(SYS_sigreturn, 0, 0);
    __builtin_unreachable();
}

int signal_banderas(int sig, void (*manejador)(int), int banderas)
{
    return (int)syscall4(SYS_signal, (uint64_t)sig, (uint64_t)manejador,
                         (uint64_t)trampolin, (uint64_t)banderas);
}

/* signal() a secas no reanuda nada, que es la semantica de System V y la
 * que hace falta para Ctrl-C: ahi romper la lectura ES lo que se busca.
 * Quien quiera lo otro tiene que decirlo, y eso es exactamente la
 * diferencia entre signal() y sigaction() en un Unix de verdad. */
int signal(int sig, void (*manejador)(int))
{
    return signal_banderas(sig, manejador, 0);
}
