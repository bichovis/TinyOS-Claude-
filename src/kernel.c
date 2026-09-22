/* kernel.c - Nucleo de TinyOS */
#include <stdint.h>
#include "uart.h"
#include "exception.h"

/* Lee CurrentEL: nos dice en que nivel de privilegio estamos.
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

static void menu(void)
{
    uart_puts("\nPruebas de excepciones:\n");
    uart_puts("  1 - BRK #0      (recuperable: el kernel sigue vivo)\n");
    uart_puts("  2 - BRK #0x1234 (recuperable, con otro inmediato)\n");
    uart_puts("  3 - leer SCTLR_EL2 desde EL1  (FATAL: no tenemos permiso)\n");
    uart_puts("  4 - leer una direccion fisica inexistente (FATAL: abort)\n");
    uart_puts("  5 - saltar a una direccion absurda (FATAL: instr. abort)\n");
    uart_puts("  cualquier otra tecla: eco\n");
}

void kernel_main(uint64_t dtb_ptr)
{
    uart_init();
    exception_init();          /* a partir de aqui las excepciones se manejan */

    uart_puts("\n");
    uart_puts("+--------------------------------------+\n");
    uart_puts("|  TinyOS  -  microkernel RPi 3B       |\n");
    uart_puts("|  paso 2: EL1 y vectores              |\n");
    uart_puts("+--------------------------------------+\n\n");

    uart_puts("  Exception Level : EL");
    uart_putc((char)('0' + current_el()));
    uart_puts(current_el() == 1 ? "  <- correcto\n" : "  <- ERROR\n");

    uart_puts("  MPIDR_EL1       : 0x");
    uart_hex64(read_mpidr());
    uart_puts("\n");

    uart_puts("  Device tree en  : 0x");
    uart_hex64(dtb_ptr);
    uart_puts("\n");

    {   /* Comprobar que VBAR_EL1 apunta de verdad a nuestra tabla */
        uint64_t vbar;
        __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
        uart_puts("  VBAR_EL1        : 0x");
        uart_hex64(vbar);
        uart_puts("\n");
    }

    uart_puts("\nProbando el vector sincrono con un BRK...\n");
    __asm__ volatile("brk #0");      /* si no volvemos de aqui, algo falla */
    uart_puts("Hemos vuelto del BRK: el manejador funciona.\n");

    menu();

    for (;;) {
        uart_puts("\n> ");
        char c = uart_getc();
        uart_putc(c);
        uart_puts("\n");

        switch (c) {
        case '1':
            __asm__ volatile("brk #0");
            break;
        case '2':
            __asm__ volatile("brk #0x1234");
            break;
        case '3': {
            /* SCTLR_EL2 solo es accesible desde EL2 o EL3. Si de verdad
             * estamos en EL1, esto tiene que explotar. */
            uint64_t v;
            __asm__ volatile("mrs %0, sctlr_el2" : "=r"(v));
            uart_puts("No deberiamos llegar aqui.\n");
            break;
        }
        case '4': {
            /* El Cortex-A53 direcciona 40 bits fisicos. Con la MMU apagada
             * cualquier direccion por encima de 2^40 es invalida. */
            volatile uint32_t *p = (volatile uint32_t *)0x0000FF0000000000UL;
            uint32_t v = *p;
            (void)v;
            uart_puts("No deberiamos llegar aqui.\n");
            break;
        }
        case '5': {
            void (*f)(void) = (void (*)(void))0x0000FF0000000000UL;
            f();
            break;
        }
        case 'h':
            menu();
            break;
        default:
            break;
        }
    }
}
