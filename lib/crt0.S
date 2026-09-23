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

    /* El kernel deja argc en x0, argv en x1 y el ENTORNO en x2, que es
     * donde el convenio espera el tercer argumento de main.
     *
     * Pero casi nadie escribe main(argc, argv, envp): lo normal es
     * pedirle una variable suelta a getenv(), y para eso hace falta que
     * el entorno este en algun sitio fijo. De ahi 'environ', que es una
     * variable global y no un parametro, y que en todos los Unix se
     * rellena exactamente aqui: en el trozo que corre antes de main. */
    adrp    x9, environ
    add     x9, x9, :lo12:environ
    str     x2, [x9]

    bl      main                /* argc, argv y envp ya estan en su sitio */

    /* main devolvio en w0, que es justo donde exit() quiere su argumento.
     * No hay nada que mover. */
    bl      exit

    /* exit no vuelve. Si volviera, que no siga adelante leyendo lo que
     * haya detras. */
1:  b       1b

.size _start, . - _start
