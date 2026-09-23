/* fpu.h - Coma flotante con cambio de contexto perezoso
 *
 * LA IDEA. Los registros de FP son 528 bytes; el contexto entero de un
 * hilo son 104. Salvarlos en cada cambio de contexto multiplicaria por
 * cinco lo que cuesta cambiar de hilo, y la mayoria de los hilos -el
 * planificador, el servidor de ficheros, el shell- no tocan la FPU jamas.
 *
 * Asi que no se salvan. Se APAGA la FPU (CPACR_EL1.FPEN = 0) y se espera a
 * que alguien la quiera. Quien la quiera provoca una excepcion, y ahi, y
 * solo ahi, se le da: se le reserva su area, se le restaura lo que tuviera
 * y se le enciende. Un proceso que nunca hace una multiplicacion en coma
 * flotante no paga ni un ciclo ni un byte.
 *
 * Es el mismo patron de la pila que crece, de copy-on-write y de las
 * paginas bajo demanda: el fallo no es un error, es el aviso de que ha
 * llegado el momento de hacer el trabajo. Lo distinto es que aqui lo que
 * se difiere no es memoria, es estado de registros.
 *
 * POR QUE SE SALVA AL SALIR Y SOLO SE RESTAURA AL ENTRAR. La version de
 * libro es perezosa por los dos lados: al cambiar de hilo no se salva
 * tampoco, se deja el estado en los registros, y si el hilo vuelve sin que
 * nadie los haya tocado se ahorra hasta la restauracion. Funciona en una
 * maquina de un solo nucleo.
 *
 * Aqui hay cuatro. Si el hilo A deja su estado en los registros del nucleo
 * 0 y luego lo planifican en el 1, el nucleo 1 no tiene forma de traerselo:
 * esta en unos registros que no son suyos, y sacarlo de ahi exige
 * interrumpir al nucleo 0 y pedirle que lo escriba. Se puede hacer, y los
 * kernels de verdad lo hacen, pero deja de ser una optimizacion barata.
 *
 * Aqui se salva al salir y se restaura perezosamente al entrar. Se conserva
 * lo que de verdad importa -quien no usa la FPU no paga nada- y no hay
 * estado vivo fuera de su propietario cuando un hilo cambia de nucleo.
 */
#pragma once
#include <stdint.h>

#define FP_STATE_SIZE  528     /* 32 x 16 de q0-q31, mas FPSR y FPCR */

struct task;

void fp_save(void *dst);            /* fpu.S */
void fp_restore(const void *src);   /* fpu.S */
void fp_hw_enable(void);            /* fpu.S: CPACR_EL1.FPEN = 0b11 */
void fp_hw_disable(void);           /* fpu.S: CPACR_EL1.FPEN = 0b00 */

/* Alguien ha tocado la FPU teniendola apagada. Devuelve 1 si se le ha
 * podido dar, 0 si no habia memoria para su area. */
int  fp_trap(void);

/* Al dejar la CPU: si este hilo tenia la FPU encendida, guardar y apagar. */
void fp_switch_out(struct task *t);

/* Reservarle el area a un hilo sin que haya atrapado: lo usa el fork, que
 * se la copia del padre. Devuelve 0 si no hay memoria. */
int  fp_area_alloc(struct task *t);

/* Soltarla: al morir, y tambien en exec, donde el programa nuevo no tiene
 * por que ver los registros del anterior. */
void fp_release(struct task *t);
void fp_stats(void);                /* para el menu */
