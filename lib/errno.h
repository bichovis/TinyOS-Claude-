/* errno.h - Por que fallo lo ultimo que hiciste
 *
 * Es una variable global, y eso hoy suena a error de disenyo: una funcion
 * que informa de un fallo escribiendo en un sitio comun en vez de
 * devolverlo. Con hilos hace falta que sea una por hilo, y hay que
 * acordarse de mirarla ANTES de llamar a otra cosa.
 *
 * Pero se entiende de donde sale: en 1970 una funcion de C devolvia un
 * solo valor, y meter el motivo del fallo dentro de ese valor significaba
 * quitarle sitio al resultado. Poner el motivo aparte fue la unica salida
 * que no rompia read() ni write().
 *
 * Aqui el kernel SI devuelve el motivo dentro del valor -en el rango de
 * -4095 a -1, como Linux- y es la libc la que lo copia a errno. O sea que
 * la variable global no es como se transporta el error: es como se le
 * entrega a un programa que espera un Unix.
 */
#pragma once
#include "ipc_abi.h"

extern int errno;

/* El texto de un codigo. Los mensajes son cortos y dicen QUE paso, no
 * "error N": un numero obliga a ir a buscarlo a una tabla. */
const char *strerror(int codigo);
