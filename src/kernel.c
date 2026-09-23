/* kernel.c - Nucleo de TinyOS */
#include <stdint.h>
#include "uart.h"
#include "exception.h"
#include "irq.h"
#include "timer.h"
#include "mm.h"
#include "mmio.h"
#include "mbox.h"
#include "smp.h"
#include "sched.h"
#include "sync.h"
#include "ipc.h"

#define TICK_HZ      100

/* La imagen del proceso de usuario, empotrada por tools/bin2c.py. Cuando
 * haya un sistema de ficheros, esto sera una carga de verdad desde disco. */
extern const uint8_t  user_hello[];      extern const uint64_t user_hello_size;
extern const uint8_t  user_conserver[];  extern const uint64_t user_conserver_size;
extern const uint8_t  user_client[];     extern const uint64_t user_client_size;

/* Registros de la PL011. Se los concedemos al driver de consola para que
 * pueda hacer su trabajo desde EL0 sin pasar por el kernel. Aqui hace falta
 * la direccion FISICA (es la que se va a meter en una tabla de paginas),
 * no la virtual por la que los ve el kernel. */
#define UART0_PHYS  (PERIPHERAL_PA + 0x201000)

/* Ventana virtual para los experimentos, dentro del espacio del kernel pero
 * fuera del mapa lineal: ahi no hay nada fisico. Que funcione es justamente
 * la demostracion de que la traduccion existe. */
#define TEST_VA_A    (KERNEL_VA_BASE + 0xC0000000UL)
#define TEST_VA_B    (KERNEL_VA_BASE + 0xC0001000UL)
#define TEST_VA_RO   (KERNEL_VA_BASE + 0xC0010000UL)

/* Area para el benchmark de memoria: 256 KB en .bss */
#define BENCH_WORDS  (256 * 1024 / 8)
/* 'volatile': sin el, el compilador ve que nadie escribe aqui, deduce que
 * son ceros y borra el bucle de medida entero. */
static volatile uint64_t bench_area[BENCH_WORDS];

static uint64_t current_el(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return el >> 2;
}

static int mmu_is_on(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (int)(sctlr & 1);
}

static int caches_are_on(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (int)((sctlr >> 2) & 1);
}

/* Recorre 256 KB varias veces y devuelve los microsegundos que ha costado.
 * Con la MMU apagada no hay caches, asi que cada lectura va a la RAM. */
static uint64_t bench_memory(void)
{
    uint64_t flags = irq_save();       /* que los ticks no falseen la medida */
    uint64_t sum = 0;
    uint64_t t0 = timer_now();

    for (int pass = 0; pass < 16; pass++)
        for (uint64_t i = 0; i < BENCH_WORDS; i++)
            sum += bench_area[i];

    uint64_t elapsed = timer_now() - t0;
    irq_restore(flags);

    __asm__ volatile("" :: "r"(sum));  /* que el compilador no borre el bucle */
    return timer_us(elapsed);
}

static void show_translation(const char *what, uint64_t va)
{
    uint64_t lf = uart_begin();
    uart_puts("   ");
    uart_puts(what);
    uart_puts("  VA 0x");
    uart_hex64(va);
    uart_puts(" -> PA 0x");
    uint64_t pa = vmm_translate(va);
    if (pa) uart_hex64(pa);
    else    uart_puts("(sin traduccion)");
    uart_puts("\n");
    uart_end(lf);
}

static void mem_stats(void)
{
    uart_puts("\n  Memoria fisica:\n    total : ");
    uart_dec(pmm_total_pages());
    uart_puts(" paginas (");
    uart_dec(pmm_total_pages() * PAGE_SIZE / 1024 / 1024);
    uart_puts(" MB)\n    usadas: ");
    uart_dec(pmm_used_pages());
    uart_puts("\n    libres: ");
    uart_dec(pmm_free_pages());
    uart_puts(" (");
    uart_dec(pmm_free_pages() * PAGE_SIZE / 1024 / 1024);
    uart_puts(" MB)\n");
}

