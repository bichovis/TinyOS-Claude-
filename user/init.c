/* user/init.c - El primer proceso
 *
 * El kernel arranca UNO solo, y a partir de ahi todo lo demas lo arranca
 * este. Antes esa lista estaba en un switch del menu del kernel, junto a
 * las demostraciones, y el entorno inicial era una cadena literal dentro
 * de sched.c. O sea que el kernel sabia lo que es un shell, en que orden
 * van los drivers y que variables tiene que heredar un proceso. Nada de
 * eso es asunto suyo.
 *
 * Aqui esta esa politica, en EL0, sin privilegios salvo dos: puede
 * arrancar los programas que el kernel lleva dentro -con el dispositivo
 * que cada uno necesite- y puede decir quien manda en la consola. El
 * kernel se los concede al primer proceso y a nadie mas.
 *
 * EL PROBLEMA DEL HUEVO Y LA GALLINA. El servidor de ficheros es un
 * programa, y para leer un programa de la tarjeta hace falta el servidor
 * de ficheros. Alguien tiene que traer los primeros dentro, y ese alguien
 * es el kernel: lleva empotrados el conserver, el fs y el sh. Es lo mismo
 * que hace un initramfs, con tres entradas en vez de un sistema de
 * ficheros entero.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include "fs_abi.h"

/* ¿Contesta ya el servidor de ficheros?
 *
 * Arrancarlo no es tenerlo: tiene que hablar con la tarjeta, leer el MBR
 * y montar dos volumenes, y eso tarda. Preguntar por algo que existe
 * seguro es la unica forma de saber que esta listo; un "sleep" generoso
 * seria adivinar.
 *
 * Y se pregunta con stat(), como haria cualquiera. Ni siquiera init tiene
 * que saber como se habla con el servidor. */
static int fs_responde(void)
{
    struct estado e;
    return stat("/", &e) == 0;
}

/* Leer /etc/rc y meter en el entorno cada linea "NOMBRE=valor".
 *
 * Que el entorno inicial salga de un FICHERO y no del codigo es la mitad
 * del sentido de tener un init: cambiar el PATH deja de ser recompilar el
 * sistema operativo y pasa a ser editar una linea desde el propio TinyOS.
 */
static int leer_rc(const char *ruta)
{
    uint64_t tam = 0;
    const char *p = mmap(ruta, &tam);
    if (!p) return 0;

    int puestas = 0;
    uint64_t i = 0;

    while (i < tam) {
        char linea[128];
        uint64_t n = 0;

        while (i < tam && p[i] != '\n' && n < sizeof(linea) - 1) linea[n++] = p[i++];
        while (i < tam && p[i] != '\n') i++;
        i++;                                  /* saltar el salto */
        linea[n] = 0;

        if (!n || linea[0] == '#') continue;  /* vacia o comentario */

        char *igual = strchr(linea, '=');
        if (!igual) continue;
        *igual = 0;

        if (setenv(linea, igual + 1) == 0) puestas++;
    }

    munmap(p);
    return puestas;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n  [init] soy el pid %lu, y arranco el sistema\n", getpid());

    /* 1. La consola primero. Hasta que este, todo lo que se imprima sale
     *    por la via de emergencia del kernel. */
    if (bootstrap("conserver", 0, environ, DEV_UART) < 0) {
        printf("  [init] no arranca el driver de consola\n");
        return 1;
    }

    /* 2. Los ficheros. */
    if (bootstrap("fs", 0, environ, DEV_EMMC) < 0) {
        printf("  [init] no arranca el servidor de ficheros\n");
        return 1;
    }

    /* 3. Esperarlo, preguntando. */
    int vueltas = 0;
    while (!fs_responde()) {
        if (++vueltas > 200) {           /* 200 x 25 ms = 5 segundos */
            printf("  [init] el servidor de ficheros no contesta\n");
            break;
        }
        sleep(3);
    }

    /* 4. La configuracion. */
    int n = leer_rc("/etc/rc");
    if (n) printf("  [init] /etc/rc: %d variables\n", n);
    else   printf("  [init] sin /etc/rc, sigo con lo que traigo\n");

    /* 5. Y el interprete, para siempre: si se va, vuelve. Eso es lo que
     *    hace que "salir" no deje la maquina muda. */
    for (;;) {
        int64_t sh = bootstrap("sh", 0, environ, DEV_NINGUNO);
        if (sh < 0) {
            printf("  [init] no arranca el interprete\n");
            return 1;
        }

        consola((uint64_t)sh);           /* que el Ctrl-C vaya a el */
        waitpid((uint64_t)sh);
        consola(0);

        printf("\n  [init] el interprete se ha ido. Arranco otro.\n");
    }
}
