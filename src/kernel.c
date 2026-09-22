/* kernel.c - Primer codigo C de TinyOS */
#include <stdint.h>
#include "uart.h"

/* Lee CurrentEL: nos dice en que nivel de privilegio nos dejo el firmware.
 * EL0=usuario, EL1=kernel, EL2=hipervisor, EL3=firmware seguro.
 * El valor viene en los bits [3:2], por eso el >> 2.                      */
static uint64_t current_el(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return el >> 2;
}

static uint64_t read_mpidr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v;
}

/* Llamado desde boot.S. x0 (el dtb) llega como primer argumento. */
void kernel_main(uint64_t dtb_ptr)
{
    uart_init();

    uart_puts("\n");
    uart_puts("+--------------------------------------+\n");
    uart_puts("|  TinyOS  -  microkernel RPi 3B       |\n");
    uart_puts("|  paso 1: arranque y consola serie    |\n");
    uart_puts("+--------------------------------------+\n\n");

    uart_puts("  Exception Level : EL");
    uart_putc((char)('0' + current_el()));
    uart_puts("\n");

    uart_puts("  MPIDR_EL1       : 0x");
    uart_hex64(read_mpidr());
    uart_puts("\n");

    uart_puts("  Device tree en  : 0x");
    uart_hex64(dtb_ptr);
    uart_puts("\n\n");

    uart_puts("Eco activo. Escribe algo (Ctrl-A X para salir de QEMU):\n> ");

    for (;;) {
        char c = uart_getc();
        if (c == '\r') uart_puts("\n> ");
        else uart_putc(c);
    }
}
