/* user/syscall.h - Biblioteca minima de espacio de usuario
 *
 * Un proceso no puede llamar a ninguna funcion del kernel: no comparten ni
 * enlazado ni privilegios. Lo unico que los une es la instruccion 'svc',
 * que provoca una excepcion sincrona y hace que la CPU entre en EL1 por el
 * vector +0x400. Estos envoltorios son toda la "libc" que hay.
 */
#pragma once
#include "ipc_abi.h"
#include "fs_abi.h"        /* el contrato compartido con el kernel */

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

static inline int64_t syscall4(uint64_t nr, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                     : "memory", "cc");
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

/* --- Deshacer el truco del valor negativo ----------------------------
 *
 * El kernel mete el motivo del fallo en el propio valor devuelto, entre
 * -4095 y -1. Aqui se separan otra vez: el motivo va a errno y la
 * funcion devuelve -1, que es lo que espera cualquier programa escrito
 * para un Unix.
 *
 * El rango no es magia: ningun resultado legitimo -un tamanyo, un
 * descriptor, una posicion- es negativo, asi que no hay ambiguedad. */
extern int errno;

static inline int64_t revisar(int64_t r)
{
    if (r < 0 && r >= -4095) { errno = (int)-r; return -1; }
    return r;
}

/* --- Llamadas basicas -------------------------------------------------- */
/* --- Descriptores ------------------------------------------------------
 * El 0 es la entrada y el 1 la salida, por costumbre. Quien arranca el
 * programa puede ponerles lo que quiera, y el programa no se entera. */
/* Las dos pasan por revisar(), como todo lo demas desde el paso 48.
 *
 * Eran las unicas que no, y se noto en cuanto alguien pregunto POR QUE:
 * el kernel decia -EINTR y aqui llegaba tal cual, asi que quien miraba
 * errno veia lo que hubiera de antes. Un read interrumpido por una senyal
 * y una entrada que se acaba son cosas distintas y tienen que contestar
 * cosas distintas. */
static inline int64_t write(int fd, const void *buf, uint64_t n)
{ return revisar(syscall3(SYS_write, (uint64_t)fd, (uint64_t)buf, n)); }

static inline int64_t read(int fd, void *buf, uint64_t n)
{ return revisar(syscall3(SYS_read, (uint64_t)fd, (uint64_t)buf, n)); }

static inline int64_t pipe(int fds[2])
{ return syscall2(SYS_pipe, (uint64_t)fds, 0); }

static inline int64_t closefd(int fd)
{ return syscall2(SYS_close, (uint64_t)fd, 0); }

static inline int64_t dup2(int viejo, int nuevo)
{ return syscall2(SYS_dup2, (uint64_t)viejo, (uint64_t)nuevo); }

/* Lo que antes estaba aqui -kprint, kgetc, exit, malloc, ustrlen, ucopy,
 * udec- se ha ido a la libc, que es donde le tocaba: <stdio.h>,
 * <stdlib.h> y <string.h>. Este fichero vuelve a ser lo que decia su
 * nombre, la frontera con el kernel y nada mas. */

/* Paginas de 4 KB libres en todo el sistema. */
static inline uint64_t freepages(void) { return (uint64_t)syscall2(SYS_freepages, 0, 0); }

/* --- Senyales (lib/signal.c) ------------------------------------------
 * signal() registra que hacer cuando llegue una; 0 vuelve a la accion por
 * defecto. SIGKILL no se puede atrapar. */
int signal(int sig, void (*manejador)(int));

/* Lo mismo, diciendo COMO se quiere que llegue. Es el sigaction del pobre,
 * y existe por SIG_REANUDAR: la unica bandera que hay, la unica que hace
 * falta, y la unica forma de que SIGCHLD no rompa lo que el proceso
 * estuviera haciendo mientras su hijo terminaba. */
int signal_banderas(int sig, void (*manejador)(int), int banderas);

static inline int64_t kill(uint64_t pid, int sig)
{ return syscall2(SYS_kill, pid, (uint64_t)sig); }