/* Dos direcciones virtuales distintas apuntando a la MISMA pagina fisica. */
static void demo_aliasing(void)
{
    uint64_t pa = pmm_alloc();
    if (!pa) { uart_puts("sin memoria\n"); return; }

    uart_puts("\n  Pagina fisica obtenida: 0x");
    uart_hex64(pa);
    uart_puts("\n  La mapeo en DOS direcciones virtuales distintas:\n");

    vmm_map_page(TEST_VA_A, pa, MM_RAM_RW);
    vmm_map_page(TEST_VA_B, pa, MM_RAM_RW);
    show_translation("A:", TEST_VA_A);
    show_translation("B:", TEST_VA_B);

    volatile uint64_t *a = (volatile uint64_t *)TEST_VA_A;
    volatile uint64_t *b = (volatile uint64_t *)TEST_VA_B;

    *a = 0xCAFEBABEDEADBEEFUL;
    uart_puts("  Escribo 0xCAFEBABEDEADBEEF en A... y leo B: 0x");
    uart_hex64(*b);
    uart_puts(*b == 0xCAFEBABEDEADBEEFUL ? "  <- misma pagina\n" : "  <- ERROR\n");

    *b = 0x1111222233334444UL;
    uart_puts("  Escribo 0x1111222233334444 en B... y leo A: 0x");
    uart_hex64(*a);
    uart_puts("\n");
}

/* Cambiar los permisos de una pagina ya mapeada. */
static void demo_readonly(void)
{
    uint64_t pa = pmm_alloc();
    if (!pa) { uart_puts("sin memoria\n"); return; }

    volatile uint64_t *p = (volatile uint64_t *)TEST_VA_RO;

    vmm_map_page(TEST_VA_RO, pa, MM_RAM_RW);
    *p = 0x5A5A5A5A;
    uart_puts("\n  Mapeada RW: escribo 0x5A5A5A5A y leo 0x");
    uart_hex64(*p);
    uart_puts("  OK\n");

    vmm_map_page(TEST_VA_RO, pa, MM_RAM_RO);
    uart_puts("  Remapeada RO (misma pagina fisica): leo 0x");
    uart_hex64(*p);
    uart_puts("  sigue OK\n");
    uart_puts("  Ahora escribo... deberia saltar un fallo de permisos:\n");
    *p = 0;
    uart_puts("  !!! No ha saltado: la proteccion NO funciona\n");
}

/* La UART es un recurso compartido y uart_puts no es atomica: si el timer
 * desaloja a un hilo a mitad de una frase, la siguiente se mete por medio.
 *
 * Antes esto se resolvia con irq_save(): tapar el hardware entero del
 * sistema para que dos hilos no se pisaran una cadena de texto. Ahora se
 * hace con un mutex, que es lo proporcionado: el hilo que llega segundo se
 * duerme y deja la CPU libre, y las interrupciones siguen entrando. */
static struct mutex console;

/* syscall.c tambien imprime, y tiene que compartir el mismo mutex. */
struct mutex *console_mutex(void) { return &console; }

/* Los hilos de demostracion de los pasos 5 y 6 siguen trabajando siempre,
 * pero callados. Cuando eran lo unico que habia, su cháchara ERA la
 * demostracion; ahora tapan todo lo demas. El comando 'd' los hace hablar.
 *
 * Lo que siguen enseñando sin decir palabra: 'l' que estan vivos y en que
 * nucleo, 'c' el contador compartido, 'k' el canal productor/consumidor. */
static volatile int verboso;

/* El mutex serializa los HILOS; el uart_begin/end serializa los NUCLEOS.
 * No es lo mismo ni sobra ninguno: dos hilos nunca entran a la vez aqui
 * gracias al mutex, pero el kernel tambien escribe desde sitios que no son
 * hilos -un manejador de interrupcion, por ejemplo- y contra eso el mutex
 * no puede hacer nada. */
