/* user/fs.c - Servidor de ficheros, en espacio de usuario
 *
 * Lee FAT16 y FAT32 de una tarjeta SD, y monta las dos particiones: la de
 * datos en "/" y la de arranque en "/boot". No es parte del kernel: es un
 * proceso de
 * EL0 al que se le ha concedido la pagina de registros del controlador
 * EMMC, igual que al servidor de consola se le concede la de la PL011.
 * Si se cuelga, se cuelga el; el resto del sistema ni se entera.
 *
 * FAT es de 1980 y se nota, pero su forma es la de cualquier sistema de
 * ficheros: un sitio donde estan los nombres (el directorio), un sitio
 * donde estan los datos (los clusters), y un mapa que dice que trozo va
 * detras de que otro (la tabla FAT, que da nombre al invento).
 *
 *   sector 0            MBR: donde empieza cada particion
 *   +reservados         la tabla FAT, dos copias
 *   +FATs               el directorio raiz, 512 entradas de 32 bytes
 *   +raiz               los datos, en clusters de 4 sectores
 */
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"
#include "sd.h"
#include "fs_abi.h"

/* --- Geometria de UN volumen -----------------------------------------
 *
 * Esto eran diez variables globales, y valia mientras hubiera un solo
 * sistema de ficheros. Ahora hay dos -la particion de arranque y la de
 * datos- asi que la geometria deja de ser "la del disco" y pasa a ser "la
 * de este volumen". Se pasa como argumento a todo el que la necesite, que
 * es casi todo.
 *
 * Podria haberse dejado un puntero global al "volumen actual" y ahorrarse
 * el refactor: el servidor atiende una peticion cada vez, asi que seria
 * correcto. Pero un estado global que hay que acordarse de poner antes de
 * cada operacion es justo la clase de cosa que funciona hasta el dia que
 * alguien anyade un camino nuevo y se olvida.
 */
struct volumen {
    int      montado;
    int      fat32;              /* 0 = FAT16, 1 = FAT32                  */

    uint32_t part_lba;           /* donde empieza la particion            */
    uint32_t fat_lba;            /* donde empieza la primera FAT          */
    uint32_t root_lba;           /* FAT16: donde empieza el raiz fijo     */
    uint32_t root_cluster;       /* FAT32: primer cluster del raiz        */
    uint32_t data_lba;           /* donde empieza el primer cluster       */
    uint32_t sec_per_clus;
    uint32_t root_entries;       /* FAT16: cuantas caben en el raiz       */
    uint32_t num_fats;           /* copias: hay que escribirlas todas     */
    uint32_t sec_por_fat;
    uint32_t max_cluster;        /* el numero mas alto que existe aqui    */
    uint32_t eoc;                /* "fin de cadena" segun el tipo de FAT  */
    uint32_t fsinfo_lba;         /* FAT32: el sector con las cuentas      */
    int      fsinfo_olvidado;    /* ...ya puesto a "no lo se"             */
};

/* Donde cuelga cada volumen. El orden importa al buscar: se prueba el
 * punto mas largo primero, para que "/boot/x" no se lo quede "/". */
#define MAX_MONTAJES 2

struct montaje {
    char           punto[16];    /* "/" o "/boot"; vacio = ranura libre   */
    struct volumen vol;
};

static struct montaje montajes[MAX_MONTAJES];

/* Un sector cacheado: leer de la SD es caro y casi todo son relecturas
 * del mismo sitio (el directorio, o la FAT). */
static uint8_t  cache[512];
static uint32_t cache_lba = 0xFFFFFFFF;

static int leer(uint32_t lba, uint8_t *dst)
{
    return sd_read_block(lba, dst);
}

static uint8_t *cached(uint32_t lba)
{
    if (cache_lba != lba) {
        if (leer(lba, cache) < 0) return 0;
        cache_lba = lba;
    }
    return cache;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --- Montar: del MBR a la geometria ----------------------------------- */
/* --- Montar -----------------------------------------------------------
 *
 * FAT16 y FAT32 son el mismo formato con dos diferencias que importan:
 *
 *   1. En FAT16 el directorio raiz es una REGION FIJA detras de las FAT,
 *      con un numero de entradas decidido al formatear. En FAT32 no
 *      existe esa region: el raiz es una cadena de clusters como
 *      cualquier directorio, y el BPB dice por cual empieza. Es mejor en
 *      todo -crece, no tiene tope- y la unica razon de que FAT16 no lo
 *      hiciera asi es que en 1983 habia que poder encontrarlo sin leer la
 *      FAT.
 *
 *   2. Las casillas de la tabla miden 16 o 32 bits. De ahi los nombres, y
 *      de ahi que un volumen FAT16 no pueda pasar de 65.525 clusters.
 *
 * Y una cosa que NO esta en el BPB: de que tipo es. No hay ningun campo
 * que lo diga -el "FAT16   " que se ve en el sector es una etiqueta que
 * nadie garantiza- y la forma oficial de averiguarlo es CONTAR LOS
 * CLUSTERS. Menos de 4085 es FAT12, menos de 65525 es FAT16, y el resto
 * FAT32. Literalmente: el tipo de un volumen FAT es una consecuencia de
 * su tamanyo, no un dato.
 */
static int montar_particion(struct volumen *v, uint32_t lba)
{
    uint8_t bpb[512];

    v->montado = 0;
    if (leer(lba, bpb) < 0) return -1;
    if (le16(bpb + 510) != 0xAA55) return -1;
    if (le16(bpb + 11) != 512) return -1;          /* solo 512 b/sector */

    v->part_lba     = lba;
    v->sec_per_clus = bpb[13];
    v->num_fats     = bpb[16];
    v->root_entries = le16(bpb + 17);

    uint32_t reservados = le16(bpb + 14);
    v->sec_por_fat = le16(bpb + 22);
    if (!v->sec_por_fat) v->sec_por_fat = le32(bpb + 36);   /* FAT32 */

    if (!v->sec_per_clus || !v->num_fats || !v->sec_por_fat) return -1;

    uint32_t total = le16(bpb + 19);
    if (!total) total = le32(bpb + 32);
    if (!total) return -1;

    v->fat_lba  = lba + reservados;
    v->root_lba = v->fat_lba + v->num_fats * v->sec_por_fat;

    uint32_t sec_raiz = (v->root_entries * 32 + 511) / 512;
    v->data_lba = v->root_lba + sec_raiz;

    if (total <= (v->data_lba - lba)) return -1;

    uint32_t clusters = (total - (v->data_lba - lba)) / v->sec_per_clus;

    /* Aqui se decide el tipo, contando. */
    v->fat32 = (clusters >= 65525);

    if (v->fat32) {
        if (v->root_entries) return -1;            /* FAT32 no tiene raiz fijo */
        v->root_cluster = le32(bpb + 44);
        if (v->root_cluster < 2) return -1;
        v->eoc = 0x0FFFFFF8;

        uint32_t fsi = le16(bpb + 48);
        v->fsinfo_lba      = fsi ? lba + fsi : 0;
        v->fsinfo_olvidado = 0;
    } else {
        if (!v->root_entries) return -1;           /* FAT16 si lo tiene */
        v->root_cluster = 0;
        v->eoc = 0xFFF8;
    }

    v->max_cluster = clusters + 1;

    /* Un tope duro: la FAT tiene 256 casillas de 16 bits por sector, o 128
     * de 32. Por muchos clusters que diga el BPB no puede haber mas de los
     * que caben en ella. Sin esto, un BPB raro haria que alloc_cluster
     * escribiera PASADA la tabla, encima de los datos. Es exactamente el
     * tipo de fallo que no avisa: la tarjeta sigue pareciendo correcta
     * hasta que se pierde entera. */
    uint32_t cabe = v->sec_por_fat * (v->fat32 ? 128 : 256);
    if (cabe < 2) return -1;
    if (v->max_cluster > cabe - 1) v->max_cluster = cabe - 1;

    v->montado = 1;
    return 0;
}

/* Poner un nombre a un montaje. */
static void poner_punto(struct montaje *m, const char *punto)
{
    int i = 0;
    for (; punto[i] && i < (int)sizeof(m->punto) - 1; i++) m->punto[i] = punto[i];
    m->punto[i] = 0;
}

/* Recorrer la tabla de particiones y colocar cada una donde toca.
 *
 * La de datos (FAT32) es el raiz, y la de arranque (FAT16) cuelga de
 * /boot. Si solo hay una, esa es el raiz: asi la imagen de pruebas de un
 * solo volumen sigue valiendo.
 *
 * Se elige por TIPO y no por orden en la tabla porque el tipo se ha
 * deducido leyendo el volumen, y el orden es solo lo que puso quien
 * formateo. */
static int montar(void)
{
    uint8_t mbr[512];

    for (int i = 0; i < MAX_MONTAJES; i++) montajes[i].punto[0] = 0;

    if (leer(0, mbr) < 0) return -1;
    if (le16(mbr + 510) != 0xAA55) return -1;

    struct volumen datos = { 0 }, arranque = { 0 };
    int hay_datos = 0, hay_arranque = 0;

    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + 446 + i * 16;
        uint8_t tipo = e[4];
        if (tipo == 0) continue;

        uint32_t lba = le32(e + 8);
        if (!lba) continue;

        struct volumen v;
        if (montar_particion(&v, lba) < 0) continue;

        if (v.fat32 && !hay_datos)        { datos    = v; hay_datos    = 1; }
        else if (!v.fat32 && !hay_arranque) { arranque = v; hay_arranque = 1; }
    }

    int n = 0;

    if (hay_datos) {
        poner_punto(&montajes[n], "/");
        montajes[n].vol = datos;
        n++;
        if (hay_arranque) {
            poner_punto(&montajes[n], "/boot");
            montajes[n].vol = arranque;
            n++;
        }
    } else if (hay_arranque) {
        /* Una sola particion: es el raiz. */
        poner_punto(&montajes[n], "/");
        montajes[n].vol = arranque;
        n++;
    }

    return n ? 0 : -1;
}

