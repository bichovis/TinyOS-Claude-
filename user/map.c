/* user/map.c - Un fichero convertido en un puntero
 *
 * Lo que hay que ver aqui no es que funcione: es CUANDO se lee el fichero.
 *
 * mmap no lee nada. Devuelve una direccion y se va. El fichero llega
 * pagina a pagina, segun se toca, y cada una de esas llegadas es un fallo
 * de pagina que el kernel resuelve preguntandole al servidor de ficheros,
 * que es un proceso de EL0 como este. Por eso el programa mide las paginas
 * libres del sistema antes y despues: el gasto aparece a medida que se
 * recorre el fichero, no al mapearlo.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("\n  uso: map FICHERO      (map -w FICHERO intenta escribir)\n");
        exit(1);
    }

    int probar_escritura = 0;
    if (argv[1][0] == '-' && argv[1][1] == 'w') {
        if (argc < 3) { printf("\n  uso: map -w FICHERO\n"); exit(1); }
        probar_escritura = 1;
        argv++;
    }

    uint64_t antes = freepages();

    uint64_t tam = 0;
    const char *p = mmap(argv[1], &tam);
    if (!p) {
        printf("  no he podido mapear %s\n", argv[1]);
        exit(1);
    }

    uint64_t despues_de_mapear = freepages();

    printf("\n  %s mapeado en 0x%lx, %lu bytes\n", argv[1], (uint64_t)p, tam);
    printf("  paginas libres: %lu antes -> %lu despues de mapear\n",
           antes, despues_de_mapear);
    printf("  (mapear no gasta memoria: todavia no se ha leido nada)\n");

    /* Tocar solo el primer byte. Eso trae UNA pagina, no el fichero. */
    char primero = p[0];
    printf("\n  leo el primer byte ('%c') -> quedan %lu paginas\n",
           primero >= ' ' ? primero : '?', freepages());

    /* Y ahora recorrerlo entero, como si fuera memoria de toda la vida.
     * Sin read(), sin buffer, sin bucle de peticiones: un puntero. */
    uint64_t lineas = 0, letras = 0;
    for (uint64_t i = 0; i < tam; i++) {
        if (p[i] == '\n') lineas++;
        if ((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z')) letras++;
    }

    printf("  lo recorro entero      -> quedan %lu paginas\n", freepages());
    printf("\n  %lu lineas, %lu letras, %lu bytes\n", lineas, letras, tam);

    /* Escribir esta prohibido: el mapeo es de solo lectura porque no hay
     * nada que devuelva los cambios al disco. Un mapeo que se deja
     * escribir y luego pierde lo escrito es peor que uno que no deja. */
    if (probar_escritura) {
        printf("\n  ahora escribo en el mapeo. Deberia morir aqui:\n");
        ((char *)p)[0] = 'X';
        printf("  NO HA MUERTO: el mapeo era escribible, y eso esta MAL\n");
        return 1;
    }

    return 0;
}
