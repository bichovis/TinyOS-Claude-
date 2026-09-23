/* setjmp.h - Saltar de vuelta a donde estabas
 *
 * Es lo mas raro que hay en una libc: una funcion que devuelve DOS veces.
 * La primera cuando la llamas, con cero; la segunda cuando alguien hace
 * longjmp, con lo que le pasaran.
 *
 * No es magia. setjmp se guarda los registros que el convenio obliga a
 * conservar entre llamadas -incluido el puntero de pila y la direccion de
 * retorno- y longjmp los vuelve a poner. Al restaurar x30 y sp, el "ret"
 * de longjmp aterriza dentro de setjmp, y setjmp vuelve por segunda vez.
 *
 * Y por eso NO se puede escribir en C: en C no hay forma de decir "el
 * puntero de pila vale esto otro".
 *
 * La trampa de usarlo: despues de un longjmp, las variables locales de la
 * funcion que llamo a setjmp valen lo que valieran cuando el compilador
 * las dejo por ultima vez en memoria. Las que vivan en registros vuelven
 * atras. Por eso se marcan 'volatile' las que importen.
 */
#pragma once
#include <stdint.h>

/* x19-x28, x29, x30, sp, y d8-d15: lo que el ABI de AArch64 obliga a
 * conservar. 22 palabras de 64 bits. */
typedef uint64_t jmp_buf[22];

int  setjmp(jmp_buf destino);
void longjmp(jmp_buf destino, int valor) __attribute__((noreturn));