static void say(const char *who, const char *what, uint64_t n)
{
    if (!verboso) return;

    mutex_lock(&console);
    uint64_t f = uart_begin();
    uart_puts("    [");
    uart_puts(who);
    uart_puts("] ");
    uart_puts(what);
    uart_dec(n);
    uart_puts("   (tick ");
    uart_dec(timer_ticks());
    uart_puts(")\n");
    uart_end(f);
    mutex_unlock(&console);
}

/* Hilo que duerme: la mayor parte del tiempo no consume CPU. */
static void thread_ticker(void *arg)
{
    uint64_t period = (uint64_t)arg;
    for (uint64_t i = 1; ; i++) {
        say(current->name, "latido ", i);
        task_sleep(period);
    }
}

/* Hilo que quema CPU: nunca cede voluntariamente. Solo el timer puede
 * quitarselo de encima, y ahi se ve si la expropiacion funciona de verdad. */
static void thread_cruncher(void *arg)
{
    (void)arg;
    uint64_t rounds = 0;
    volatile uint64_t sum = 0;
    for (;;) {
        for (int i = 0; i < 200000; i++)
            sum += (uint64_t)i;
        rounds++;
        __asm__ volatile("" :: "r"(sum));   /* que el bucle no se optimice */
        if (rounds % 2000 == 0)
            say(current->name, "vueltas ", rounds);
    }
}

/* Hilo que termina: demuestra que task_exit deja un zombi. */
static void thread_shortlived(void *arg)
{
    (void)arg;
    say(current->name, "nazco y muero tras dormir 3 s, tarea ", current->pid);
    task_sleep(300);
    say(current->name, "me voy. pid ", current->pid);
}

static void command(char c);

/* La consola vive en su propio hilo. Antes estaba en la tarea idle, pero la
 * idle solo corre cuando nadie mas quiere CPU: con un hilo como 'crunch' en
 * marcha, la consola no habria respondido jamas. */
static void thread_shell(void *arg)
{
    (void)arg;
    /* Ya no hay polling: el hilo se bloquea en la cola de espera de la UART
     * y la interrupcion de recepcion lo despierta. Latencia minima y cero
     * CPU consumida mientras no escribes. */
    for (;;)
        command(uart_getc_blocking());
}

/* ---------------- Demostracion 1: la carrera de datos ----------------
 * Dos hilos incrementan el mismo contador. La operacion "leer, sumar,
 * escribir" NO es atomica: si el timer desaloja al hilo entre la lectura y
 * la escritura, el otro lee el mismo valor y uno de los dos incrementos se
 * evapora. El bucle de en medio ensancha esa ventana para que se vea.
 */
static volatile uint64_t shared_counter;   /* lo que vale de verdad     */
static volatile uint64_t attempted;        /* lo que deberia valer      */
static struct mutex      counter_mutex;
static volatile int      use_mutex = 1;

static void thread_incrementer(void *arg)
{
    (void)arg;
    for (;;) {
        /* Sin el mutex, los dos hilos leen el mismo valor y escriben el
         * mismo resultado: dos incrementos, uno solo cuenta. */
        if (use_mutex) mutex_lock(&counter_mutex);

        uint64_t v = shared_counter;

        /* Ceder aqui fuerza el peor caso en cada vuelta. Esperar a que el
         * timer caiga por azar justo entre la lectura y la escritura
         * tambien funciona, pero es un suceso de 1 entre mil: la carrera
         * apareceria a las horas, que es exactamente lo que hace a estos
         * bugs tan dificiles de encontrar. */
        task_yield();

        shared_counter = v + 1;

        if (use_mutex) mutex_unlock(&counter_mutex);

        /* 'attempted' tambien es una seccion critica, pero esta la
         * protegemos siempre: es nuestra vara de medir. */
        uint64_t f = irq_save();
        attempted++;
        irq_restore(f);

        task_sleep(1);
    }
}

/* ------------- Demostracion 2: productor / consumidor ----------------
 * Un canal de 8 huecos. El productor va mas rapido que el consumidor, asi
 * que se llena y el productor acaba bloqueado esperando sitio: control de
 * flujo gratis, sin que ninguno de los dos sepa nada del otro.
 */
static struct channel pipe_chan;

