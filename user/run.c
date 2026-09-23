/* user/run.c - Cargar un programa de la tarjeta y ejecutarlo
 *
 * Esto es lo que cierra el circulo: hasta ahora todos los programas venian
 * empotrados en la imagen del kernel por tools/bin2c.py. Este los saca de
 * la SD.
 *
 * Fijate en quien hace que. El kernel NO sabe leer ficheros y no le hace
 * falta: este proceso lee los bytes del servidor de ficheros y luego le
 * pide al kernel, con spawn(), que los convierta en un proceso. Si fuera
 * al reves -el kernel leyendo de un servidor de usuario- el kernel
 * dependeria de un proceso que puede morirse, y eso es justo lo que un
 * microkernel no hace.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "syscall.h"
#include "fs_abi.h"



/* Rehacer la linea de argumentos para el hijo: todo lo que venga despues
 * de "run". Asi "run HELLO.ELF uno dos" arranca HELLO.ELF viendose a si
 * mismo como argv[0] y con "uno dos" detras. */

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("\n  uso: run PROGRAMA.ELF [argumentos]\n");
        exit(1);
    }
    char ruta[FS_PATH_MAX];
    if (realpath(argv[1], ruta) < 0) { printf("  [run] ruta imposible\n"); exit(1); }
    const char *programa = ruta;
    /* Esto eran treinta lineas de pedirle trozos al servidor y meterlos en
     * un array de 32 KB. Ahora es una llamada, y el array ha desaparecido:
     * ni se reserva ni se llena. El kernel traera las paginas que necesite
     * mientras lee el ELF, y ni una mas. */
    uint64_t total = 0;
    const char *imagen = mmap(programa, &total);
    if (!imagen) {
        printf("  [run] no existe, o no hay servidor de ficheros (arrancalo con 'f')\n");
        exit(1);
    }

    printf("\n  [run] %s mapeado, %lu bytes\n", programa, total);

    /* argv + 1 y ya esta: nuestro propio argv ya viene terminado en cero,
     * asi que saltarse el nombre de "run" deja exactamente los argumentos
     * del hijo. Aqui habia una funcion que los juntaba todos en una
     * cadena con espacios para que el kernel los volviera a separar. */
    int64_t pid = spawn(imagen, total, argv + 1);
    if (pid < 0) printf("  [run] el kernel no lo ha querido\n");
    else         printf("  [run] arrancado como pid %lu\n", (uint64_t)pid);

    munmap(imagen);
    return 0;
}
