/* fpu.c - La politica. El "como" esta en fpu.S; aqui esta el "cuando".
 *
 * Ver include/fpu.h para la idea y para por que no es perezosa del todo.
 */
#include <stdint.h>
#include "fpu.h"
#include "sched.h"
#include "mm.h"
#include "uart.h"

static uint64_t trampas;      /* cuantas veces se ha encendido la FPU */
static uint64_t areas;        /* cuantos hilos la tienen reservada AHORA */
static uint64_t pedidas;      /* cuantos han llegado a pedirla, en total */

/* Reservarle el area a un hilo. Todo el que consiga una pasa por aqui -la
 * trampa y el fork-, y por eso las cuentas viven aqui dentro y no
 * repartidas por el kernel.
 *
 * 'pedidas' cuenta HILOS, no reservas: un hijo que hereda el area del
 * padre y luego hace exec la pierde y la vuelve a pedir, y sigue siendo un
 * solo hilo. De ahi la marca, que no se borra hasta que la ranura se
 * recicla para otro. */
int fp_area_alloc(struct task *t)
{
    if (!t) return 0;
    if (t->fp_state) return 1;

    void *area = kmalloc(FP_STATE_SIZE);
    if (!area) return 0;                     /* sin memoria: que muera */

    /* A cero, que es el estado de arranque que espera cualquier programa:
     * registros en cero y FPCR con redondeo al par. Dejarle lo que hubiera
     * de otro proceso seria una fuga de datos entre procesos, y de las
     * silenciosas. */
    for (int i = 0; i < FP_STATE_SIZE; i++) ((char *)area)[i] = 0;

    t->fp_state = area;
    areas++;
    if (!t->fp_pedida) { t->fp_pedida = 1; pedidas++; }
    return 1;
}

int fp_trap(void)
{
    struct task *t = current;
    if (!t) return 0;

    if (!fp_area_alloc(t)) return 0;

    trampas++;

    fp_hw_enable();
    fp_restore(t->fp_state);
    t->fp_activo = 1;
    return 1;
}

void fp_switch_out(struct task *t)
{
    if (!t || !t->fp_activo) return;         /* no la encendio: nada que hacer */

    fp_save(t->fp_state);
    t->fp_activo = 0;
    fp_hw_disable();
}

void fp_release(struct task *t)
{
    if (!t) return;
    t->fp_activo = 0;
    if (!t->fp_state) return;

    kfree(t->fp_state);
    t->fp_state = 0;
    if (areas) areas--;
}

void fp_stats(void)
{
    uart_puts("\n  coma flotante (perezosa)\n");
    uart_puts("  veces que se ha encendido : ");
    uart_dec(trampas);
    uart_puts("\n  hilos que la han pedido   : ");
    uart_dec(pedidas);
    uart_puts("\n  la tienen reservada ahora : ");
    uart_dec(areas);
    uart_puts("\n  bytes por area            : ");
    uart_dec(FP_STATE_SIZE);
    uart_puts("\n  hilos creados en total    : ");
    uart_dec(task_creados());
    uart_puts("\n");
}
