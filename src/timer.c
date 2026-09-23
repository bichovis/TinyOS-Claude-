/* timer.c - Temporizador generico de ARMv8
 *
 * Este temporizador NO es de Broadcom: forma parte de la arquitectura ARM y
 * existe en cualquier ARMv8. Cada nucleo tiene el suyo, y eso lo hace la
 * fuente natural del tick de un planificador.
 *
 * Piezas:
 *   CNTFRQ_EL0     frecuencia del contador, en Hz (la fija el firmware)
 *   CNTPCT_EL0     contador global, solo sube; es nuestro reloj absoluto
 *   CNTP_TVAL_EL0  cuenta atras: escribes N y dispara cuando llega a 0
 *   CNTP_CTL_EL0   bit0 ENABLE, bit1 IMASK (1=tapada), bit2 ISTATUS (ya disparo)
 *
 * Recuerda que en boot.S pusimos CNTHCTL_EL2 |= 3: sin eso, EL1 no tendria
 * permiso para leer ni escribir nada de esto.
 */
#include <stdint.h>
#include "timer.h"
#include "uart.h"
#include "sched.h"
#include "smp.h"

static uint32_t          counter_hz;    /* frecuencia del contador       */
static uint32_t          interval;      /* cuentas entre dos ticks       */
static volatile uint64_t ticks;         /* la modifica la IRQ            */

static inline uint64_t read_cntfrq(void)
{
    uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}

static inline uint64_t read_cntpct(void)
{
    /* isb: impide que la CPU adelante esta lectura y nos de un valor viejo */
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline void write_tval(uint32_t v)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"((uint64_t)v));
}

static inline void write_ctl(uint64_t v)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
}

/* El ritmo lo fija el nucleo 0 una sola vez. */
void timer_init(uint32_t hz)
{
    counter_hz = (uint32_t)read_cntfrq();
    if (counter_hz == 0)
        counter_hz = 19200000;          /* la Pi 3 real: 19.2 MHz         */

    interval = counter_hz / hz;
    ticks    = 0;

    timer_start_core();
}

/* Pero el temporizador es de cada nucleo: CNTP_TVAL_EL0 y CNTP_CTL_EL0 son
 * registros de CPU, no un periferico compartido. Cada nucleo tiene que
 * armar el suyo o no recibira un solo tick.
 *
 * Se ponen explicitamente los dos aunque "deberian" venir a cero: en tres
 * de los cuatro nucleos nadie los ha tocado nunca, y lo que haya dejado el
 * firmware no es asunto nuestro. */
void timer_start_core(void)
{
    write_ctl(0);                       /* parado mientras lo configuramos */
    write_tval(interval);
    write_ctl(1);                       /* ENABLE=1, IMASK=0 -> que avise  */
}

/* --- El reloj de pared -------------------------------------------------
 *
 * La Pi no tiene reloj de tiempo real: al arrancar no sabe que dia es.
 * Lo que si sabe es cuanto lleva encendida, asi que la hora es una suma y
 * la base la pone alguien.
 *
 * FECHA_COMPILACION la inyecta el Makefile con la fecha de la maquina que
 * compilo. No es la hora, pero cumple lo unico que hace falta para que
 * make funcione: que un fichero escrito despues tenga una marca mayor que
 * uno escrito antes. Y no queda atras de los fuentes, porque los fuentes
 * se copiaron a la tarjeta con esta misma maquina.
 *
 * Lo que NO arregla: dos arranques seguidos empiezan en la misma base, asi
 * que un fichero de la sesion de ayer puede parecer mas nuevo que uno de
 * hoy. Eso solo lo cura un reloj de verdad o que init guarde la hora al
 * salir, y esta en las limitaciones. */
#ifndef FECHA_COMPILACION
#define FECHA_COMPILACION 1700000000UL   /* noviembre de 2023, por decir algo */
#endif

static uint64_t reloj_base = FECHA_COMPILACION;

uint64_t reloj_ahora(void)
{
    uint32_t hz = timer_hz();
    if (!hz) return reloj_base;
    return reloj_base + timer_now() / hz;
}

void reloj_poner(uint64_t segundos)
{
    uint32_t hz = timer_hz();
    reloj_base = segundos - (hz ? timer_now() / hz : 0);
}

void timer_irq(void)
{
    /* 'ticks' es el reloj del SISTEMA, no el de este nucleo. Si lo subieran
     * los cuatro, el tiempo correria al cuadruple y task_sleep() dormiria
     * la cuarta parte. Lo lleva el nucleo 0 y nadie mas. */
    if (this_core() == 0)
        ticks++;

    scheduler_tick();          /* esto si es de cada nucleo */

    /* Rearmar. Escribir TVAL de nuevo tambien baja la senal de interrupcion:
     * es asi como se "reconoce" este temporizador, no hay registro de ACK. */
    write_tval(interval);
}

uint64_t timer_ticks(void) { return ticks; }
uint32_t timer_hz(void)    { return counter_hz; }

uint64_t timer_uptime_ms(void)
{
    /* Dividimos primero para no desbordar: CNTPCT llega a 64 bits. */
    return read_cntpct() / (counter_hz / 1000);
}

void delay_ms(uint32_t ms)
{
    uint64_t target = read_cntpct() + (uint64_t)ms * (counter_hz / 1000);
    while (read_cntpct() < target)
        ;
}

uint64_t timer_now(void) { return read_cntpct(); }

uint64_t timer_us(uint64_t cycles)
{
    return cycles * 1000000UL / counter_hz;
}
