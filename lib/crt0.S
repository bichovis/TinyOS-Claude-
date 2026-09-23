/* crt0.S - Lo que se ejecuta antes de main()
 *
 * Todo programa en C empieza en main(), y esa frase esconde una mentira
 * util: main no es el principio de nada. El principio es _start, y hasta
 * ahora cada programa de TinyOS tenia el suyo, con la firma rara
 * `void _start(int argc, char **argv)` y un exit() a mano al final.
 *
 * Esto es ese trozo, escrito una sola vez y en el sitio que le toca. El
 * kernel entra por el punto de entrada del ELF con argc en x0 y argv en
 * x1, que es exactamente donde el convenio de llamada de AArch64 espera
 * los dos primeros argumentos. Asi que no hay nada que colocar: basta con
 * saltar a main.
 *
 * Lo que si hay que hacer es lo de despues. main devuelve un int en w0, y
 * ese numero tiene que convertirse en la salida del proceso. Sin este
 * fichero, un main que termina con `return 0` se iria por el final de la
 * funcion y volveria a una direccion basura.
 */
.section .text.start
.global _start
.type   _start, %function

_start:
    /* Cortar la cadena de marcos de pila. Quien vaya siguiendo x29 hacia
     * atras para reconstruir la pila tiene que parar aqui: no hay nadie
     * mas abajo, este es el fondo del proceso. */
    mov     x29, xzr
    mov     x30, xzr

    bl      main                /* argc y argv ya estan en x0 y x1 */

    /* main devolvio en w0, que es justo donde exit() quiere su argumento.
     * No hay nada que mover. */
    bl      exit

    /* exit no vuelve. Si volviera, que no siga adelante leyendo lo que
     * haya detras. */
1:  b       1b

.size _start, . - _start