/* De una ruta absoluta al volumen que la sirve, y a lo que queda de ruta
 * dentro de el.
 *
 * "/boot/config.txt" -> volumen de arranque, "/config.txt"
 * "/hola.txt"        -> volumen de datos,    "/hola.txt"
 *
 * Se prueba el punto de montaje MAS LARGO primero, porque "/" casa con
 * todo. Y el punto tiene que terminar en barra o en fin de cadena, o
 * "/bootcode.bin" se lo quedaria "/boot" y buscaria un "code.bin" que no
 * existe. */
static struct volumen *volumen_de(const char *ruta, const char **resto)
{
    struct montaje *mejor = 0;
    int mejor_largo = -1;

    for (int i = 0; i < MAX_MONTAJES; i++) {
        struct montaje *m = &montajes[i];
        if (!m->punto[0] || !m->vol.montado) continue;

        int largo = 0;
        while (m->punto[largo]) largo++;

        int es_raiz = (largo == 1);              /* el punto "/" */
        int casa = 1;
        for (int k = 0; k < largo; k++)
            if (ruta[k] != m->punto[k]) { casa = 0; break; }

        if (!casa) continue;
        if (!es_raiz && ruta[largo] != '/' && ruta[largo] != 0) continue;

        if (largo > mejor_largo) { mejor = m; mejor_largo = largo; }
    }

    if (!mejor) return 0;

    if (mejor_largo == 1) *resto = ruta;         /* "/" no se quita */
    else {
        *resto = ruta + mejor_largo;
        if (!(*resto)[0]) *resto = "/";          /* "/boot" a secas */
    }
    return &mejor->vol;
}

/* ====================== ESCRITURA ==================================
 *
 * Escribir en FAT es tocar tres sitios distintos y en el orden correcto:
 * los datos, la tabla FAT (que dice que cluster va detras de cual) y la
 * entrada de directorio (que dice el tamanyo). Si solo se toca uno, el
 * fichero queda inconsistente. Aqui no hay diario ni nada parecido: un
 * corte de corriente a mitad deja el volumen a medias, como en 1980.
 */

/* --- Nombres largos (VFAT) --------------------------------------------
 *
 * FAT guarda los nombres en 8.3 y punto. Los nombres largos se anyadieron
 * despues, en 1995, y el truco con el que se hizo es de los mas elegantes
 * que hay en informatica: delante de la entrada corta de toda la vida se
 * ponen entradas EXTRA con el atributo 0x0F.
 *
 * Ese 0x0F es solo-lectura + oculto + sistema + etiqueta-de-volumen a la
 * vez, una combinacion que no tiene ningun sentido. Y ahi esta la gracia:
 * MS-DOS, que no sabia nada de esto, las descartaba por absurdas y seguia
 * viendo el disco entero con sus nombres cortos. Un formato ampliado sin
 * romper a quien no entiende la ampliacion.
 *
 * Cada entrada extra lleva 13 caracteres UTF-16 repartidos en tres huecos
 * (1-10, 14-25, 28-31), porque tuvieron que colarse entre los campos que
 * ya existian. Van en orden INVERSO: la primera que aparece en el disco es
 * el ultimo trozo del nombre, y lleva el bit 0x40 en su numero de
 * secuencia para decir "por aqui empieza".
 *
 * Y una suma de comprobacion del nombre CORTO, repetida en cada trozo. No
 * es paranoia: si un sistema antiguo renombra el fichero, toca la entrada
 * corta y deja las largas huerfanas apuntando a un nombre que ya no
 * existe. La suma es lo que detecta ese desacuerdo. Si no cuadra, se tira
 * el nombre largo y se usa el corto, que siempre esta.
 */
static uint8_t suma_83(const uint8_t *d)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) ? 0x80 : 0) + (s >> 1) + d[i]);
    return s;
}

struct lfn {
    char    nombre[FS_NAME_MAX];
    int     valido;
    uint8_t suma;
};

static void lfn_reset(struct lfn *l)
{
    l->valido = 0;
    for (int i = 0; i < FS_NAME_MAX; i++) l->nombre[i] = 0;
}

