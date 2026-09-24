/* user/clave.c - Pedir algo sin que se vea, y leer tecla a tecla
 *
 * Este programa no sabe nada de terminales. No hace eco, no entiende un
 * backspace y no ha oido hablar de Ctrl-D. Y aun asi puedes corregir lo
 * que escribes, y aun asi la contrasenya no aparece en pantalla.
 *
 * Esa es toda la idea del paso: lo que hace falta para que eso funcione
 * no esta aqui dentro, y no puede estarlo. Si el eco lo hiciera cada
 * programa, apagarlo exigiria que todos supieran hacerlo, y bastaria uno
 * que no lo supiera para que la contrasenya se viera.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"

/* Lo que de verdad hace falta para pedir una contrasenya: apagar el eco,
 * leer, y DEJARLO COMO ESTABA. Lo ultimo no es cortesia: el terminal es
 * uno y lo comparten todos, asi que un programa que se va con el eco
 * apagado deja el shell escribiendo a ciegas. */
static void sin_eco(char *dst, int max)
{
    int antes = (int)termios(-1);
    termios(antes & ~T_ECO);

    if (!fgets(dst, max, stdin)) dst[0] = 0;

    termios(antes);                  /* pase lo que pase */
    printf("\n");                    /* el Enter tampoco se vio */

    int n = (int)strlen(dst);
    if (n && dst[n - 1] == '\n') dst[n - 1] = 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    char nombre[64], clave[64];

    /* Sin salto de linea al final, a proposito: la pregunta se queda en el
     * cubo hasta que alguien la saque, y quien la saca es la propia
     * lectura. Es lo del paso 49, y aqui se ve para que servia. */
    printf("  nombre: ");

    unsigned long antes = stdio_lecturas;
    if (!fgets(nombre, sizeof(nombre), stdin)) { printf("\n  nada\n"); return 1; }
    unsigned long viajes = stdio_lecturas - antes;

    int n = (int)strlen(nombre);
    if (n && nombre[n - 1] == '\n') nombre[n - 1] = 0;

    /* El numero que resume el paso. Hasta el 52, leer del terminal era un
     * read por CARACTER -el terminal se leia sin cubo, porque con cubo el
     * shell se quedaba con lo que era del programa siguiente-. Ahora una
     * lectura trae una linea y no puede traer mas, asi que el cubo es
     * seguro y la linea entera cuesta un viaje. */
    printf("  (esa linea son %d caracteres y ha costado %lu viaje%s al kernel)\n",
           n, viajes, viajes == 1 ? "" : "s");

    printf("  clave (no se vera): ");
    sin_eco(clave, sizeof(clave));

    printf("  has dicho que eres \"%s\"\n", nombre);
    printf("  y la clave tiene %d caracteres\n", (int)strlen(clave));

    /* Y ahora lo contrario: el modo crudo, que es lo que necesita un
     * editor de pantalla. Cada tecla llega en cuanto se pulsa, sin
     * esperar al Enter y sin que nadie la corrija por ti. Se nota en que
     * el backspace deja de borrar y pasa a ser un numero. */
    printf("\n  ahora en crudo: pulsa teclas, y 'q' para salir\n  ");

    int modo = (int)termios(-1);
    termios(modo & ~T_CANONICO);

    for (;;) {
        int c = getchar();
        if (c < 0 || c == 'q') break;
        printf("[%d] ", c);
        if (c == '\r' || c == '\n') printf("\n  ");
    }

    termios(modo);
    printf("\n  y vuelta a lo normal\n");
    return 0;
}
