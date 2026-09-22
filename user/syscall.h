/* user/syscall.h - Biblioteca minima de espacio de usuario
 *
 * Un proceso no puede llamar a ninguna funcion del kernel: no comparten ni
 * enlazado ni privilegios. Lo unico que los une es la instruccion 'svc',
 * que provoca una excepcion sincrona y hace que la CPU entre en EL1 por el
 * vector +0x400. Estos envoltorios son toda la "libc" que hay.
 */
#pragma once

typedef unsigned long uint64_t;
typedef long          int64_t;

#define SYS_write   1
#define SYS_exit    2
#define SYS_yield   3
#define SYS_getpid  4
#define SYS_sleep   5
#define SYS_uptime  6

static inline int64_t syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
    return (int64_t)x0;
}

static inline uint64_t ustrlen(const char *s)
{
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline void print(const char *s)  { syscall2(SYS_write, (uint64_t)s, ustrlen(s)); }
static inline void exit(int code)        { syscall2(SYS_exit, (uint64_t)code, 0); }
static inline void yield(void)           { syscall2(SYS_yield, 0, 0); }
static inline uint64_t getpid(void)      { return (uint64_t)syscall2(SYS_getpid, 0, 0); }
static inline void sleep(uint64_t ticks) { syscall2(SYS_sleep, ticks, 0); }
static inline uint64_t uptime(void)      { return (uint64_t)syscall2(SYS_uptime, 0, 0); }

static inline void print_dec(uint64_t v)
{
    char buf[21];
    int n = 0;
    if (v == 0) { print("0"); return; }
    while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    char out[22];
    int i = 0;
    while (n--) out[i++] = buf[n];
    out[i] = 0;
    print(out);
}
