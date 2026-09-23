/* user/echo.c - Repetir lo que le den
 *
 * Diez lineas, y hace falta: sin el no hay forma de ver que "$PATH" se ha
 * convertido en algo antes de llegar aqui. El shell expande, y este
 * programa recibe el resultado sin saber que hubo un dolar.
 */
#include <stdio.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    printf("\n");
    return 0;
}