/* Convertirse en otro programa. Si sale bien no vuelve; si vuelve, fallo.
 *
 * argv es un array de punteros terminado en cero, como el que recibe
 * main(). El kernel lo COPIA; no lo parte. Quien trocea la linea es el
 * shell, que es su trabajo. */
static inline int64_t exec(const void *imagen, uint64_t bytes,
                           char *const argv[], char *const envp[])
{ return syscall4(SYS_exec, (uint64_t)imagen, bytes,
                  (uint64_t)argv, (uint64_t)envp); }

/* Duplicarse. Devuelve el pid del hijo al padre, y 0 al hijo. */
static inline int64_t fork(void)          { return syscall2(SYS_fork, 0, 0); }

/* Pedirle memoria al kernel moviendo el tope del monton. Devuelve el tope
 * VIEJO: el principio de lo que acabas de conseguir. Con delta negativo,
 * la devuelve. */
static inline void *sbrk(int64_t delta)
{ return (void *)(uint64_t)syscall2(SYS_sbrk, (uint64_t)delta, 0); }

/* --- Esperar a un hijo, la completa -----------------------------------
 *
 * Devuelve el PID de aquel del que hay noticias, o un errno negativo. Las
 * otras dos respuestas van por puntero, y los dos se pueden dejar a cero si
 * no interesan:
 *
 *   que    -> W_SALIDA (termino) o W_PARADO (sigue ahi, detenido)
 *   codigo -> con W_SALIDA, lo que devolvio; con W_PARADO, la senyal que lo
 *             paro
 *
 * Con PID_CUALQUIERA vale cualquier hijo, y es ahora cuando eso sirve para
 * algo: mientras el pid no se devolvia, "cualquiera" solo se podia usar para
 * enterrar, porque no habia forma de saber de quien eran las noticias. */
static inline int64_t esperar(int64_t pid, int *que, int64_t *codigo,
                              int banderas)
{ return syscall4(SYS_waitpid, (uint64_t)pid, (uint64_t)banderas,
                  (uint64_t)que, (uint64_t)codigo); }

/* Bloquear hasta que ese hijo termine, sin preguntar nada mas. Es lo que
 * quiere quien se bifurco para hacer una cosa y espera a que este hecha. */
static inline int64_t waitpid(int64_t pid)
{ return esperar(pid, 0, 0, 0); }

/* Preguntar sin quedarse esperando: -EAGAIN si no hay nada que contar,
 * -ECHILD si no hay hijos. Es lo que necesita un shell para enterarse de que
 * un trabajo de segundo plano ha cambiado sin bloquearse en el, y de paso
 * para recogerlo: un hijo que nadie espera se queda de zombi. */
static inline int64_t waitpid_ya(int64_t pid, int *que, int64_t *codigo)
{ return esperar(pid, que, codigo, WNOHANG | WUNTRACED); }

/* El grupo, o sea el trabajo. Con pid 0 se refiere a uno mismo, y con
 * pgid 0 el grupo pasa a llamarse como el propio pid: "formo el mio". */
static inline int64_t setpgid(uint64_t pid, uint64_t pgid)
{ return syscall2(SYS_setpgid, pid, pgid); }

static inline int64_t getpgid(uint64_t pid)
{ return syscall2(SYS_getpgid, pid, 0); }
static inline void yield(void)            { syscall2(SYS_yield, 0, 0); }
static inline uint64_t getpid(void)       { return (uint64_t)syscall2(SYS_getpid, 0, 0); }
static inline void sleep(uint64_t ticks)  { syscall2(SYS_sleep, ticks, 0); }
static inline uint64_t uptime(void)       { return (uint64_t)syscall2(SYS_uptime, 0, 0); }

/* --- Paso de mensajes -------------------------------------------------- */
/* 'want' es el puerto que se quiere, o -1 para el primero libre. */
static inline int64_t port_create(int64_t want)
{ return syscall2(SYS_port_create, (uint64_t)want, 0); }
static inline int64_t alarma(uint64_t port, uint64_t cada)
                                          { return syscall2(SYS_alarma, port, cada); }