static void lfn_add(struct lfn *l, const uint8_t *d)
{
    int seq = d[0] & 0x3F;

    if (d[0] & 0x40) {                       /* el ultimo trozo del nombre */
        lfn_reset(l);
        l->valido = 1;
        l->suma   = d[13];
    }

    /* Un trozo suelto, sin su cabecera, o de otro fichero: no vale nada. */
    if (!l->valido || seq < 1 || seq > 20 || d[13] != l->suma) {
        lfn_reset(l);
        return;
    }

    static const int hueco[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
    int base = (seq - 1) * 13;

    for (int i = 0; i < 13; i++) {
        uint16_t c = (uint16_t)(d[hueco[i]] | (d[hueco[i] + 1] << 8));
        if (c == 0x0000 || c == 0xFFFF) break;   /* fin, o relleno */

        int o = base + i;
        if (o >= FS_NAME_MAX - 1) break;

        /* Solo ASCII. Lo de fuera se marca en vez de inventarselo: un '?'
         * se ve, y un byte truncado al azar da un nombre que parece bueno
         * y no abre nada. */
        l->nombre[o] = (c < 128) ? (char)c : '?';
    }
}

/* Las dos viven mas abajo, con la parte de lectura, porque las comparten
 * los dos lados. */
static void     a_8_3(const char *nombre, char out[11]);
static uint32_t siguiente_cluster(struct volumen *v, uint32_t c);

/* Y estas tres tambien: las necesita el recorrido de directorios, que esta
 * antes, y se definen con el resto de la parte de lectura. */
static void nombre_de_entrada(const struct lfn *l, const uint8_t *d,
                              char salida[FS_NAME_MAX]);
static char mayus(char c);
static void de_8_3(const uint8_t *d, char salida[FS_NAME_MAX]);
static int  igual_sin_caja(const char *a, const char *b);



static int escribir(uint32_t lba, const uint8_t *src)
{
    if (sd_write_block(lba, src) < 0) return -1;
    cache_lba = 0xFFFFFFFF;          /* lo cacheado ya no vale */
    return 0;
}

/* Poner un valor en la tabla FAT, en TODAS sus copias.
 *
 * Hay dos (a veces mas) porque la FAT es lo unico irremplazable del
 * volumen: perder los datos de un fichero es perder un fichero, perder la
 * FAT es perderlos todos. Escribir solo en la primera "funciona" hasta que
 * alguien repare el disco con la segunda. */
/* FAT32 guarda en un sector aparte -el FSInfo- cuantos clusters quedan
 * libres y por donde seguir buscando. Son un ATAJO, no la verdad: la
 * verdad esta en la FAT, y el estandar dice que se pueden poner a
 * 0xFFFFFFFF para decir "no lo se, cuentalo tu".
 *
 * Nosotros no llevamos esa cuenta, asi que en cuanto tocamos la FAT el
 * numero de ahi deja de valer. Dejarlo como estaba seria peor que no
 * tenerlo: un numero que parece bueno y no lo es. Asi que se invalida, una
 * vez, la primera vez que se escribe algo.
 *
 * Lo encontro fsck_msdos, no nosotros:
 *     Warning: Free space in FSInfo block (115117) not correct (115116)
 */
static void fsinfo_olvidar(struct volumen *v)
{
    if (!v->fat32 || !v->fsinfo_lba || v->fsinfo_olvidado) return;

    uint8_t *b = cached(v->fsinfo_lba);
    if (!b) return;
    if (le32(b) != 0x41615252) return;          /* no es un FSInfo */

    for (int i = 488; i < 496; i++) b[i] = 0xFF;   /* libres y "siguiente" */
    if (escribir(v->fsinfo_lba, b) == 0) v->fsinfo_olvidado = 1;
}

static int fat_set(struct volumen *v, uint32_t c, uint32_t valor)
{
    if (c < 2 || c > v->max_cluster) return -1;

    fsinfo_olvidar(v);

    uint32_t ancho = v->fat32 ? 4 : 2;
    uint32_t off   = c * ancho;

    for (uint32_t copia = 0; copia < v->num_fats; copia++) {
        uint32_t lba = v->fat_lba + copia * v->sec_por_fat + off / 512;

        uint8_t *b = cached(lba);
        if (!b) return -1;

        uint32_t o = off % 512;
        b[o]     = (uint8_t)(valor & 0xFF);
        b[o + 1] = (uint8_t)(valor >> 8);

        if (v->fat32) {
            /* Los cuatro bits de arriba son del volumen, no nuestros: se
             * conservan tal cual estaban. */
            b[o + 2] = (uint8_t)(valor >> 16);
            b[o + 3] = (uint8_t)((b[o + 3] & 0xF0) | ((valor >> 24) & 0x0F));
        }

        if (escribir(lba, b) < 0) return -1;
    }
    return 0;
}

/* Buscar un cluster libre y marcarlo como "fin de fichero". Devuelve su
 * numero, o 0 si el volumen esta lleno. El 0 vale de "no hay" porque los
 * clusters 0 y 1 estan reservados y nunca se reparten. */
static uint32_t alloc_cluster(struct volumen *v)
{
    for (uint32_t c = 2; c <= v->max_cluster; c++) {
        if (siguiente_cluster(v, c) == 0) {
            /* Marcarlo como "fin de fichero". El valor depende del tipo:
             * 0xFFFF en FAT16 y 0x0FFFFFFF en FAT32. */
            if (fat_set(v, c, v->fat32 ? 0x0FFFFFFF : 0xFFFF) < 0) return 0;
            return c;
        }
    }
    return 0;
}

static void free_chain(struct volumen *v, uint32_t c)
{
    while (c >= 2 && c < v->eoc) {
        uint32_t sig = siguiente_cluster(v, c);
        fat_set(v, c, 0);
        c = sig;
    }
}

/* --- Un directorio, sea el raiz o no ---------------------------------
 *
 * En FAT16 el directorio raiz es RARO: es una region de sectores fija,
 * puesta justo detras de las FAT, con un numero de entradas decidido al
 * formatear y que no se puede cambiar. Un subdirectorio, en cambio, es un
 * fichero normal y corriente cuyo contenido son entradas de 32 bytes: una
 * cadena de clusters, que crece como cualquier otra.
 *
 * Son dos cosas distintas de verdad, y la unica forma de no escribir dos
 * veces cada recorrido es esconder la diferencia detras de una pregunta:
 * "dame el sector numero N de este directorio". El cluster 0 quiere decir
 * el raiz, que es un numero que no existe como cluster de datos -los
 * validos empiezan en el 2- y por eso sirve de marca.
 *
 * Que el raiz tenga un tope y los demas no es la razon de que en FAT16 se
 * pueda llenar el directorio raiz teniendo el disco medio vacio. */
static int dir_sector(struct volumen *v, uint32_t dir, uint32_t n, uint32_t *lba)
{
    if (dir == 0) {
        /* En FAT16 el raiz es una region fija con tope. En FAT32 no hay
         * tal region: el raiz es una cadena de clusters como cualquier
         * directorio, y por eso puede crecer. */
        if (!v->fat32) {
            uint32_t sectores = (v->root_entries * 32 + 511) / 512;
            if (n >= sectores) return -1;
            *lba = v->root_lba + n;
            return 0;
        }
        dir = v->root_cluster;
    }

    uint32_t c = dir;
    for (uint32_t saltar = n / v->sec_per_clus; saltar; saltar--) {
        c = siguiente_cluster(v, c);
        if (c < 2 || c >= v->eoc) return -1;      /* se acabo la cadena */
    }
    *lba = v->data_lba + (c - 2) * v->sec_per_clus + (n % v->sec_per_clus);
    return 0;
}

/* Localizar una entrada dentro de un directorio: en que sector esta y en
 * que posicion. Con eso se puede leer y tambien MODIFICAR, que es lo que
 * hace falta para cambiarle el tamanyo.
 *
 * 'quiero': 0 solo ficheros, 1 solo directorios, -1 lo que sea. Hace
 * falta porque al recorrer una ruta las componentes de en medio TIENEN que
 * ser directorios, y la ultima no. */
static int dir_lookup_en(struct volumen *v, uint32_t dir, const char *nombre, int quiero,
                         uint32_t *lba, uint32_t *off)
{
    char patron[11];
    a_8_3(nombre, patron);

    /* El acumulador vive FUERA del bucle de sectores: una cadena de
     * entradas largas puede empezar al final de un sector y terminar en el
     * siguiente, y si se reiniciara en cada sector se perderian justo los
     * nombres mas largos. */
    struct lfn l;
    lfn_reset(&l);

    for (uint32_t s = 0; ; s++) {
        uint32_t sl;
        if (dir_sector(v, dir, s, &sl) < 0) return -1;

        uint8_t *b = cached(sl);
        if (!b) return -1;

        for (int e = 0; e < 512; e += 32) {
            uint8_t *d = b + e;
            if (d[0] == 0x00) return -1;          /* fin del directorio */

            if ((d[11] & 0x0F) == 0x0F) { lfn_add(&l, d); continue; }

            if (d[0] == 0xE5) { lfn_reset(&l); continue; }   /* borrada */
            if (d[11] & 0x08) { lfn_reset(&l); continue; }   /* etiqueta */

            char largo[FS_NAME_MAX];
            nombre_de_entrada(&l, d, largo);
            lfn_reset(&l);                        /* consumido */

            int es_dir = (d[11] & 0x10) ? 1 : 0;
            if (quiero >= 0 && es_dir != quiero) continue;

            /* Vale cualquiera de los dos nombres. El corto sigue
             * funcionando porque esta siempre y porque es lo que ensenya
             * un sistema que no entienda los largos. */
            int igual = 1;
            for (int i = 0; i < 11; i++)
                if (d[i] != (uint8_t)patron[i]) { igual = 0; break; }

            if (!igual && !igual_sin_caja(largo, nombre)) continue;

            *lba = sl;
            *off = (uint32_t)e;
            return 0;
        }
    }
}

/* El cluster donde empieza un subdirectorio, dada su entrada.
 *
 * Ojo con el "..": en FAT, el ".." del primer nivel apunta al cluster 0, y
 * el 0 quiere decir el raiz. No es un caso especial inventado por
 * nosotros, viene asi en el disco. */
static uint32_t entrada_cluster(uint32_t lba, uint32_t off)
{
    uint8_t *b = cached(lba);
    return b ? le16(b + off + 26) : 0;
}

/* Crear una entrada nueva en un directorio. Se reaprovecha la primera
 * casilla borrada que aparezca, y si no hay ninguna se usa la primera sin
 * estrenar.
 *
 * 'attr' es 0x20 para un fichero y 0x10 para un directorio. */
/* ¿Cabe este nombre en 8.3?
 *
 * Los nombres largos se LEEN pero no se escriben: eso exigiria generar la
 * cadena de entradas VFAT y, peor, inventar un nombre corto que no choque
 * con ninguno de los que ya hay.
 *
 * Mientras no este, lo importante es NEGARSE en vez de apanyarselo.
 * a_8_3() de un "con espacios.txt" da "CON ESPATXT", que es un nombre que
 * ningun sistema sabe volver a escribir igual y que al listarlo sale como
 * "CON.TXT" -de_8_3 para en el primer espacio-. O sea: un fichero que se
 * crea con un nombre y aparece con otro. Mejor un error. */
static int cabe_en_8_3(const char *n)
{
    int base = 0, ext = 0, punto = 0;

    for (const char *p = n; *p; p++) {
        if (*p == ' ') return 0;              /* un espacio no cabe */
        if (*p == '.') {
            if (punto) return 0;              /* ni dos puntos */
            punto = 1;
            continue;
        }
        if (punto) ext++; else base++;
    }

    return base >= 1 && base <= 8 && ext <= 3;
}

/* --- Inventar un nombre corto -----------------------------------------
 *
 * Todo fichero con nombre largo tiene TAMBIEN un nombre 8.3, y no es
 * decoracion: es el que ve un sistema que no entienda las entradas VFAT, y
 * es el que lleva la suma de comprobacion que ata la cadena. Hay que
 * inventarselo, y tiene que ser unico dentro de su directorio.
 *
 * La receta: coger las primeras letras que valgan, tirar espacios y
 * signos raros, subir a mayusculas, y pegar "~1" al final. Si ya existe,
 * "~2", y asi. De ahi salen los HOLAMU~1.TXT de toda la vida.
 *
 * Y de ahi sale tambien el ENSA~209.ELF que aparecio en la tarjeta de
 * verdad: cuando hay muchas colisiones, el numero crece y se come las
 * letras. Un nombre generado no es un nombre elegido, y se nota.
 *
 * Lo caro es la comprobacion: hay que mirar el directorio entero por cada
 * intento. Con directorios de decenas de entradas da igual; es de las
 * cosas que en un sistema de verdad se resuelven con un hash. */
static int vale_en_8_3(char c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c == '_' || c == '-' || c == '!' || c == '#' || c == '$' ||
           c == '%' || c == '&' || c == '\'' || c == '(' || c == ')' ||
           c == '@' || c == '^' || c == '{' || c == '}' || c == '~';
}

static void nombre_corto(const char *largo, int n, char salida[FS_NAME_MAX])
{
    /* La extension es lo que hay detras del ULTIMO punto. */
    const char *punto = 0;
    for (const char *p = largo; *p; p++) if (*p == '.') punto = p;

    char base[8];
    int b = 0;
    for (const char *p = largo; *p && p != punto && b < 6; p++) {
        char c = mayus(*p);
        if (vale_en_8_3(c)) base[b++] = c;
    }
    if (!b) base[b++] = '_';               /* un nombre todo raro */

    int o = 0;
    for (int i = 0; i < b; i++) salida[o++] = base[i];

    salida[o++] = '~';
    if (n >= 100) salida[o++] = (char)('0' + (n / 100) % 10);
    if (n >= 10)  salida[o++] = (char)('0' + (n / 10) % 10);
    salida[o++] = (char)('0' + n % 10);

    if (punto) {
        salida[o++] = '.';
        for (int i = 1; i <= 3 && punto[i]; i++) {
            char c = mayus(punto[i]);
            if (vale_en_8_3(c)) salida[o++] = c;
        }
    }
    salida[o] = 0;
}

static int nombre_corto_libre(struct volumen *v, uint32_t dir, const char *largo,
                              char salida[FS_NAME_MAX])
{
    for (int n = 1; n < 1000; n++) {
        nombre_corto(largo, n, salida);

        uint32_t l, o;
        if (dir_lookup_en(v, dir, salida, -1, &l, &o) < 0) return 0;   /* libre */
    }
    return -1;                             /* mil colisiones: que se queje */
}

/* Buscar N huecos SEGUIDOS en un directorio.
 *
 * Seguidos y en ese orden, porque las entradas de nombre largo tienen que
 * ir pegadas justo delante de la corta: asi es como se sabe cuales son
 * suyas. Un hueco aqui y otro alla no vale.
 *
 * Devuelve el indice de entrada (no de sector) donde empieza el hueco. */
static int hueco_seguido(struct volumen *v, uint32_t dir, int cuantas,
                         uint32_t *donde)
{
    int seguidas = 0;
    uint32_t inicio = 0;

    for (uint32_t s = 0; ; s++) {
        uint32_t lba;
        if (dir_sector(v, dir, s, &lba) < 0) return -1;   /* directorio lleno */

        uint8_t *b = cached(lba);
        if (!b) return -1;

        for (int e = 0; e < 512; e += 32) {
            uint32_t indice = s * 16 + (uint32_t)(e / 32);
            uint8_t *d = b + e;

            if (d[0] == 0x00 || d[0] == 0xE5) {
                if (!seguidas) inicio = indice;
                if (++seguidas == cuantas) { *donde = inicio; return 0; }
            } else {
                seguidas = 0;
            }
        }
    }
}

/* La entrada numero 'indice' de un directorio, para escribir en ella. */
static uint8_t *entrada_en(struct volumen *v, uint32_t dir, uint32_t indice,
                           uint32_t *lba, uint32_t *off)
{
    if (dir_sector(v, dir, indice / 16, lba) < 0) return 0;
    *off = (indice % 16) * 32;
    return cached(*lba);
}

/* Crear una entrada con nombre largo: la cadena VFAT y detras la corta.
 *
 * Los trozos van en orden INVERSO -el ultimo pedazo del nombre primero- y
 * el primero que aparece lleva el bit 0x40. Cada uno repite la suma de
 * comprobacion del nombre corto, que es lo que ata la cadena a su duenyo.
 */
static int dir_create_largo(struct volumen *v, uint32_t dir, const char *largo,
                            uint8_t attr, uint32_t *lba, uint32_t *off)
{
    char corto[FS_NAME_MAX];
    if (nombre_corto_libre(v, dir, largo, corto) < 0) return -1;

    uint64_t len = 0;
    while (largo[len]) len++;
    if (len >= FS_NAME_MAX) return -1;

    int trozos = (int)((len + 12) / 13);
    if (trozos < 1 || trozos > 20) return -1;

    uint32_t inicio;
    if (hueco_seguido(v, dir, trozos + 1, &inicio) < 0) return -1;

    char patron[11];
    a_8_3(corto, patron);
    uint8_t suma = suma_83((const uint8_t *)patron);

    static const int hueco[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };

    for (int t = trozos; t >= 1; t--) {
        uint32_t l, o;
        uint8_t *b = entrada_en(v, dir, inicio + (uint32_t)(trozos - t), &l, &o);
        if (!b) return -1;

        uint8_t *d = b + o;
        for (int i = 0; i < 32; i++) d[i] = 0;

        d[0]  = (uint8_t)(t | (t == trozos ? 0x40 : 0));
        d[11] = 0x0F;
        d[13] = suma;

        for (int i = 0; i < 13; i++) {
            uint64_t p = (uint64_t)(t - 1) * 13 + (uint64_t)i;
            uint16_t c;

            if (p < len)       c = (uint16_t)(unsigned char)largo[p];
            else if (p == len) c = 0x0000;         /* el cero final */
            else               c = 0xFFFF;         /* relleno */

            d[hueco[i]]     = (uint8_t)(c & 0xFF);
            d[hueco[i] + 1] = (uint8_t)(c >> 8);
        }

        if (escribir(l, b) < 0) return -1;
    }

    /* Y la corta, detras del todo. */
    uint32_t l, o;
    uint8_t *b = entrada_en(v, dir, inicio + (uint32_t)trozos, &l, &o);
    if (!b) return -1;

    int ultima = (b[o] == 0x00);
    uint8_t *d = b + o;

    for (int i = 0; i < 11; i++) d[i] = (uint8_t)patron[i];
    for (int i = 11; i < 32; i++) d[i] = 0;
    d[11] = attr;

    if (ultima && o + 32 < 512) d[32] = 0x00;

    if (escribir(l, b) < 0) return -1;

    *lba = l;
    *off = o;
    return 0;
}

static int dir_create_en(struct volumen *v, uint32_t dir, const char *nombre, uint8_t attr,
                         uint32_t *lba, uint32_t *off)
{
    /* Si cabe en 8.3 se escribe como siempre, sin cadena VFAT: un nombre
     * corto no necesita que nadie lo explique. */
    if (!cabe_en_8_3(nombre))
        return dir_create_largo(v, dir, nombre, attr, lba, off);

    char patron[11];
    a_8_3(nombre, patron);

    for (uint32_t s = 0; ; s++) {
        uint32_t sl;
        if (dir_sector(v, dir, s, &sl) < 0) return -1;   /* directorio lleno */

        uint8_t *b = cached(sl);
        if (!b) return -1;

        for (int e = 0; e < 512; e += 32) {
            uint8_t *d = b + e;
            if (d[0] != 0x00 && d[0] != 0xE5) continue;

            int ultima = (d[0] == 0x00);

            for (int i = 0; i < 11; i++) d[i] = (uint8_t)patron[i];
            for (int i = 11; i < 32; i++) d[i] = 0;
            d[11] = attr;

            /* Si esta era la marca de "aqui se acaba el directorio", la
             * siguiente casilla tiene que heredarla o el recorrido no
             * pararia nunca. */
            if (ultima && e + 32 < 512) d[32] = 0x00;

            if (escribir(sl, b) < 0) return -1;

            *lba = sl;
            *off = (uint32_t)e;
            return 0;
        }
    }
}

/* --- Recorrer una ruta ------------------------------------------------
 *
 * "/DOCS/NOTAS/A.TXT" se parte en componentes y se baja una a una: se
 * busca "DOCS" en el raiz, se coge su cluster, se busca "NOTAS" ahi
 * dentro, y se para antes de la ultima. Lo que sale es el directorio que
 * CONTIENE lo que se pedia, mas el nombre suelto de la ultima componente.
 *
 * Esa separacion no es un capricho de la implementacion: crear, borrar y
 * renombrar necesitan las dos mitades por separado, porque lo que se toca
 * es la entrada DENTRO del directorio padre. Un "abrir" que solo devolviera
 * el fichero no valdria para ninguna de las tres.
 *
 * Las rutas llegan siempre absolutas: el servidor no sabe que es un
 * directorio actual, y no quiere saberlo. Ver fs_abi.h. */
static int resolver(struct volumen *v, const char *ruta, uint32_t *dir, char *ultimo)
{
    if (ruta[0] != '/') return -1;               /* tiene que ser absoluta */

    uint32_t actual = 0;                         /* el raiz */
    const char *p = ruta + 1;

    for (;;) {
        /* Cortar la componente siguiente. */
        char comp[FS_NAME_MAX];
        uint32_t n = 0;
        while (*p && *p != '/' && n < FS_NAME_MAX - 1) comp[n++] = *p++;
        comp[n] = 0;
        while (*p && *p != '/') p++;             /* por si era larguisima */

        if (*p != '/') {                         /* era la ultima */
            if (n == 0) return -1;               /* "/docs/" no nombra nada */
            *dir = actual;
            for (uint32_t i = 0; i <= n; i++) ultimo[i] = comp[i];
            return 0;
        }

        p++;                                     /* saltar la barra */
        if (n == 0) continue;                    /* "//", que no estorba */

        /* Componente de en medio: TIENE que ser un directorio. */
        uint32_t lba, off;
        if (dir_lookup_en(v, actual, comp, 1, &lba, &off) < 0) return -1;
        actual = entrada_cluster(lba, off);
    }
}

/* Los dos de siempre, ahora encima de resolver(v). */
static int dir_lookup(struct volumen *v, const char *ruta, uint32_t *lba, uint32_t *off)
{
    uint32_t dir;
    char ultimo[FS_NAME_MAX];
    if (resolver(v, ruta, &dir, ultimo) < 0) return -1;
    return dir_lookup_en(v, dir, ultimo, 0, lba, off);
}

static int dir_create(struct volumen *v, const char *ruta, uint32_t *lba, uint32_t *off)
{
    uint32_t dir;
    char ultimo[FS_NAME_MAX];
    if (resolver(v, ruta, &dir, ultimo) < 0) return -1;
    return dir_create_en(v, dir, ultimo, 0x20, lba, off);
}

/* --- La fecha, en el formato de 1980 ----------------------------------
 *
 * FAT guarda la fecha en dos palabras de 16 bits, y el reparto de bits
 * cuenta la historia de cuando se disenyo:
 *
 *   fecha: anyo desde 1980 (7 bits) | mes (4) | dia (5)
 *   hora:  hora (5) | minuto (6) | segundo PARTIDO POR DOS (5)
 *
 * Los segundos van de dos en dos porque no cabian: con 5 bits llegas a 31,
 * no a 59. Se decidio que un fichero con la hora a dos segundos de
 * precision era suficiente, y ahi sigue cuarenta anyos despues.
 *
 * El anyo en 7 bits llega hasta 2107. Esa si es una fecha lejana... y sin
 * embargo alguien la vera. */
static void fecha_fat(uint64_t segundos, uint16_t *fecha, uint16_t *hora)
{
    uint64_t dias = segundos / 86400;
    uint64_t resto = segundos % 86400;

    uint32_t h = (uint32_t)(resto / 3600);
    uint32_t m = (uint32_t)((resto % 3600) / 60);
    uint32_t s = (uint32_t)(resto % 60);

    /* De dias desde 1970 a anyo/mes/dia, contando anyos bisiestos a mano.
     * Es el algoritmo aburrido y es el correcto; el listo con divisiones
     * magicas se equivoca en los siglos. */
    uint32_t anyo = 1970;
    for (;;) {
        int bis = (anyo % 4 == 0 && anyo % 100 != 0) || anyo % 400 == 0;
        uint32_t largo = bis ? 366 : 365;
        if (dias < largo) break;
        dias -= largo;
        anyo++;
    }

    int bis = (anyo % 4 == 0 && anyo % 100 != 0) || anyo % 400 == 0;
    static const uint32_t meses[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

    uint32_t mes = 0;
    for (; mes < 12; mes++) {
        uint32_t largo = meses[mes] + ((mes == 1 && bis) ? 1u : 0u);
        if (dias < largo) break;
        dias -= largo;
    }

    if (anyo < 1980) anyo = 1980;          /* FAT no sabe de antes */

    *fecha = (uint16_t)(((anyo - 1980) << 9) | ((mes + 1) << 5) | (dias + 1));
    *hora  = (uint16_t)((h << 11) | (m << 5) | (s / 2));
}

/* Y de vuelta, para poder ensenyarla y para que make la compare. */
static uint64_t fecha_unix(uint16_t fecha, uint16_t hora)
{
    if (!fecha) return 0;                  /* sin fecha */

    uint32_t anyo = 1980 + (fecha >> 9);
    uint32_t mes  = (fecha >> 5) & 0x0F;
    uint32_t dia  = fecha & 0x1F;
    if (mes < 1 || mes > 12 || dia < 1) return 0;

    uint64_t dias = 0;
    for (uint32_t a = 1970; a < anyo; a++)
        dias += ((a % 4 == 0 && a % 100 != 0) || a % 400 == 0) ? 366 : 365;

    static const uint32_t meses[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int bis = (anyo % 4 == 0 && anyo % 100 != 0) || anyo % 400 == 0;
    for (uint32_t m = 1; m < mes; m++)
        dias += meses[m - 1] + ((m == 2 && bis) ? 1u : 0u);

    dias += dia - 1;

    return dias * 86400
         + (uint64_t)((hora >> 11) & 0x1F) * 3600
         + (uint64_t)((hora >> 5) & 0x3F) * 60
         + (uint64_t)(hora & 0x1F) * 2;
}

/* Poner en una entrada la hora de ahora. Se llama al crear y al escribir,
 * que son los dos momentos en que un fichero cambia. */
static void tocar(uint8_t *d)
{
    uint16_t fecha, hora;
    fecha_fat(ahora(), &fecha, &hora);

    d[14] = (uint8_t)(hora & 0xFF);  d[15] = (uint8_t)(hora >> 8);   /* creacion */
    d[16] = (uint8_t)(fecha & 0xFF); d[17] = (uint8_t)(fecha >> 8);
    d[18] = (uint8_t)(fecha & 0xFF); d[19] = (uint8_t)(fecha >> 8);  /* acceso */
    d[22] = (uint8_t)(hora & 0xFF);  d[23] = (uint8_t)(hora >> 8);   /* escritura */
    d[24] = (uint8_t)(fecha & 0xFF); d[25] = (uint8_t)(fecha >> 8);
}

/* Leer y modificar un campo de la entrada de directorio. */
static int dir_update(uint32_t lba, uint32_t off, uint32_t cluster, uint32_t tam)
{
    uint8_t *b = cached(lba);
    if (!b) return -1;

    uint8_t *d = b + off;
    tocar(d);
    d[26] = (uint8_t)(cluster & 0xFF);
    d[27] = (uint8_t)(cluster >> 8);
    d[28] = (uint8_t)(tam & 0xFF);
    d[29] = (uint8_t)(tam >> 8);
    d[30] = (uint8_t)(tam >> 16);
    d[31] = (uint8_t)(tam >> 24);

    return escribir(lba, b);
}

static void dir_read(uint32_t lba, uint32_t off, uint32_t *cluster, uint32_t *tam)
{
    uint8_t *b = cached(lba);
    *cluster = b ? le16(b + off + 26) : 0;
    *tam     = b ? le32(b + off + 28) : 0;
}

/* Escribir 'n' bytes en 'offset'. Extiende el fichero si hace falta.
 *
 * No se admiten agujeros: escribir mas alla del final obligaria a rellenar
 * con ceros lo de en medio, y eso son mas casos que valor. */
static int fichero_escribir(struct volumen *v, const char *nombre, uint32_t offset,
                            const uint8_t *datos, uint32_t n)
{
    uint32_t dlba, doff;
    if (dir_lookup(v, nombre, &dlba, &doff) < 0) {
        if (dir_create(v, nombre, &dlba, &doff) < 0) return -1;
    }

    uint32_t primero, tam;
    dir_read(dlba, doff, &primero, &tam);
    if (offset > tam) return -1;

    uint32_t bytes_por_clus = v->sec_per_clus * 512;

    /* Un fichero recien creado no tiene ni un cluster. */
    if (!primero) {
        primero = alloc_cluster(v);
        if (!primero) return -1;
        if (dir_update(dlba, doff, primero, tam) < 0) return -1;
    }

    /* Llegar hasta el cluster donde cae 'offset', creando los que falten. */
    uint32_t c = primero;
    for (uint32_t saltar = offset / bytes_por_clus; saltar; saltar--) {
        uint32_t sig = siguiente_cluster(v, c);
        if (sig < 2 || sig >= v->eoc) {
            sig = alloc_cluster(v);
            if (!sig) return -1;
            if (fat_set(v, c, (uint32_t)sig) < 0) return -1;
        }
        c = sig;
    }

    uint32_t dentro = offset % bytes_por_clus;
    uint32_t hechos = 0;

    while (hechos < n) {
        uint32_t lba = v->data_lba + (c - 2) * v->sec_per_clus + dentro / 512;
        uint32_t en_sector = 512 - (dentro % 512);
        uint32_t trozo = n - hechos;
        if (trozo > en_sector) trozo = en_sector;

        /* Leer-modificar-escribir: el sector es la unidad minima que sabe
         * mover la tarjeta, asi que cambiar tres bytes obliga a traerse
         * los 512, tocarlos y devolverlos. */
        uint8_t *b = cached(lba);
        if (!b) return -1;
        for (uint32_t i = 0; i < trozo; i++)
            b[(dentro % 512) + i] = datos[hechos + i];
        if (escribir(lba, b) < 0) return -1;

        hechos += trozo;
        dentro += trozo;

        if (dentro >= bytes_por_clus && hechos < n) {
            uint32_t sig = siguiente_cluster(v, c);
            if (sig < 2 || sig >= v->eoc) {
                sig = alloc_cluster(v);
                if (!sig) return -1;
                if (fat_set(v, c, (uint32_t)sig) < 0) return -1;
            }
            c = sig;
            dentro = 0;
        }
    }

    /* Y lo ultimo, el tamanyo: hasta que no se apunta ahi, los bytes
     * escritos no existen para nadie. */
    if (offset + n > tam)
        return dir_update(dlba, doff, primero, offset + n);
    return 0;
}

/* Crear vacio, o vaciar lo que hubiera. */
static int fichero_crear(struct volumen *v, const char *nombre)
{
    uint32_t dlba, doff;

    if (dir_lookup(v, nombre, &dlba, &doff) == 0) {
        uint32_t primero, tam;
        dir_read(dlba, doff, &primero, &tam);
        if (primero) free_chain(v, primero);
        return dir_update(dlba, doff, 0, 0);
    }
    return dir_create(v, nombre, &dlba, &doff);
}

/* ¿Esta vacio este directorio?
 *
 * "Vacio" en FAT no quiere decir "sin entradas": un directorio recien
 * hecho ya trae "." y "..", que las pone quien lo crea. Asi que vacio es
 * "sin nada MAS que esas dos".
 *
 * Las ocultas SI cuentan. macOS deja un "._loquesea" al lado de cada
 * fichero y al listar se descartan, pero ocupan sitio de verdad: borrar
 * el directorio se los llevaria por delante sin avisar. Mejor negarse. */
static int dir_vacio(struct volumen *v, uint32_t cluster)
{
    for (uint32_t s = 0; ; s++) {
        uint32_t lba;
        if (dir_sector(v, cluster, s, &lba) < 0) return 1;   /* se acabo */

        uint8_t *b = cached(lba);
        if (!b) return 0;

        for (int e = 0; e < 512; e += 32) {
            uint8_t *d = b + e;
            if (d[0] == 0x00) return 1;           /* fin del directorio */
            if (d[0] == 0xE5) continue;           /* borrada */
            if ((d[11] & 0x0F) == 0x0F) continue; /* trozo de nombre largo */
            if (d[0] == '.') continue;            /* "." y ".." */
            return 0;                             /* algo hay */
        }
    }
}

/* Marcar una entrada como borrada. Es poner un 0xE5 en la primera letra
 * del nombre: el resto se queda ahi, y por eso se pueden recuperar
 * ficheros borrados. Nadie ha tocado ni los datos ni la cadena. */
static int entrada_borrar(uint32_t lba, uint32_t off)
{
    uint8_t *b = cached(lba);
    if (!b) return -1;
    b[off] = 0xE5;
    return escribir(lba, b);
}

/* En que numero de entrada cae un (sector, desplazamiento).
 *
 * Hace falta para llegar a las entradas de ANTES, que es donde vive la
 * cadena de nombre largo. Cada sector tiene un LBA distinto, asi que
 * basta con recorrer el directorio hasta encontrarlo. */
static int indice_de(struct volumen *v, uint32_t dir, uint32_t lba, uint32_t off)
{
    for (uint32_t s = 0; ; s++) {
        uint32_t l;
        if (dir_sector(v, dir, s, &l) < 0) return -1;
        if (l == lba) return (int)(s * 16 + off / 32);
    }
}

/* Borrar una entrada Y su cadena de nombre largo.
 *
 * Si se borrara solo la corta, los trozos de delante se quedarian ahi,
 * huerfanos: apuntando por su suma de comprobacion a un nombre que ya no
 * existe. fsck lo llama "orphaned long file name" y lo limpia, pero
 * mientras tanto cualquier sistema que los lea vera un nombre a medias.
 *
 * Se reconocen porque van pegados justo delante y repiten la suma del
 * nombre corto. En cuanto uno no cuadra, se para: lo de mas atras es de
 * otro. */
static int entrada_borrar_todo(struct volumen *v, uint32_t dir,
                               uint32_t lba, uint32_t off)
{
    int idx = indice_de(v, dir, lba, off);
    if (idx < 0) return entrada_borrar(lba, off);

    uint8_t *b = cached(lba);
    if (!b) return -1;

    /* La suma ANTES de tocar nada: cached() solo guarda un sector, y la
     * primera vuelta del bucle se lo lleva por delante. */
    uint8_t suma = suma_83(b + off);

    for (int i = idx - 1; i >= 0; i--) {
        uint32_t l, o;
        uint8_t *p = entrada_en(v, dir, (uint32_t)i, &l, &o);
        if (!p) break;
        if (p[o + 11] != 0x0F || p[o + 13] != suma) break;   /* no es suya */

        p[o] = 0xE5;
        if (escribir(l, p) < 0) return -1;
    }

    return entrada_borrar(lba, off);
}

/* Borrar un directorio vacio. */
static int dir_borrar(struct volumen *v, const char *ruta, int *motivo)
{
    uint32_t dir;
    char ultimo[FS_NAME_MAX];
    if (resolver(v, ruta, &dir, ultimo) < 0) { *motivo = FS_ERROR; return -1; }

    uint32_t lba, off;
    if (dir_lookup_en(v, dir, ultimo, 1, &lba, &off) < 0) { *motivo = FS_ERROR; return -1; }

    uint32_t cluster, tam;
    dir_read(lba, off, &cluster, &tam);

    if (!dir_vacio(v, cluster)) { *motivo = FS_NO_VACIO; return -1; }

    /* La entrada PRIMERO y la cadena despues. Al reves, un corte a mitad
     * dejaria una entrada apuntando a clusters que ya son de otro, que es
     * la peor de las dos formas de romperse: no se pierde un directorio,
     * se pierde lo que venga luego. */
    if (entrada_borrar_todo(v, dir, lba, off) < 0) { *motivo = FS_ERROR; return -1; }
    if (cluster) free_chain(v, cluster);
    return 0;
}

/* Mover o renombrar. Las dos cosas son la misma: escribir la entrada en
 * otro sitio y quitar la de antes. Los DATOS no se tocan; lo que cambia
 * es quien los nombra.
 *
 * De ahi que renombrar un fichero de un giga cueste lo mismo que uno de
 * cero bytes, y que mover "a otra carpeta" sea instantaneo mientras no se
 * cambie de volumen. Cuando se cambia de volumen ya no es un rename: es
 * copiar y borrar, y eso si cuesta lo que pesa. */
static int mover(struct volumen *v, const char *origen, const char *destino,
                 int *motivo)
{
    *motivo = FS_ERROR;

    uint32_t dir_o, dir_d;
    char nom_o[FS_NAME_MAX], nom_d[FS_NAME_MAX];

    if (resolver(v, origen,  &dir_o, nom_o) < 0) return -1;
    if (resolver(v, destino, &dir_d, nom_d) < 0) return -1;

    uint32_t lba_o, off_o;
    if (dir_lookup_en(v, dir_o, nom_o, -1, &lba_o, &off_o) < 0) return -1;

    /* Lo que hay que llevarse: donde empiezan los datos, cuanto miden, y
     * si es un directorio. */
    uint8_t *b = cached(lba_o);
    if (!b) return -1;

    uint32_t cluster = le16(b + off_o + 26);
    uint32_t tam     = le32(b + off_o + 28);
    uint8_t  attr    = b[off_o + 11];
    int      es_dir  = (attr & 0x10) ? 1 : 0;

    /* El destino tiene que estar libre. Machacarlo en silencio seria
     * perder un fichero por escribir mal una orden. */
    uint32_t lba_d, off_d;
    if (dir_lookup_en(v, dir_d, nom_d, -1, &lba_d, &off_d) == 0) {
        *motivo = FS_EXISTE;
        return -1;
    }

    /* Un directorio no se puede meter dentro de si mismo: quedaria un
     * bucle en el arbol, y cualquiera que lo recorriera no pararia. Se
     * comprueba subiendo desde el destino a ver si aparece el origen. */
    if (es_dir && cluster) {
        uint32_t subir = dir_d;
        for (int i = 0; i < 16 && subir; i++) {
            if (subir == cluster) return -1;      /* se mete en si mismo */
            uint32_t l, o;
            if (dir_lookup_en(v, subir, "..", -1, &l, &o) < 0) break;
            subir = entrada_cluster(l, o);
        }
    }

    /* La entrada nueva PRIMERO y la vieja despues. Si se corta la luz en
     * medio queda el mismo fichero con dos nombres, que es feo pero se
     * arregla; al reves, se habria perdido. Entre dos formas de romperse
     * se elige la que deja los datos alcanzables. */
    if (dir_create_en(v, dir_d, nom_d, attr, &lba_d, &off_d) < 0) return -1;
    if (dir_update(lba_d, off_d, cluster, tam) < 0) return -1;

    /* Si es un directorio y ha cambiado de padre, su ".." apuntaba al
     * antiguo. En FAT no hay indice de padres en ningun sitio: cada hijo
     * lleva escrito quien es el suyo, y por eso hay que ir a corregirlo. */
    if (es_dir && dir_o != dir_d && cluster) {
        uint32_t l, o;
        if (dir_lookup_en(v, cluster, "..", -1, &l, &o) == 0) {
            uint8_t *p = cached(l);
            if (!p) return -1;
            p[o + 26] = (uint8_t)(dir_d & 0xFF);
            p[o + 27] = (uint8_t)(dir_d >> 8);
            if (escribir(l, p) < 0) return -1;
        }
    }

    return entrada_borrar_todo(v, dir_o, lba_o, off_o);
}

static int fichero_borrar(struct volumen *v, const char *nombre)
{
    uint32_t dlba, doff;
    if (dir_lookup(v, nombre, &dlba, &doff) < 0) return -1;

    uint32_t primero, tam;
    dir_read(dlba, doff, &primero, &tam);
    if (primero) free_chain(v, primero);

    /* Borrar en FAT es poner un 0xE5 en la primera letra del nombre. El
     * resto de la entrada se queda ahi, y por eso se pueden recuperar
     * ficheros borrados: nadie ha tocado ni los datos ni la cadena. */
    uint32_t d; char u[FS_NAME_MAX];
    if (resolver(v, nombre, &d, u) < 0) return -1;

    return entrada_borrar_todo(v, d, dlba, doff);
}

/* --- El nombre, en formato 8.3 ---------------------------------------- */
/* "HOLA.TXT" se guarda en el disco como "HOLA    TXT": once bytes, sin
 * punto, rellenados con espacios y EN MAYUSCULAS. Convertir es la mitad
 * del trabajo de buscar un fichero.
 *
 * Las mayusculas se ponen aqui, y ese "aqui" importa: que FAT no distinga
 * mayusculas de minusculas es una regla del SISTEMA DE FICHEROS, no de
 * quien lo usa. Si la aplicara cada cliente, "cat hola.txt" funcionaria y
 * el siguiente programa que alguien escriba volveria a fallar. */
static char mayus(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static void a_8_3(const char *nombre, char out[11])
{
    for (int i = 0; i < 11; i++) out[i] = ' ';

    int i = 0, o = 0;
    while (nombre[i] && nombre[i] != '.' && o < 8)
        out[o++] = mayus(nombre[i++]);
    while (nombre[i] && nombre[i] != '.') i++;

    if (nombre[i] == '.') {
        i++;
        o = 8;
        while (nombre[i] && o < 11)
            out[o++] = mayus(nombre[i++]);
    }
}

/* --- Buscar y listar, ya con rutas ------------------------------------ */

/* "HOLA    TXT" de vuelta a "HOLA.TXT". */
static void de_8_3(const uint8_t *d, char salida[FS_NAME_MAX])
{
    int o = 0;
    for (int i = 0; i < 8 && d[i] != ' '; i++) salida[o++] = (char)d[i];
    if (d[8] != ' ') {
        salida[o++] = '.';
        for (int i = 8; i < 11 && d[i] != ' '; i++) salida[o++] = (char)d[i];
    }
    salida[o] = 0;
}

static void nombre_de_entrada(const struct lfn *l, const uint8_t *d,
                              char salida[FS_NAME_MAX])
{
    if (l->valido && l->nombre[0] && l->suma == suma_83(d)) {
        for (int i = 0; i < FS_NAME_MAX; i++) salida[i] = l->nombre[i];
        return;
    }
    de_8_3(d, salida);
}

/* Comparar nombres sin distinguir mayusculas, que es la regla de FAT. */
static int igual_sin_caja(const char *a, const char *b)
{
    for (;; a++, b++) {
        if (mayus(*a) != mayus(*b)) return 0;
        if (!*a) return 1;
    }
}

/* Localizar lo que nombra una ruta: fichero o directorio, da igual. */
static int buscar(struct volumen *v, const char *ruta, uint32_t *cluster, uint32_t *tam,
                  uint32_t *flags, uint64_t *mtime)
{
    /* El raiz no tiene entrada de directorio en ninguna parte: no es hijo
     * de nadie. Asi que no se puede buscar, hay que saberlo.
     *
     * Sin esto, preguntar por "/" daba error, y el sintoma aparecia lejos
     * del sitio: "cd .." desde el primer nivel no funcionaba, porque
     * chdir comprueba que el destino existe y es un directorio, y el
     * destino era el raiz. */
    int solo_barras = 1;
    for (const char *p = ruta; *p; p++) if (*p != '/') { solo_barras = 0; break; }
    if (ruta[0] == '/' && solo_barras) {
        *cluster = 0;
        *tam     = 0;
        if (flags) *flags = FS_ES_DIR;
        if (mtime) *mtime = 0;
        return 0;
    }

    uint32_t dir;
    char ultimo[FS_NAME_MAX];
    if (resolver(v, ruta, &dir, ultimo) < 0) return -1;

    uint32_t lba, off;
    if (dir_lookup_en(v, dir, ultimo, -1, &lba, &off) < 0) return -1;

    uint8_t *b = cached(lba);
    if (!b) return -1;

    *cluster = le16(b + off + 26);
    *tam     = le32(b + off + 28);
    if (flags) *flags = (b[off + 11] & 0x10) ? FS_ES_DIR : 0;
    if (mtime) *mtime = fecha_unix(le16(b + off + 24), le16(b + off + 22));
    return 0;
}

/* El cluster de un directorio dado por su ruta. La raiz es el caso
 * especial y por eso se mira aparte: "/" no tiene ultima componente que
 * buscar, y el cluster 0 no se corresponde con ningun dato del disco. */
static int resolver_dir(struct volumen *v, const char *ruta, uint32_t *dir)
{
    if (ruta[0] != '/') return -1;

    uint32_t cluster, tam, flags;
    uint64_t mtime;
    if (buscar(v, ruta, &cluster, &tam, &flags, &mtime) < 0) return -1;
    if (!(flags & FS_ES_DIR)) return -1;
    *dir = cluster;
    return 0;
}

/* La entrada numero 'indice' de un directorio.
 *
 * "." y ".." se saltan POR EL NOMBRE, y eso tiene su historia. Un
 * directorio hecho por TinyOS las lleva con atributo 0x10 (directorio) y
 * uno hecho por macOS con 0x12 (directorio + oculto). Como aqui se
 * descartan las ocultas -macOS deja un "._loquesea" al lado de cada
 * fichero y llenan el listado de basura-, las de macOS desaparecian y las
 * nuestras no: la MISMA entrada salia o no segun quien hubiera creado el
 * directorio.
 *
 * Mirar el nombre en vez del atributo es lo unico que da el mismo
 * resultado en los dos casos. Es un recordatorio util: en FAT los
 * atributos son una sugerencia que cada sistema rellena a su gusto, y
 * apoyarse en ellos para decidir QUE es algo sale caro. */
static int listar(struct volumen *v, uint32_t dir, uint32_t indice, struct fs_info *out)
{
    uint32_t vistas = 0;

    struct lfn l;
    lfn_reset(&l);

    for (uint32_t s = 0; ; s++) {
        uint32_t lba;
        if (dir_sector(v, dir, s, &lba) < 0) return -1;

        uint8_t *b = cached(lba);
        if (!b) return -1;

        for (int e = 0; e < 512; e += 32) {
            uint8_t *d = b + e;

            if (d[0] == 0x00) return -1;          /* fin del directorio */

            /* Las entradas de nombre largo se acumulan; la corta que viene
             * detras es la que las cobra. */
            if ((d[11] & 0x0F) == 0x0F) { lfn_add(&l, d); continue; }

            char nombre[FS_NAME_MAX];
            nombre_de_entrada(&l, d, nombre);
            lfn_reset(&l);

            if (d[0] == 0xE5) continue;           /* borrada            */
            if (d[11] & 0x08) continue;           /* etiqueta de volumen*/
            if (d[11] & 0x02) continue;           /* oculta: un Mac deja */
                                                  /* un "._loquesea" al  */
                                                  /* lado de cada fichero*/

            if (d[0] == '.') continue;            /* "." y "..", ver arriba */

            if (vistas++ != indice) continue;

            out->size  = le32(d + 28);
            out->flags = (d[11] & 0x10) ? FS_ES_DIR : 0;
            out->mtime = fecha_unix(le16(d + 24), le16(d + 22));
            for (int i = 0; i < FS_NAME_MAX; i++) out->name[i] = nombre[i];
            return 0;
        }
    }
}

/* Cuantas entradas de verdad tiene un directorio. Hace falta para saber
 * donde empiezan los puntos de montaje en el listado. */
static uint32_t cuantas_entradas(struct volumen *v, uint32_t dir)
{
    struct fs_info tmp;
    uint32_t n = 0;
    while (listar(v, dir, n, &tmp) == 0) n++;
    return n;
}

/* El punto de montaje numero 'i' que cuelga DIRECTAMENTE de 'padre'.
 *
 * "cuelga directamente" quiere decir que su punto empieza por la ruta del
 * padre y lo que sobra no lleva mas barras: /boot cuelga de /, pero
 * /a/b no colgaria de /. */
static int montaje_bajo(const char *padre, uint32_t i, struct fs_info *out)
{
    int largo_padre = 0;
    while (padre[largo_padre]) largo_padre++;
    int raiz = (largo_padre == 1 && padre[0] == '/');

    uint32_t vistos = 0;

    for (int k = 0; k < MAX_MONTAJES; k++) {
        struct montaje *m = &montajes[k];
        if (!m->punto[0] || !m->vol.montado) continue;
        if (m->punto[1] == 0) continue;              /* el raiz no cuelga */

        int desde = raiz ? 1 : largo_padre + 1;

        if (!raiz) {
            int casa = 1;
            for (int j = 0; j < largo_padre; j++)
                if (m->punto[j] != padre[j]) { casa = 0; break; }
            if (!casa || m->punto[largo_padre] != '/') continue;
        }

        const char *nom = m->punto + desde;
        int hay_barra = 0;
        for (const char *p = nom; *p; p++) if (*p == '/') hay_barra = 1;
        if (hay_barra || !*nom) continue;

        if (vistos++ != i) continue;

        out->size  = 0;
        out->flags = FS_ES_DIR;
        int o = 0;
        for (; nom[o] && o < FS_NAME_MAX - 1; o++) out->name[o] = nom[o];
        out->name[o] = 0;
        return 0;
    }
    return -1;
}

/* Crear un directorio.
 *
 * Un directorio es un fichero cuyo contenido son entradas de 32 bytes, asi
 * que crearlo es: pedir un cluster, ponerlo a ceros -el 0x00 de la primera
 * entrada es lo que marca "aqui se acaba"- y escribir dentro las dos
 * entradas que tiene todo directorio menos el raiz: "." apuntando a si
 * mismo y ".." apuntando al padre.
 *
 * Ese ".." es la unica forma que tiene FAT de subir un nivel: no hay
 * indice de padres en ningun sitio, esta escrito en cada hijo. Y el del
 * primer nivel apunta al cluster 0, que es como se dice "el raiz". */
static int dir_nuevo(struct volumen *v, const char *ruta)
{
    uint32_t padre;
    char nombre[FS_NAME_MAX];
    if (resolver(v, ruta, &padre, nombre) < 0) return -1;

    uint32_t lba, off;
    if (dir_lookup_en(v, padre, nombre, -1, &lba, &off) == 0) return -1;  /* ya existe */

    uint32_t c = alloc_cluster(v);
    if (!c) return -1;

    /* A ceros, entero. Un cluster reciclado trae la basura del fichero
     * anterior, y esa basura se leeria como entradas de directorio. */
    uint8_t vacio[512];
    for (int i = 0; i < 512; i++) vacio[i] = 0;
    for (uint32_t s = 0; s < v->sec_per_clus; s++)
        if (escribir(v->data_lba + (c - 2) * v->sec_per_clus + s, vacio) < 0) return -1;

    /* "." y "..", a mano, en el primer sector. */
    uint8_t *b = cached(v->data_lba + (c - 2) * v->sec_per_clus);
    if (!b) return -1;

    for (int i = 0; i < 11; i++) b[i] = ' ';
    b[0] = '.';
    b[11] = 0x10;
    b[26] = (uint8_t)(c & 0xFF);
    b[27] = (uint8_t)(c >> 8);

    for (int i = 32; i < 43; i++) b[i] = ' ';
    b[32] = '.'; b[33] = '.';
    b[43] = 0x10;
    b[58] = (uint8_t)(padre & 0xFF);
    b[59] = (uint8_t)(padre >> 8);

    if (escribir(v->data_lba + (c - 2) * v->sec_per_clus, b) < 0) return -1;

    /* Y ahora si, la entrada en el padre. La ultima, para que un fallo a
     * mitad no deje un directorio que se ve pero esta sin estrenar. */
    if (dir_create_en(v, padre, nombre, 0x10, &lba, &off) < 0) return -1;
    return dir_update(lba, off, c, 0);           /* los directorios miden 0 */
}

/* --- Seguir la cadena de clusters ------------------------------------- */
/* La tabla FAT es un array de enteros de 16 bits, uno por cluster: en la
 * casilla N esta el numero del cluster que va DESPUES del N. Un valor
 * >= v->eoc significa "aqui se acaba el fichero". */
static uint32_t siguiente_cluster(struct volumen *v, uint32_t c)
{
    uint32_t off = c * (v->fat32 ? 4 : 2);
    uint8_t *b = cached(v->fat_lba + off / 512);
    if (!b) return v->eoc;

    if (!v->fat32) return le16(b + (off % 512));

    /* Los cuatro bits de arriba de una casilla FAT32 estan RESERVADOS y no
     * son parte del numero. Hay que enmascararlos: si no, un volumen que
     * los traiga a uno da clusters astronomicos y la cadena se va a paseo.
     * Es el detalle que mas veces se olvida de FAT32. */
    return le32(b + (off % 512)) & 0x0FFFFFFF;
}

/* Leer hasta 'n' bytes del fichero que empieza en 'primero', saltandose
 * los primeros 'offset'. Devuelve cuantos ha leido. */
static int leer_fichero(struct volumen *v, uint32_t primero, uint32_t tam,
                        uint32_t offset, uint8_t *dst, uint32_t n)
{
    if (offset >= tam) return 0;
    if (offset + n > tam) n = tam - offset;

    uint32_t bytes_por_clus = v->sec_per_clus * 512;
    uint32_t c = primero;

    /* Saltar clusters enteros siguiendo la cadena. */
    for (uint32_t saltar = offset / bytes_por_clus; saltar; saltar--) {
        c = siguiente_cluster(v, c);
        if (c < 2 || c >= v->eoc) return 0;
    }

    uint32_t dentro = offset % bytes_por_clus;
    uint32_t hechos = 0;

    while (hechos < n) {
        if (c < 2 || c >= v->eoc) break;

        uint32_t lba = v->data_lba + (c - 2) * v->sec_per_clus + dentro / 512;
        uint8_t *b = cached(lba);
        if (!b) break;

        uint32_t en_sector = 512 - (dentro % 512);
        uint32_t trozo = n - hechos;
        if (trozo > en_sector) trozo = en_sector;

        for (uint32_t i = 0; i < trozo; i++)
            dst[hechos + i] = b[(dentro % 512) + i];

        hechos += trozo;
        dentro += trozo;

        if (dentro >= bytes_por_clus) {
            dentro = 0;
            c = siguiente_cluster(v, c);
        }
    }
    return (int)hechos;
}

/* ====================== EL SERVIDOR ================================== */

static struct message pet, resp;

static void responder(uint64_t puerto, uint64_t tipo, const void *datos, uint64_t n)
{
    resp.type = tipo;
    resp.len  = n;
    for (uint64_t i = 0; i < MSG_DATA_MAX; i++)
        resp.data[i] = (i < n) ? ((const char *)datos)[i] : 0;
    msg_send(puerto, &resp);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("\n  [fs] servidor de ficheros vivo en EL0\n");

    /* Lo PRIMERO, antes de tocar el hardware: quedarse con el puerto.
     *
     * Si ya hay un servidor sirviendolo, este sobra y tiene que irse sin
     * haber hecho nada. Arrancar la tarjeta primero seria reiniciar el
     * controlador EMMC -sd_init() hace SRST_HC- mientras el servidor que
     * ya funciona esta quiza a mitad de una lectura. El puerto hace aqui
     * de cerrojo entre procesos: quien lo tiene, manda. */
    int64_t puerto = port_create(PORT_FILES);
    if (puerto != PORT_FILES) {
        printf("  [fs] ya hay un servidor de ficheros: me voy\n");
        exit(1);
    }

    volatile uint32_t *emmc = (volatile uint32_t *)mmio_base();
    if (!emmc) {
        printf("  [fs] no tengo el MMIO del EMMC, no puedo trabajar\n");
        exit(1);
    }

    printf("  [fs] reloj base del EMMC segun la GPU: %lu Hz\n",
           clock_rate(CLK_EMMC));

    printf("  [fs] arrancando la tarjeta SD...\n");
    if (sd_init(emmc) < 0) {
        printf("  [fs] la tarjeta no arranca\n"
               "       controlador SDHCI version %lu\n"
               "       %s\n",
               (uint64_t)sd_host_version(), sd_last_error());
        exit(1);
    }

    if (montar() < 0) {
        printf("  [fs] no encuentro una particion FAT16 que entienda\n");
        exit(1);
    }

    printf("  [fs] tarjeta a %lu Hz\n", (uint64_t)sd_sd_clock());
    for (int i = 0; i < MAX_MONTAJES; i++) {
        struct montaje *m = &montajes[i];
        if (!m->punto[0]) continue;
        printf("  [fs] %-5s  FAT%d  %lu sectores/cluster  %lu clusters\n",
               m->punto, m->vol.fat32 ? 32 : 16,
               (uint64_t)m->vol.sec_per_clus,
               (uint64_t)m->vol.max_cluster);
    }

    for (;;) {
        if (msg_recv(PORT_FILES, &pet) < 0) continue;

        struct fs_request *r = (struct fs_request *)pet.data;
        uint64_t quien = r->port;
        uint32_t cluster = 0, tam = 0, flags = 0;
        uint64_t mtime = 0;

        r->name[FS_PATH_MAX - 1] = 0;         /* venga de donde venga */

        /* Lo PRIMERO es decidir en que volumen cae la ruta, y quedarse con
         * lo que sobra del punto de montaje. A partir de aqui todo lo
         * demas trabaja dentro de un solo sistema de ficheros y no sabe
         * que hay otro. */
        const char *ruta = r->name;
        struct volumen *v = volumen_de(r->name, &ruta);

        if (!v) { responder(quien, FS_ERROR, "", 0); continue; }

        switch (pet.type) {

        case FS_SIZE:
            if (buscar(v, ruta, &cluster, &tam, &flags, &mtime) < 0) {
                responder(quien, FS_ERROR, "", 0);
            } else {
                struct fs_info info;
                info.size  = tam;
                info.flags = flags;
                info.mtime = mtime;

                /* Solo la ultima componente: quien pregunta por
                 * "/DOCS/A.TXT" ya sabe la ruta, lo que no sabe es como
                 * quedo el nombre despues de pasar por 8.3. */
                uint32_t dir;
                if (resolver(v, ruta, &dir, info.name) < 0) info.name[0] = 0;

                responder(quien, FS_OK, &info, sizeof(info));
            }
            break;

        case FS_READ: {
            if (buscar(v, ruta, &cluster, &tam, &flags, &mtime) < 0 ||
                (flags & FS_ES_DIR)) {
                responder(quien, FS_ERROR, "", 0);
                break;
            }
            uint8_t trozo[FS_CHUNK];
            int n = leer_fichero(v, cluster, tam, (uint32_t)r->arg,
                                 trozo, FS_CHUNK);
            if (n <= 0) responder(quien, FS_EOF, "", 0);
            else        responder(quien, FS_OK, trozo, (uint64_t)n);
            break;
        }

        /* Ahora LIST lleva la ruta del directorio que se quiere listar, y
         * no solo el indice: el raiz ha dejado de ser el unico sitio. */
        case FS_LIST: {
            uint32_t dir;
            struct fs_info info;

            if (resolver_dir(v, ruta, &dir) < 0) {
                responder(quien, FS_ERROR, "", 0);
                break;
            }

            if (listar(v, dir, (uint32_t)r->arg, &info) == 0) {
                responder(quien, FS_OK, &info, sizeof(info));
                break;
            }

            /* Se acabaron las entradas de verdad. Quedan los PUNTOS DE
             * MONTAJE que cuelgan de este directorio, que no estan en el
             * disco de nadie: son de la tabla de montajes.
             *
             * Es la primera vez que este servidor ensenya algo que no ha
             * leido de un sector. En Unix el punto de montaje tiene que
             * existir como directorio en el volumen de abajo y queda
             * tapado; aqui no hay tal directorio, asi que se anyade. Mas
             * simple, y se ve mejor lo que es un montaje: un trozo de
             * nombre que lleva a otro sitio. */
            uint32_t vistas = cuantas_entradas(v, dir);
            uint32_t i = (uint32_t)r->arg - vistas;

            /* La ruta ENTERA, no la recortada. 'ruta' ya viene sin el
             * punto de montaje, asi que listar /boot preguntaria por los
             * montajes que cuelgan de "/" y se encontraria a si mismo. */
            if (montaje_bajo(r->name, i, &info) == 0)
                responder(quien, FS_OK, &info, sizeof(info));
            else
                responder(quien, FS_EOF, "", 0);
            break;
        }

        case FS_MKDIR:
            responder(quien, dir_nuevo(v, ruta) < 0 ? FS_ERROR : FS_OK,
                      "", 0);
            break;

        case FS_RMDIR: {
            int motivo = FS_ERROR;
            responder(quien, dir_borrar(v, ruta, &motivo) < 0 ? motivo : FS_OK,
                      "", 0);
            break;
        }

        /* El destino viaja en data[], porque en name[] solo cabe uno. */
        case FS_RENAME: {
            int motivo = FS_ERROR;
            r->data[FS_CHUNK - 1] = 0;

            /* Las dos rutas tienen que caer en el MISMO volumen. Mover
             * entre particiones no es renombrar: hay que copiar los
             * bytes, y eso lo hace "cp" y luego "rm". */
            const char *dest = r->data;
            struct volumen *vd = volumen_de(dest, &dest);

            if (vd != v) responder(quien, FS_ERROR, "", 0);
            else responder(quien,
                           mover(v, ruta, dest, &motivo) < 0 ? motivo : FS_OK,
                           "", 0);
            break;
        }

        case FS_WRITE: {
            uint32_t n = (uint32_t)pet.len;
            if (n > FS_CHUNK) n = FS_CHUNK;
            if (fichero_escribir(v, ruta, (uint32_t)r->arg,
                                 (const uint8_t *)r->data, n) < 0)
                responder(quien, FS_ERROR, "", 0);
            else
                responder(quien, FS_OK, "", 0);
            break;
        }

        case FS_CREATE:
            responder(quien, fichero_crear(v, ruta) < 0 ? FS_ERROR : FS_OK,
                      "", 0);
            break;

        case FS_DELETE: {
            /* Antes, "rm docs" decia "no existe", porque dir_lookup solo
             * mira ficheros y el directorio no aparecia. Era verdad a
             * medias y mandaba al sitio equivocado: el fichero SI esta,
             * lo que pasa es que no es un fichero. */
            uint32_t l, o;
            uint32_t d; char u[FS_NAME_MAX];

            if (resolver(v, ruta, &d, u) == 0 &&
                dir_lookup_en(v, d, u, 1, &l, &o) == 0) {
                responder(quien, FS_ES_DIRECTORIO, "", 0);
                break;
            }

            responder(quien, fichero_borrar(v, ruta) < 0 ? FS_ERROR : FS_OK,
                      "", 0);
            break;
        }

        default:
            responder(quien, FS_ERROR, "", 0);
            break;
        }
    }
}
