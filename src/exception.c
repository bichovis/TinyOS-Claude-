/* exception.c - Manejo de excepciones en EL1
 *
 * Cuando algo va mal, la CPU nos da tres pistas:
 *   ESR_EL1  "Exception Syndrome Register": QUE ha pasado, codificado.
 *   ELR_EL1  "Exception Link Register": QUE instruccion lo provoco.
 *   FAR_EL1  "Fault Address Register": QUE direccion se intento tocar
 *            (solo tiene sentido en fallos de memoria).
 * Traducir eso a texto es la diferencia entre depurar y adivinar.
 */
#include <stdint.h>
#include "uart.h"
#include "exception.h"
#include "irq.h"
#include "syscall.h"
#include "sched.h"

extern char vector_table[];   /* definido en vectors.S */

/* Nombres de las 16 entradas de la tabla, en el mismo orden. */
static const char *const vector_name[16] = {
    "EL1t Synchronous", "EL1t IRQ", "EL1t FIQ", "EL1t SError",
    "EL1h Synchronous", "EL1h IRQ", "EL1h FIQ", "EL1h SError",
    "EL0/64 Synchronous", "EL0/64 IRQ", "EL0/64 FIQ", "EL0/64 SError",
    "EL0/32 Synchronous", "EL0/32 IRQ", "EL0/32 FIQ", "EL0/32 SError",
};

/* EC = Exception Class, los 6 bits altos de ESR_EL1. Es el "codigo de error"
 * principal. Aqui solo los que nos pueden salir en esta fase del kernel. */
static const char *ec_name(uint32_t ec)
{
    switch (ec) {
    case 0x00: return "Instruccion desconocida / no definida";
    case 0x01: return "WFI/WFE atrapada";
    case 0x07: return "Acceso a FP/SIMD atrapado";
    case 0x0E: return "Estado de ejecucion ilegal";
    case 0x15: return "SVC desde AArch64 (llamada al sistema)";
    case 0x16: return "HVC desde AArch64";
    case 0x17: return "SMC desde AArch64";
    case 0x18: return "MSR/MRS a registro no permitido en este EL";
    case 0x20: return "Instruction Abort desde EL inferior";
    case 0x21: return "Instruction Abort en el mismo EL";
    case 0x22: return "PC desalineado";
    case 0x24: return "Data Abort desde EL inferior";
    case 0x25: return "Data Abort en el mismo EL";
    case 0x26: return "SP desalineado";
    case 0x2C: return "Excepcion de coma flotante";
    case 0x2F: return "SError (fallo asincrono del bus)";
    case 0x30: case 0x31: return "Breakpoint hardware";
    case 0x32: case 0x33: return "Software step";
    case 0x34: case 0x35: return "Watchpoint";
    case 0x3C: return "BRK (breakpoint software)";
    default:   return "(clase no catalogada)";
    }
}

/* DFSC/IFSC = los 6 bits bajos del ISS en un abort de memoria: dice en que
 * fase de la traduccion se rompio. Sera oro puro cuando activemos la MMU. */
static const char *fsc_name(uint32_t fsc)
{
    if ((fsc & 0x3C) == 0x00) return "Fallo de tamano de direccion";
    if ((fsc & 0x3C) == 0x04) return "Fallo de traduccion (pagina no mapeada)";
    if ((fsc & 0x3C) == 0x08) return "Fallo de Access Flag";
    if ((fsc & 0x3C) == 0x0C) return "Fallo de permisos";
    switch (fsc) {
    case 0x10: return "Abort externo sincrono";
    case 0x21: return "Alineacion";
    case 0x30: return "Fallo de TLB conflict";
    default:   return "(codigo no catalogado)";
    }
}

static uint64_t read_far(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, far_el1" : "=r"(v));
    return v;
}

static void row(const char *label, uint64_t value)
{
    uart_puts("  ");
    uart_puts(label);
    uart_puts(" : 0x");
    uart_hex64(value);
    uart_puts("\n");
}

static void dump(struct trap_frame *f, uint64_t index)
{
    uint32_t ec  = (uint32_t)(f->esr >> 26) & 0x3F;
    uint32_t iss = (uint32_t)(f->esr & 0x01FFFFFF);

    uart_puts("\n");
    uart_puts("############## EXCEPCION ##############\n");

    uart_puts("  Vector  : #");
    uart_putc((char)('0' + index / 10));
    uart_putc((char)('0' + index % 10));
    uart_puts("  ");
    uart_puts(vector_name[index & 15]);
    uart_puts("\n");

    uart_puts("  Causa   : EC=0x");
    uart_hex8((uint8_t)ec);
    uart_puts("  ");
    uart_puts(ec_name(ec));
    uart_puts("\n");

    /* En los aborts de memoria, el ISS lleva el codigo de fallo y FAR_EL1
     * la direccion culpable. En el resto, FAR no significa nada. */
    if (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) {
        uart_puts("  Detalle : ");
        uart_puts(fsc_name(iss & 0x3F));
        uart_puts((iss & (1u << 6)) ? " (escritura)" : " (lectura)");
        uart_puts("\n");
        row("Direccion (FAR_EL1)", read_far());
    }

    row("ESR_EL1 ", f->esr);
    row("ELR_EL1 ", f->elr);
    row("SPSR_EL1", f->spsr);

    uart_puts("  --- registros ---\n");
    for (int i = 0; i < 30; i += 2) {
        uart_puts("   x");
        uart_putc((char)('0' + i / 10));
        uart_putc((char)('0' + i % 10));
        uart_puts("=0x");
        uart_hex64(f->x[i]);
        uart_puts("   x");
        uart_putc((char)('0' + (i + 1) / 10));
        uart_putc((char)('0' + (i + 1) % 10));
        uart_puts("=0x");
        uart_hex64(f->x[i + 1]);
        uart_puts("\n");
    }
    uart_puts("   x30=0x");
    uart_hex64(f->lr);
    uart_puts("\n");
    uart_puts("#######################################\n");
}