static inline int64_t msg_send(uint64_t port, struct message *m)
                                          { return syscall2(SYS_send, port, (uint64_t)m); }
static inline int64_t msg_recv(uint64_t port, struct message *m)
                                          { return syscall2(SYS_recv, port, (uint64_t)m); }

/* --- MMIO concedido (solo para drivers) -------------------------------- */
static inline uint64_t mmio_base(void)    { return (uint64_t)syscall2(SYS_mmio_base, 0, 0); }

/* --- El directorio actual ---------------------------------------------
 * realpath() une el cwd con una ruta relativa y la normaliza. La hace el
 * KERNEL, no la libc, y eso es deliberado: el kernel la necesita igual
 * para la redireccion, y dos implementaciones acabarian discrepando en
 * algun caso raro. Ver src/path.c. */
static inline int64_t chdir(const char *ruta)
                                          { return revisar(syscall2(SYS_chdir, (uint64_t)ruta, 0)); }
static inline int64_t getcwd(char *buf, uint64_t n)
                                          { return syscall2(SYS_getcwd, (uint64_t)buf, n); }
static inline int64_t realpath(const char *ruta, char *salida)
                                          { return syscall2(SYS_realpath, (uint64_t)ruta,
                                                            (uint64_t)salida); }

/* --- Arrancar lo empotrado, y repartir la consola ---------------------
 *
 * bootstrap sigue siendo solo de init: es la unica forma de llegar a los
 * programas que el kernel lleva dentro.
 *
 * consola() ya no. Poner un grupo en primer plano lo puede hacer init o
 * cualquiera cuyo grupo la tenga ya, que es la regla que necesita un
 * shell para ceder la consola al trabajo que arranca y recuperarla
 * cuando termina. */
static inline int64_t bootstrap(const char *nombre, char *const argv[],
                                char *const envp[], uint64_t dispositivo)
{
    return syscall4(SYS_bootstrap, (uint64_t)nombre, (uint64_t)argv,
                    (uint64_t)envp, dispositivo);
}

static inline int64_t consola(uint64_t pgid)
{ return syscall2(SYS_consola, pgid, 0); }

