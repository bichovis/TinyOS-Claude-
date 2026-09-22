/* syscall.h - Interfaz entre EL0 y el kernel
 *
 * Convencion (calcada de Linux en AArch64):
 *   x8       numero de llamada
 *   x0..x5   argumentos
 *   x0       valor devuelto
 * El proceso ejecuta 'svc #0' y la CPU salta al vector +0x400 (EL0/AArch64
 * sincrono), que llevamos preparado desde el paso 2 sin usarlo.
 */
#pragma once
#include <stdint.h>

#define SYS_write     1    /* (const char *buf, uint64_t len)             */
#define SYS_exit      2    /* (int codigo)                                */
#define SYS_yield     3    /* ()                                          */
#define SYS_getpid    4    /* () -> pid                                   */
#define SYS_sleep     5    /* (uint64_t ticks)                            */
#define SYS_uptime    6    /* () -> ms desde el arranque                  */

struct trap_frame;
void syscall_dispatch(struct trap_frame *f);
