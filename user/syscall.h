/* user/syscall.h - Biblioteca minima de espacio de usuario
 *
 * Un proceso no puede llamar a ninguna funcion del kernel: no comparten ni
 * enlazado ni privilegios. Lo unico que los une es la instruccion 'svc',
 * que provoca una excepcion sincrona y hace que la CPU entre en EL1 por el
 * vector +0x400. Estos envoltorios son toda la "libc" que hay.
 */
#pragma once
#include "ipc_abi.h"        /* el contrato compartido con el kernel */

typedef unsigned long uint64_t;
typedef long          int64_t;

static inline int64_t syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
    return (int64_t)x0;
}

static inline int64_t syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8)
                     : "memory", "cc");
    return (int64_t)x0;
}

static inline uint64_t ustrlen(const char *s)
{
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

/* --- Llamadas basicas -------------------------------------------------- */
static inline void kprint(const char *s)  { syscall2(SYS_write, (uint64_t)s, ustrlen(s)); }
static inline void exit(int code)         { syscall2(SYS_exit, (uint64_t)code, 0); }

/* Un caracter de la consola. Bloquea hasta que llegue. */
static inline char kgetc(void)            { return (char)syscall2(SYS_read, 0, 0); }

/* --- El monton del proceso (user/umalloc.c) ---------------------------
 * Encima de sbrk(), el mismo asignador que usa el kernel para el suyo. */
void *malloc(uint64_t n);
void  free(void *p);

/* Paginas de 4 KB libres en todo el sistema. */
static inline uint64_t freepages(void) { return (uint64_t)syscall2(SYS_freepages, 0, 0); }

/* Duplicarse. Devuelve el pid del hijo al padre, y 0 al hijo. */
static inline int64_t fork(void)          { return syscall2(SYS_fork, 0, 0); }

/* Pedirle memoria al kernel moviendo el tope del monton. Devuelve el tope
 * VIEJO: el principio de lo que acabas de conseguir. Con delta negativo,
 * la devuelve. */
static inline void *sbrk(int64_t delta)
{ return (void *)(uint64_t)syscall2(SYS_sbrk, (uint64_t)delta, 0); }

/* Esperar a que termine un proceso. Vuelve enseguida si ya no existe. */
static inline void waitpid(uint64_t pid)  { syscall2(SYS_waitpid, pid, 0); }
static inline void yield(void)            { syscall2(SYS_yield, 0, 0); }
static inline uint64_t getpid(void)       { return (uint64_t)syscall2(SYS_getpid, 0, 0); }
static inline void sleep(uint64_t ticks)  { syscall2(SYS_sleep, ticks, 0); }
static inline uint64_t uptime(void)       { return (uint64_t)syscall2(SYS_uptime, 0, 0); }

/* --- Paso de mensajes -------------------------------------------------- */
/* 'want' es el puerto que se quiere, o -1 para el primero libre. */
static inline int64_t port_create(int64_t want)
{ return syscall2(SYS_port_create, (uint64_t)want, 0); }
static inline int64_t msg_send(uint64_t port, struct message *m)
                                          { return syscall2(SYS_send, port, (uint64_t)m); }
static inline int64_t msg_recv(uint64_t port, struct message *m)
                                          { return syscall2(SYS_recv, port, (uint64_t)m); }

/* --- MMIO concedido (solo para drivers) -------------------------------- */
static inline uint64_t mmio_base(void)    { return (uint64_t)syscall2(SYS_mmio_base, 0, 0); }

/* Crear un proceso a partir de una imagen que tenemos en memoria.
 *
 * Que esto sea una llamada al sistema y no una funcion del kernel es lo
 * que permite que el kernel NO sepa leer ficheros: quien quiera arrancar un
 * programa que lo lea del servidor de ficheros y pase los bytes. El kernel
 * solo sabe convertir bytes en proceso. */
/* A que velocidad va un reloj de la placa. Lo sabe la GPU y lo pregunta el
 * kernel por nosotros: ver SYS_clock_rate. */
static inline uint64_t clock_rate(uint64_t id)
{ int64_t r = syscall2(SYS_clock_rate, id, 0); return r < 0 ? 0 : (uint64_t)r; }

static inline int64_t spawn(const void *imagen, uint64_t bytes, const char *args)
{ return syscall3(SYS_spawn, (uint64_t)imagen, bytes, (uint64_t)args); }

/* --- Utilidades sin libc ----------------------------------------------- */
static inline void ucopy(char *dst, const char *src, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) dst[i] = src[i];
}

static inline uint64_t udec(char *out, uint64_t v)
{
    char tmp[21];
    uint64_t n = 0, i = 0;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) out[i++] = tmp[--n];
    return i;
}
