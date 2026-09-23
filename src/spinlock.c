/* spinlock.c - Exclusion mutua entre nucleos */
#include <stdint.h>
#include "spinlock.h"
#include "irq.h"

void spin_lock(struct spinlock *l)
{
    uint32_t tmp;

    __asm__ volatile(
        /* 'sevl' arma un evento para que el primer 'wfe' no se duerma: sin
         * el, un cerrojo libre costaria una espera entera. */
        "   sevl\n"
        "1: wfe\n"                       /* dormir hasta que pase algo     */
        "2: ldaxr   %w0, [%1]\n"         /* leer y reservar la direccion   */
        "   cbnz    %w0, 1b\n"           /* esta cerrado: a esperar        */
        "   stlxr   %w0, %w2, [%1]\n"    /* intentar cerrarlo              */
        "   cbnz    %w0, 2b\n"           /* alguien se colo: reintentar    */
        : "=&r"(tmp)
        : "r"(&l->locked), "r"(1u)
        : "memory");
}

void spin_unlock(struct spinlock *l)
{
    __asm__ volatile(
        "   stlr    wzr, [%0]\n"         /* abrir, con semantica release   */
        "   sev\n"                       /* y avisar a los que esperan     */
        :: "r"(&l->locked) : "memory");
}

uint64_t spin_lock_irqsave(struct spinlock *l)
{
    uint64_t flags = irq_save();     /* primero tapar... */
    spin_lock(l);                    /* ...y despues cerrar */
    return flags;
}

void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags)
{
    spin_unlock(l);                  /* y al reves para salir */
    irq_restore(flags);
}
