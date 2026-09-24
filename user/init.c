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

/* Recoger a cualquiera que haya muerto, sin bloquearse y sin preguntar por
 * nadie en concreto. No imprime: un manejador de senyal no puede llamar a
 * printf, que no es reentrante. Y no hace falta contar nada -de un driver que
 * se muere no hay codigo de salida que a init le sirva-, solo enterrarlo. */
static void enterrar(int sig)
{
    (void)sig;

    /* Con tope, como todo bucle cuya salida decide otro. */
    for (int v = 0; v < 32; v++)
        if (waitpid_ya(PID_CUALQUIERA, 0, 0) < 0) return;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n  [init] soy el pid %lu, y arranco el sistema\n", getpid());

    /* Y lo primero de todo: enterrar.
     *
     * Esto faltaba, y lo destapo el paso 58 al intentar comprobar que las
     * paginas de DMA de un driver vuelven cuando el driver muere. No
     * volvian, y no era un fallo del kernel: el recolector no toca un zombi
     * que tenga padre vivo -existe para que su padre lea su codigo de
     * salida- e init no esperaba a nadie mas que al interprete. Un driver
     * que se muriera se quedaba de zombi PARA SIEMPRE, con su memoria, su
     * ranura de tarea y, ahora, su tramo de DMA.
     *
     * Es el problema clasico del proceso 1, y tiene la solucion clasica: un
     * init entierra. Con SIGCHLD se entera en el acto (paso 54) y con
     * PID_CUALQUIERA puede recoger sin saber a quien (paso 55), que es justo
     * lo que hace falta aqui: init no lleva una lista de sus drivers.
     *
     * Y con SIG_REANUDAR, porque init se pasa la vida dentro de un waitpid
     * bloqueante y no quiere que se le rompa cada vez que muere alguien. */
    signal_banderas(SIGCHLD, enterrar, SIG_REANUDAR);

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

    /* 4b. Y la semilla del USB, AL FINAL y no entre los otros dos drivers.
     *
     * El orden importa, y lo ensenyo la Pi. Arrancandola justo detras del
     * servidor de ficheros, sus mensajes salian trenzados letra a letra con
     * los del conserver y los del fs:
     *
     *     ns[rse ]edvider de finhorasvivv  enEE00
     *
     * El motivo no es nuevo -es el de siempre: el kernel y el conserver son
     * dos drivers sobre la misma PL011, y el conserver escribe desde EL0 sin
     * cerrojo- pero antes no se veia al arrancar porque no habia tres
     * procesos hablando a la vez en esa ventana. En QEMU tampoco se ve,
     * porque los tiempos son otros.
     *
     * Esto no arregla el trenzado: arregla que lo provoque yo. Un driver de
     * algo que todavia no usa nadie no tiene por que competir por la consola
     * mientras arranca el sistema, y arrancarlo cuando los demas ya han
     * terminado de hablar es gratis. El trenzado de verdad se arregla el dia
     * que haya UN solo driver de la UART, y eso es la reforma que el README
     * lleva nombrando desde el paso 8.
     *
     * Y el sleep de abajo es exactamente lo que parece: una tirita. Lo
     * correcto seria que init esperara a que cada servidor este listo antes
     * de arrancar el siguiente -es lo que ya hace con el de ficheros,
     * preguntandole por stat("/")- y para eso la semilla necesita algo a lo
     * que se le pueda preguntar, que hoy no tiene porque todavia no es un
     * servidor. Cuando lo sea, este sleep se va y se hace como con el fs.
     *
     * Lo apunto en vez de disimularlo porque un sleep en un arranque es la
     * clase de linea que dentro de diez pasos nadie recuerda por que esta.
     *
     * No espera a nadie ni nadie la espera: si falla, se queja y el sistema
     * sigue igual. */
    if (bootstrap("usb", 0, environ, DEV_USB) < 0)
        printf("  [init] la semilla de USB no arranca, sigo sin ella\n");
    else
        sleep(10);          /* 100 ms: que acabe de hablar antes del shell */

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