void panic(const char *msg)
{
    uart_puts("\n*** PANIC: ");
    uart_puts(msg);
    uart_puts(" ***\nSistema detenido.\n");
    for (;;)
        __asm__ volatile("wfe");
}

/* Punto de entrada desde vectors.S. */
void exception_dispatch(struct trap_frame *f, uint64_t index)
{
    uint32_t ec = (uint32_t)(f->esr >> 26) & 0x3F;

    /* Los dos bits bajos del indice dicen el TIPO dentro de cada grupo de la
     * tabla: 0=Synchronous, 1=IRQ, 2=FIQ, 3=SError. Asi el mismo codigo
     * atiende las IRQ vengan de EL1 (vector 5) o de EL0 (vector 9). */
    if ((index & 3) == 1) {
        irq_handle();
        return;
    }

    /* Vector 8 = excepcion sincrona desde EL0 en AArch64. Si la causa es
     * EC=0x15 (SVC), es una llamada al sistema: el proceso ha pedido algo.
     * Cualquier otra cosa desde EL0 es un fallo suyo, y ahi no se hace
     * panic del sistema: se mata al proceso y el kernel sigue. */
    if (index == 8 && ec == 0x15) {
        syscall_dispatch(f);
        return;
    }

    /* BRK es una excepcion "de mentira": la pedimos nosotros. La informamos
     * y seguimos adelante saltando por encima de la instruccion brk.
     * Esto demuestra algo importante: el handler puede MODIFICAR el estado
     * guardado, y kernel_exit lo restaurara. Asi funcionaran las syscalls. */
    if (ec == 0x3C) {
        uart_puts("[BRK] breakpoint software en 0x");
        uart_hex64(f->elr);
        uart_puts(", imm=0x");
        uart_hex32(f->esr & 0xFFFF);
        uart_puts(" -> continuamos\n");
        f->elr += 4;              /* saltar la instruccion brk (4 bytes) */
        return;
    }

    /* Fallo de traduccion en un proceso, justo debajo de su pila: no es un
     * error, es que necesita mas sitio. Se le da y se REINTENTA la
     * instruccion, sin que el proceso llegue a enterarse de nada.
     *
     * EC 0x24 es un data abort desde EL0, y los codigos 0b0001xx del ISS
     * son "fallo de traduccion" (el nivel va en los dos bits de abajo).
     * Un fallo de PERMISOS no entra aqui: ese si es una violacion. */
    if (index == 8) {
        uint64_t ec  = (f->esr >> 26) & 0x3F;
        uint64_t iss = f->esr & 0x3F;

        if (ec == 0x24 && (iss & 0x3C) == 0x04) {
            if (task_grow_stack(read_far(), f->sp_el0)) {
                /* Solo se cuentan las primeras. Un programa que se come un
                 * megabyte de pila no necesita doscientas cincuenta lineas
                 * diciendoselo: necesita ver el fallo del final. */
                uint64_t pags = task_stack_pages(current);
                if (pags <= 8) {
                    uint64_t lf = uart_begin();
                    uart_puts("  [kernel] la pila de ");
                    uart_puts(current->name);
                    uart_puts(" crece a ");
                    uart_dec(pags);
                    uart_puts(" paginas\n");
                    uart_end(lf);
                }
                return;                      /* a reintentar la instruccion */
            }
        }
    }

    /* Fallo dentro de un proceso de usuario: culpa suya, no nuestra. */
    if (index == 8 || index == 12) {
        dump(f, index);
        uart_puts("\n  [kernel] el proceso ");
        uart_puts(current ? current->name : "?");
        uart_puts(" ha violado la ley. Lo mato y sigo.\n");
        task_exit();
        return;
    }

    dump(f, index);
    panic("excepcion no manejada");
}

void exception_init(void)
{
    /* VBAR_EL1 = "Vector Base Address Register": donde vive nuestra tabla. */
    __asm__ volatile("msr vbar_el1, %0" :: "r"(vector_table));
    __asm__ volatile("isb");   /* barrera: que la CPU lo vea ya */
}
