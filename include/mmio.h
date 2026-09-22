/* mmio.h - Acceso a registros de periferico (Memory Mapped I/O)
 *
 * En ARM no existen instrucciones de "puerto" como el IN/OUT de x86.
 * Los perifericos son direcciones de memoria: escribir en 0x3F201000 es
 * escribir en el registro de datos de la UART.
 */
#pragma once
#include <stdint.h>

/* Base de perifericos del BCM2837 (Raspberry Pi 3).
 * Ojo: en la Pi 1 era 0x20000000 y en la Pi 4 es 0xFE000000. */
#define PERIPHERAL_BASE   0x3F000000UL

/* 'volatile' es obligatorio: le prohibe al compilador cachear, reordenar o
 * eliminar estos accesos. Sin el, -O2 borraria medio driver. */
static inline void mmio_write(uint64_t reg, uint32_t val)
{
    *(volatile uint32_t *)reg = val;
}

static inline uint32_t mmio_read(uint64_t reg)
{
    return *(volatile uint32_t *)reg;
}

/* Espera activa tosca: 'count' iteraciones que el compilador no puede
 * optimizar. Suficiente para los retardos que exige el manual del GPIO. */
static inline void delay_cycles(int32_t count)
{
    __asm__ volatile("1: subs %w0, %w0, #1; bne 1b"
                     : "=r"(count) : "0"(count) : "cc");
}