static void thread_producer(void *arg)
{
    (void)arg;
    for (uint64_t n = 1; ; n++) {
        chan_send(&pipe_chan, n * n);
        task_sleep(15);
    }
}

static void thread_consumer(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t msg = chan_recv(&pipe_chan);
        say(current->name, "recibo ", msg);
        task_sleep(40);              /* consume mas despacio de lo que llega */
    }
}

static void race_stats(void)
{
    uint64_t f = irq_save();
    uint64_t got = shared_counter, want = attempted;
    irq_restore(f);

    uart_puts("\n  mutex: ");
    uart_puts(use_mutex ? "SI" : "NO");
    uart_puts("\n  incrementos intentados : ");
    uart_dec(want);
    uart_puts("\n  valor del contador     : ");
    uart_dec(got);
    uart_puts("\n  PERDIDOS               : ");
    uart_dec(want - got);
    uart_puts(want == got ? "  (ninguno)\n" : "  <-- carrera de datos\n");
}

static void chan_stats(void)
{
    uart_puts("\n  canal: ");
    uart_dec(pipe_chan.count);
    uart_puts("/");
    uart_dec(CHAN_CAPACITY);
    uart_puts(" ocupado   enviados: ");
    uart_dec(pipe_chan.sent);
    uart_puts("   recibidos: ");
    uart_dec(pipe_chan.received);
    uart_puts("\n");
}

static void menu(void)
{
    uart_puts("\nComandos:\n");
    uart_puts("  l - listar hilos\n");
    uart_puts("  c - contador compartido: intentos vs valor real\n");
    uart_puts("  m - activar/desactivar el mutex del contador\n");
    uart_puts("  k - estado del canal productor/consumidor\n");
    uart_puts("  u - lanzar el proceso 'hello' en EL0\n");
    uart_puts("  s - lanzar el SERVIDOR de consola (driver en EL0)\n");
    uart_puts("  n - lanzar un cliente que imprime por mensajes\n");
    uart_puts("  i - estado de los puertos IPC\n");
    uart_puts("  y - ceder la CPU (yield) desde la tarea idle\n");
    uart_puts("  p - estado de la memoria fisica\n");
    uart_puts("  v - dos direcciones virtuales, una pagina fisica\n");
    uart_puts("  r - pagina de solo lectura (FATAL: fallo de permisos)\n");
    uart_puts("  x - traducciones VA -> PA del kernel\n");
    uart_puts("  j - estado de los cuatro nucleos\n");
    uart_puts("  d - que los hilos de demostracion hablen (o se callen)\n");
    uart_puts("  w - los 4 nucleos contra un contador (con y sin cerrojo)\n");
    uart_puts("  b - medir velocidad de la memoria\n");
    uart_puts("  t - tiempo e interrupciones\n");
    uart_puts("  h - ayuda\n");
    uart_puts("  4 - direccion invalida (FATAL: data abort)\n");
    uart_puts("  resto: eco\n");
}

