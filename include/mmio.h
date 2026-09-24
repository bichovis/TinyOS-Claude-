/* mmio.h - Acceso a registros de periferico (Memory Mapped I/O)
 *
 * En ARM no existen instrucciones de "puerto" como el IN/OUT de x86.
 * Los perifericos son direcciones de memoria: escribir en 0x3F201000 es
 * escribir en el registro de datos de la UART.
 */
#pragma once

/* Los dos perifericos que se pueden conceder a un driver de EL0. Viven
 * aqui y no en kernel.c porque quien decide a quien se conceden ya no es
 * el menu: es task_bootstrap, y el unico que puede pedirlo es init. */
#include <stdint.h>
#include "mm.h"

/* Base FISICA de los perifericos del BCM2837 (Raspberry Pi 3).
 * Ojo: en la Pi 1 era 0x20000000 y en la Pi 4 es 0xFE000000. */
#define PERIPHERAL_PA     0x3F000000UL

/* ...y la direccion por la que los ve el kernel. Desde que el kernel vive
 * en TTBR1 no puede tocar una direccion fisica directamente: los 0x3F...
 * de los manuales de Broadcom caen en el espacio del proceso. */
#define PERIPHERAL_BASE   (KERNEL_VA_BASE + PERIPHERAL_PA)

/* Registros de la PL011. Se los concedemos al driver de consola para que
 * pueda hacer su trabajo desde EL0 sin pasar por el kernel. Aqui hace falta
 * la direccion FISICA (es la que se va a meter en una tabla de paginas),
 * no la virtual por la que los ve el kernel. */
#define UART0_PHYS  (PERIPHERAL_PA + 0x201000)

/* Registros del controlador EMMC. Se los concedemos al servidor de
 * ficheros por el mismo camino: una pagina de MMIO en su espacio, y a
 * partir de ahi habla con la tarjeta sin pasar por el kernel. */
#define EMMC_PHYS   (PERIPHERAL_PA + 0x300000)

/* El controlador USB: un Synopsys DesignWare DWC2 en modo anfitrion. En la
 * Pi 3B no es "un puerto mas": de el cuelga un LAN9514, que es a la vez el
 * hub de los cuatro conectores Y la tarjeta de red. O sea que sin USB no
 * hay red, y sin hub no hay USB, porque hasta la Ethernet esta detras del
 * hub interno. */
#define USB_PHYS    (PERIPHERAL_PA + 0x980000)


/* Los "ARM local peripherals" (timers y mailboxes por nucleo) viven fuera
 * del bloque anterior, en 0x40000000. */
#define LOCAL_PA          0x40000000UL
#define LOCAL_BASE        (KERNEL_VA_BASE + LOCAL_PA)

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