/* Copiar una cadena con tope. Hace falta en varios programas. */
static inline void ucopiar(char *dst, const char *src, uint64_t max)
{
    uint64_t i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* --- Ficheros, como los espera cualquier libc ------------------------
 * Debajo siguen estando los mensajes al servidor; lo que cambia es que ya
 * no hay que conocerlos para usar un fichero. */
static inline int64_t stat(const char *ruta, struct estado *e)
{ return revisar(syscall2(SYS_stat, (uint64_t)ruta, (uint64_t)e)); }

static inline int64_t lseek(int fd, int64_t desp, int desde)
{ return revisar(syscall3(SYS_lseek, (uint64_t)fd, (uint64_t)desp, (uint64_t)desde)); }

static inline int64_t unlink(const char *ruta)
{ return revisar(syscall2(SYS_unlink, (uint64_t)ruta, 0)); }

static inline int64_t mkdir(const char *ruta)
{ return revisar(syscall2(SYS_mkdir, (uint64_t)ruta, 0)); }

static inline int64_t rmdir(const char *ruta)
{ return revisar(syscall2(SYS_rmdir, (uint64_t)ruta, 0)); }

static inline int64_t rename(const char *origen, const char *destino)
{ return revisar(syscall2(SYS_rename, (uint64_t)origen, (uint64_t)destino)); }

static inline int64_t opendir(const char *ruta)
{ return revisar(syscall2(SYS_opendir, (uint64_t)ruta, 0)); }

/* 1 si hay entrada, 0 si se acabo, -1 si algo fue mal. */
static inline int64_t readdir(int fd, struct fs_info *info)
{ return revisar(syscall2(SYS_readdir, (uint64_t)fd, (uint64_t)info)); }

/* La hora, en segundos desde 1970. Ver SYS_time: en esta maquina es un
 * invento honesto, no un reloj. */
static inline uint64_t ahora(void)  { return (uint64_t)syscall2(SYS_time, 0, 0); }
static inline int64_t poner_hora(uint64_t s) { return syscall2(SYS_settime, s, 0); }

/* Mapear un fichero: convertirlo en un puntero. No lo lee ahora; los
 * trozos van llegando segun se tocan. */
static inline const char *mmap(const char *ruta, uint64_t *tam)
{
    int64_t r = syscall2(SYS_mmap, (uint64_t)ruta, (uint64_t)tam);
    return r < 0 ? 0 : (const char *)(uint64_t)r;
}

/* Memoria nueva, a cero y escribible. No la lee de ningun sitio: las
 * paginas aparecen cuando se tocan. */
static inline void *mmap_anon(uint64_t bytes)
{
    int64_t r = syscall2(SYS_mmap_anon, bytes, 0);
    return r < 0 ? 0 : (void *)(uint64_t)r;
}

static inline int64_t munmap(const char *p)
                                          { return syscall2(SYS_munmap, (uint64_t)p, 0); }

/* Abrir un fichero de la tarjeta y quedarselo en un descriptor. */
static inline int64_t openf(const char *nombre, uint64_t modo)
                                          { return revisar(syscall2(SYS_open, (uint64_t)nombre, modo)); }

/* Para escribir un driver: pedir que una interrupcion llegue como mensaje,
 * devolverla cuando ya esta atendida, y entregarle al kernel las teclas. */
static inline int64_t irq_register(uint64_t irq, uint64_t puerto)
                                          { return syscall2(SYS_irq_register, irq, puerto); }
static inline int64_t irq_ack(uint64_t irq)
                                          { return syscall2(SYS_irq_ack, irq, 0); }
static inline int64_t console_push(const char *b, uint64_t n)
                                          { return syscall2(SYS_console_push, (uint64_t)b, n); }
static inline int64_t console_int(void)   { return syscall2(SYS_console_int, 0, 0); }
static inline
int64_t console_stop(void)  { return syscall2(SYS_console_stop, 0, 0); }

/* El modo del terminal. Devuelve el que habia, asi que guardar y reponer
 * es el mismo par de llamadas:
 *
 *     int antes = termios(termios(-1) & ~T_ECO);
 *     ... pedir la contrasenya ...
 *     termios(antes);
 */
/* Un tramo de memoria para DMA: paginas SEGUIDAS y sin cachear. Devuelve la
 * direccion virtual y deja la fisica en *pa, que es la que hay que darle al
 * periferico. Solo para drivers: un proceso normal recibe EPERM.
 *
 * Uno por proceso. Si un driver necesita varios buffers, reparte el suyo,
 * que ademas es como funciona un "DMA pool" de verdad. */
/* Sacar texto del anillo del kernel. Solo lo puede llamar el duenyo de la
 * UART, que desde el paso 59 es el UNICO que escribe en ella: el kernel le
 * cede el hardware al reclamarla y le pasa su texto por aqui. Devuelve los
 * bytes copiados, o 0 si no habia nada. */
static inline int64_t klog(char *buf, uint64_t n)
{ return syscall2(SYS_klog, (uint64_t)buf, n); }

/* Pedirle a la GPU que encienda un dispositivo de la placa y espere a que este
 * listo. Devuelve 1 si quedo encendido. Solo para drivers. */
static inline int64_t dev_power(uint64_t dispositivo)
{ return syscall2(SYS_dev_power, dispositivo, 0); }

static inline int64_t dma_alloc(uint64_t paginas, uint64_t *pa)
{ return revisar(syscall2(SYS_dma_alloc, paginas, (uint64_t)pa)); }

static inline int64_t termios(int modo)
{ return syscall2(SYS_termios, (uint64_t)(int64_t)modo, 0); }

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

static inline int64_t spawn(const void *imagen, uint64_t bytes,
                            char *const argv[], char *const envp[])
{ return syscall4(SYS_spawn, (uint64_t)imagen, bytes,
                  (uint64_t)argv, (uint64_t)envp); }