static void command(char c)
{
    switch (c) {
    case 'l':
        sched_dump();
        break;

    case 'c':
        race_stats();
        break;

    case 'm': {
        uint64_t f = irq_save();
        use_mutex = !use_mutex;
        shared_counter = 0;
        attempted = 0;
        irq_restore(f);
        uart_puts(use_mutex ? "\n  mutex ACTIVADO, contadores a cero\n"
                            : "\n  mutex DESACTIVADO, contadores a cero\n");
        break;
    }

    case 'k':
        chan_stats();
        break;

    case 's': {
        uart_puts("\n  [kernel] arrancando el driver de consola en EL0,\n");
        uart_puts("           con la pagina de la PL011 mapeada en su espacio\n");
        int pid = task_create_user("conserver", user_conserver,
                                   user_conserver_size, UART0_PHYS);
        if (pid < 0) uart_puts("  [kernel] no he podido crearlo\n");
        break;
    }

    case 'n': {
        int pid = task_create_user("client", user_client, user_client_size, 0);
        if (pid < 0) uart_puts("\n  [kernel] no he podido crearlo\n");
        else {
            uart_puts("\n  [kernel] cliente creado, pid ");
            uart_dec((uint64_t)pid);
            uart_puts("\n");
        }
        break;
    }

    case 'i':
        ipc_dump();
        break;

    case 'u': {
        uart_puts("\n  [kernel] cargando ");
        uart_dec(user_hello_size);
        uart_puts(" bytes en un espacio de direcciones nuevo...\n");
        int pid = task_create_user("hello", user_hello, user_hello_size, 0);
        if (pid < 0) uart_puts("  [kernel] no he podido crearlo\n");
        else {
            uart_puts("  [kernel] proceso creado, pid ");
            uart_dec((uint64_t)pid);
            uart_puts("\n");
        }
        break;
    }

    case 'y':
        uart_puts("\n  idle cede la CPU...\n");
        task_yield();
        uart_puts("  idle ha vuelto\n");
        break;

    case 'p':
        mem_stats();
        break;

    case 'v':
        demo_aliasing();
        break;

    case 'r':
        demo_readonly();
        break;

    case 'd':
        verboso = !verboso;
        uart_puts(verboso ? "\n  Los hilos hablan.\n"
                          : "\n  Los hilos se callan. Siguen trabajando.\n");
        break;

    case 'j':
        smp_dump();
        break;

    case 'w': {
        const uint64_t N = 200000;
        uart_puts("\n  Los cuatro nucleos suman ");
        uart_dec(N);
        uart_puts(" veces cada uno sobre el mismo contador.\n");
        uart_puts("  Esperado: ");
        uart_dec(N * CORES);
        uart_puts("\n");

        uint64_t r0 = smp_hammer(N, 0);
        uint64_t lf = uart_begin();
        uart_puts("\n    sin cerrojo : ");
        smp_print_cores();
        uart_dec(r0);
        uart_puts("\n");
        uart_end(lf);

        uint64_t r1 = smp_hammer(N, 1);
        lf = uart_begin();
        uart_puts("    con cerrojo : ");
        smp_print_cores();
        uart_dec(r1);
        uart_puts("\n");
        uart_end(lf);
        break;
    }

    case 'x':
        uart_puts("\n  Lo que ve la MMU (instruccion 'at s1e1r'):\n");
        show_translation("kernel  ", KERNEL_VA_BASE + 0x80000UL);
        show_translation("UART0   ", PERIPHERAL_BASE + 0x201000);
        show_translation("timers  ", LOCAL_BASE);
        show_translation("sin map ", KERNEL_VA_BASE + 0x70000000UL);
        show_translation("usuario ", USER_BASE);
        break;

    case 'b': {
        uart_puts("\n  Recorriendo 4 MB (16 pasadas sobre 256 KB)...\n    ");
        uart_dec(bench_memory());
        uart_puts(" us con las caches ");
        uart_puts(caches_are_on() ? "ENCENDIDAS\n" : "apagadas\n");
        break;
    }

    case 't':
        uart_puts("\n  uptime: ");
        uart_dec(timer_uptime_ms());
        uart_puts(" ms   ticks: ");
        uart_dec(timer_ticks());
        uart_puts("   IRQs: ");
        uart_dec(irq_count());
        uart_puts("\n");
        break;

    case 'h':
        menu();
        break;

    case '4': {
        volatile uint32_t *p = (volatile uint32_t *)0x0000FF0000000000UL;
        uint32_t v = *p;
        (void)v;
        break;
    }

    default:
        uart_putc(c);
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
    uart_puts("|  microkernel ARM64 bare metal        |\n");
    uart_puts("+--------------------------------------+\n\n");

    uart_puts("  Exception Level : EL");
    uart_putc((char)('0' + current_el()));
    uart_puts("\n  Device tree en  : 0x");
    uart_hex64(dtb_ptr);
    uart_puts("\n  MMU             : ");
    uart_puts(mmu_is_on() ? "encendida desde boot.S\n" : "APAGADA (?)\n");
    uart_puts("  Kernel en       : 0x");
    uart_hex64(KERNEL_VA_BASE + 0x80000);
    uart_puts("  (TTBR1)\n");

    timer_init(TICK_HZ);
    uart_enable_rx_irq();
    irq_enable();

    /* Preguntar a la GPU cuanta RAM nos ha dejado. Tiene que hacerse ANTES
     * de encender la MMU: la GPU lee el buffer de la RAM y no ve nuestra
     * cache. Si no contesta (o estamos en un emulador que no lo implementa)
     * nos quedamos con el tope teorico. */
    uint64_t ram_base = 0, ram_size = 0;
    if (mbox_arm_memory(&ram_base, &ram_size)) {
        uart_puts("  RAM de la CPU   : ");
        uart_dec(ram_size / 1024 / 1024);
        uart_puts(" MB  (base 0x");
        uart_hex64(ram_base);
        uart_puts(")\n");
    } else {
        uart_puts("  RAM de la CPU   : la GPU no contesta, supongo el maximo\n");
        ram_size = RAM_MAX;
    }

    pmm_init(ram_base + ram_size);
    ipc_init();
    mem_stats();

    /* La MMU lleva encendida desde boot.S: no habia alternativa, el kernel
     * esta enlazado en direcciones altas. Lo que si podemos apagar en
     * caliente son las caches, y es lo unico que se nota en la medida: la
     * traduccion en si no acelera nada, las caches lo son todo.
     * (En QEMU los dos numeros salen iguales porque no emula caches.) */
    uart_puts("\n  Midiendo la memoria con las caches APAGADAS...\n    ");

    /* Las IRQ, tapadas durante TODO el tramo sin caches, y no solo durante
     * la medida. Sin cache no hay instrucciones exclusivas, y sin ellas no
     * hay spinlock: una interrupcion aqui acabaria en scheduler_tick()
     * pidiendo el cerrojo del planificador, y ahi se quedaria. */
    uint64_t bf = irq_save();
    caches_disable();
    uint64_t before = bench_memory();
    caches_enable();
    irq_restore(bf);

    uart_dec(before);
    uart_puts(" us\n");

    uart_puts("  ...y con las caches encendidas:\n    ");
    uint64_t after = bench_memory();
    uart_dec(after);
    uart_puts(" us\n");
    if (after > 0 && before > after) {
        uart_puts("    mejora: x");
        uart_dec(before / after);
        uart_puts("\n");
    }

    /* Tres cuartas partes de la maquina llevan aparcadas desde el paso 1.
     * A partir de aqui estan encendidas, cada una con su pila y su MMU,
     * compartiendo las tablas del kernel. Todavia no hacen nada: sin
     * cerrojos de verdad no pueden tocar lo que es de todos. */
    uart_puts("\n  Despertando los nucleos 1, 2 y 3...\n");
    int vivos = smp_start_secondaries();
    uart_puts("    han contestado ");
    uart_dec((uint64_t)vivos);
    uart_puts(" de 3\n");
    smp_dump();

    uart_puts("\n  Arrancando hilos...\n");
    sched_init();
    mutex_init(&console);                       /* adopta este contexto como tarea 0 */
    task_create("alfa",  thread_ticker,     (void *)120UL);
    task_create("beta",  thread_ticker,     (void *)170UL);
    task_create("crunch", thread_cruncher,  0);
    task_create("efimero", thread_shortlived, 0);
    task_create("shell",  thread_shell,       0);

    mutex_init(&counter_mutex);
    chan_init(&pipe_chan);
    task_create("inc-a",  thread_incrementer, 0);
    task_create("inc-b",  thread_incrementer, 0);
    task_create("prod",   thread_producer,    0);
    task_create("cons",   thread_consumer,    0);
    sched_dump();

    /* Y ahora si: los otros tres nucleos entran al planificador. Hasta esta
     * linea se han limitado a contar sus ticks. */
    uart_puts("\n  Abriendo el planificador a los cuatro nucleos...\n");
    sched_start_smp();

    menu();

    /* La tarea 0 se queda de idle pura, igual que los otros tres nucleos:
     * solo se ejecuta cuando nadie mas quiere CPU. */
    idle_loop();
}
