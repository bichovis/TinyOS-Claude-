/* syscall.h - Interfaz entre EL0 y el kernel
 *
 * Convencion (calcada de Linux en AArch64):
 *   x8       numero de llamada
 *   x0..x5   argumentos
 *   x0       valor devuelto
 * El proceso ejecuta 'svc #0' y la CPU salta al vector +0x400 (EL0/AArch64
 * sincrono), que llevabamos preparado desde el paso 2 sin usarlo.
 *
 * Los numeros viven en ipc_abi.h, que es el contrato compartido con user/.
 */
#pragma once
#include <stdint.h>
#include "ipc_abi.h"

struct trap_frame;
void syscall_dispatch(struct trap_frame *f);
