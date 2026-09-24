/* user/malo.c - Darle al kernel punteros que no valen
 *
 * La prueba de que el kernel sabe fallar y recuperarse. Cada una de estas
 * llamadas le pasa una direccion que no es del proceso, y el kernel tiene
 * que contestar -1 y seguir vivo.
 *
 * Antes del paso 41 esto no era posible: el kernel comprobaba el puntero
 * a mano y, si se le escapaba alguno, tocaba memoria de usuario desde EL1
 * y se caia entero. Ahora lo comprueba el silicio en cada acceso (ldtr) y,
 * cuando falla, un arreglo devuelve el control al bucle de copia.
 */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n  --- punteros que no valen ---\n");

    /* Una direccion del KERNEL. Si ldtr no usara permisos de EL0, esto
     * leeria memoria privilegiada desde una llamada al sistema. */
    const void *kernel = (const void *)0xFFFFFF8000080000UL;
    printf("  exec con un puntero al kernel : %ld\n",
           (long)exec(kernel, 4096, argv, environ));

    /* Una direccion de usuario sin mapear, dentro del rango permitido. */
    const void *vacio = (const void *)0x0E000000UL;
    printf("  exec con memoria sin mapear   : %ld\n",
           (long)exec(vacio, 4096, argv, environ));

    /* Un cero de toda la vida. */
    printf("  exec con un puntero nulo      : %ld\n",
           (long)exec(0, 4096, argv, environ));

    /* Y lo mismo con spawn, que crea un proceso en vez de sustituirnos. */
    printf("  spawn con memoria sin mapear  : %ld\n",
           (long)spawn(vacio, 4096, argv, environ));

    /* --- Y la frontera nueva del paso 58 -------------------------------
     *
     * Memoria para DMA. Este programa NO es un driver -no tiene ningun
     * periferico concedido- asi que no puede tener una direccion fisica.
     *
     * La regla parece burocratica y no lo es: una direccion fisica es la
     * llave para saltarse la MMU, porque el DMA no pasa por ella. Un
     * proceso que pudiera pedir un tramo y conocer su fisica sabria donde
     * cae la RAM, y con un periferico cualquiera podria escribir en el
     * kernel. Una IOMMU lo ataja en el hardware; la Pi 3 no tiene. */
    uint64_t pa = 0;
    printf("\n  --- y la direccion fisica, que no es para cualquiera ---\n");
    printf("  dma_alloc siendo un programa normal : %ld (%s)\n",
           (long)dma_alloc(4, &pa), strerror(errno));

    printf("\n  --- y sigo vivo para contarlo ---\n");
    return 0;
}
