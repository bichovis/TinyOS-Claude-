/* kernel.c - Nucleo de TinyOS */
#include <stdint.h>
#include "uart.h"
#include "exception.h"
#include "irq.h"
#include "timer.h"

#define TICK_HZ   100          /* 100 ticks por segundo = uno cada 10 ms */

static uint64_t current_el(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return el >> 2;
}

static void menu(void)
{
    uart_puts("\nComandos:\n");
    uart_puts("  t - estadisticas de tiempo e interrupciones\n");
    uart_puts("  m - tapar las IRQ 2 segundos (mira que pasa con los ticks)\n");
    uart_puts("  h - esta ayuda\n");
    uart_puts("  1 - BRK #0                    (excepcion recuperable)\n");
    uart_puts("  3 - leer SCTLR_EL2 desde EL1  (FATAL)\n");
    uart_puts("  4 - direccion fisica invalida (FATAL: data abort)\n");
    uart_puts("  resto: eco\n");
}

static void stats(void)
{
    uart_puts("  uptime : ");
    uart_dec(timer_uptime_ms());
    uart_puts(" ms\n  ticks  : ");
    uart_dec(timer_ticks());
    uart_puts("\n  IRQs   : ");
    uart_dec(irq_count());
    uart_puts("\n");
}

static void command(char c)
{
    switch (c) {
    case 't':
        uart_puts("\n");
        stats();
        break;

    case 'm': {
        /* Tapar las IRQ y esperar 2 s con una espera activa sobre CNTPCT
         * (que no necesita interrupciones). Al volver veras que el uptime
         * ha avanzado 2000 ms pero los ticks NO: las interrupciones del
         * temporizador no se han "acumulado", se han PERDIDO. Por eso un
         * kernel debe pasar el menor tiempo posible con las IRQ tapadas. */
        uart_puts("\nAntes de tapar:\n");
        stats();
        uint64_t flags = irq_save();
        delay_ms(2000);
        irq_restore(flags);
        uart_puts("Despues de 2 s con las IRQ tapadas:\n");
        stats();
        break;
    }

    case 'h':
        menu();
        break;

    case '1':
        uart_puts("\n");
        __asm__ volatile("brk #0");
        break;

    case '3': {
        uint64_t v;
        __asm__ volatile("mrs %0, sctlr_el2" : "=r"(v));
        break;
    }

    case '4': {
        volatile uint32_t *p = (volatile uint32_t *)0x0000FF0000000000UL;
        uint32_t v = *p;
        (void)v;
        break;
    }

    default:
        uart_putc(c);          /* eco */
        break;
    }
}

void kernel_main(uint64_t dtb_ptr)
{
    uart_init();
    exception_init();
    irq_init();

    uart_puts("\n");
    uart_puts("+--------------------------------------+\n");
    uart_puts("|  TinyOS  -  microkernel RPi 3B       |\n");
    uart_puts("|  paso 3: timer e interrupciones      |\n");
    uart_puts("+--------------------------------------+\n\n");

    uart_puts("  Exception Level : EL");
    uart_putc((char)('0' + current_el()));
    uart_puts("\n  Device tree en  : 0x");
    uart_hex64(dtb_ptr);
    uart_puts("\n");

    timer_init(TICK_HZ);
    uart_puts("  CNTFRQ_EL0      : ");
    uart_dec(timer_hz());
    uart_puts(" Hz\n  Tick            : ");
    uart_dec(TICK_HZ);
    uart_puts(" Hz (cada ");
    uart_dec(1000 / TICK_HZ);
    uart_puts(" ms)\n");

    uart_enable_rx_irq();

    /* Hasta esta linea, DAIF tenia las IRQ tapadas desde boot.S. */
    irq_enable();
    uart_puts("\n  IRQ habilitadas. El kernel duerme en 'wfi' entre ellas.\n");

    menu();

    uint64_t last_second = 0;
    for (;;) {
        /* wfi = Wait For Interrupt: para el nucleo hasta que algo pase.
         * Todo el trabajo real ocurre dentro de los handlers; aqui solo
         * recogemos lo que han dejado. Un sistema ocioso no quema CPU. */
        __asm__ volatile("wfi");

        char c;
        while (uart_read(&c))
            command(c);

        uint64_t secs = timer_ticks() / TICK_HZ;
        if (secs != last_second) {
            last_second = secs;
            uart_puts("[tick] ");
            uart_dec(secs);
            uart_puts(" s   ticks=");
            uart_dec(timer_ticks());
            uart_puts("  irqs=");
            uart_dec(irq_count());
            uart_puts("\n");
        }
    }
}
