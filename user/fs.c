/* user/fs.c - Servidor de ficheros, en espacio de usuario
 *
 * Lee FAT16 de una tarjeta SD. No es parte del kernel: es un proceso de
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

/* --- Geometria del volumen, calculada al montar ----------------------- */
static uint32_t part_lba;        /* donde empieza la particion            */
static uint32_t fat_lba;         /* donde empieza la primera FAT          */
static uint32_t root_lba;        /* donde empieza el directorio raiz      */
static uint32_t data_lba;        /* donde empieza el primer cluster       */
static uint32_t sec_per_clus;
static uint32_t root_entries;
static uint32_t num_fats;        /* copias de la FAT: hay que escribirlas todas */
static uint32_t sec_por_fat;
static uint32_t max_cluster;     /* el numero mas alto que existe en este volumen */
static int      montado;

/* Un sector cacheado: leer de la SD es caro y casi todo son relecturas
 * del mismo sitio (el directorio, o la FAT). */
static uint8_t  cache[512];
static uint32_t cache_lba = 0xFFFFFFFF;

static uint8_t  secbuf[512];     /* para lecturas que no queremos cachear */

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
static int montar(void)
{
    if (leer(0, secbuf) < 0) return -1;
    if (le16(secbuf + 510) != 0xAA55) return -1;

    /* La tabla de particiones son cuatro entradas de 16 bytes a partir del
     * 446. Nos quedamos con la primera que sea FAT. */
    part_lba = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = secbuf + 446 + i * 16;
        uint8_t tipo = e[4];
        if (tipo == 0x01 || tipo == 0x04 || tipo == 0x06 ||
            tipo == 0x0B || tipo == 0x0C || tipo == 0x0E) {
            part_lba = le32(e + 8);
            break;
        }
    }
    if (!part_lba) return -1;

    /* El primer sector de la particion es el BPB, que describe el resto. */
    if (leer(part_lba, secbuf) < 0) return -1;
    if (le16(secbuf + 510) != 0xAA55) return -1;
    if (le16(secbuf + 11) != 512) return -1;      /* solo 512 b/sector */

    sec_per_clus          = secbuf[13];
    uint32_t reservados   = le16(secbuf + 14);
    num_fats              = secbuf[16];
    root_entries          = le16(secbuf + 17);
    sec_por_fat           = le16(secbuf + 22);

    if (!sec_per_clus || !num_fats || !root_entries || !sec_por_fat)
        return -1;                                 /* esto seria FAT32 */

    fat_lba  = part_lba + reservados;
    root_lba = fat_lba + num_fats * sec_por_fat;
    data_lba = root_lba + (root_entries * 32 + 511) / 512;

    /* Cuantos clusters hay de verdad. Hace falta para no inventarse uno al
     * buscar sitio libre: la FAT tiene mas casillas que clusters. */
    uint32_t total = le16(secbuf + 19);
    if (!total) total = le32(secbuf + 32);
    if (total <= (data_lba - part_lba)) return -1;

    max_cluster = (total - (data_lba - part_lba)) / sec_per_clus + 1;

    /* Y un tope duro: la tabla FAT tiene 256 casillas por sector, asi que
     * por muchos clusters que diga el BPB no puede haber mas de los que
     * caben en ella. Sin esta linea, un BPB raro haria que alloc_cluster
     * escribiera PASADA la tabla, encima del directorio raiz. Es
     * exactamente el tipo de fallo que no avisa: la tarjeta sigue
     * pareciendo correcta hasta que se pierde entera. */
    uint32_t cabe = sec_por_fat * 256;
    if (cabe < 2) return -1;
    if (max_cluster > cabe - 1) max_cluster = cabe - 1;

    montado = 1;
    return 0;
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
static uint32_t siguiente_cluster(uint32_t c);

/* Y estas tres tambien: las necesita el recorrido de directorios, que esta
 * antes, y se definen con el resto de la parte de lectura. */
static void nombre_de_entrada(const struct lfn *l, const uint8_t *d,
                              char salida[FS_NAME_MAX]);
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
static int fat_set(uint32_t c, uint16_t valor)
{
    if (c < 2 || c > max_cluster) return -1;

    uint32_t off = c * 2;
    for (uint32_t copia = 0; copia < num_fats; copia++) {
        uint32_t lba = fat_lba + copia * sec_por_fat + off / 512;

        uint8_t *b = cached(lba);
        if (!b) return -1;
        b[off % 512]     = (uint8_t)(valor & 0xFF);
        b[off % 512 + 1] = (uint8_t)(valor >> 8);

        if (escribir(lba, b) < 0) return -1;
    }
    return 0;
}

/* Buscar un cluster libre y marcarlo como "fin de fichero". Devuelve su
 * numero, o 0 si el volumen esta lleno. El 0 vale de "no hay" porque los
 * clusters 0 y 1 estan reservados y nunca se reparten. */
static uint32_t alloc_cluster(void)
{
    for (uint32_t c = 2; c <= max_cluster; c++) {
        if (siguiente_cluster(c) == 0) {
            if (fat_set(c, 0xFFFF) < 0) return 0;
            return c;
        }
    }
    return 0;
}

static void free_chain(uint32_t c)
{
    while (c >= 2 && c < 0xFFF8) {
        uint32_t sig = siguiente_cluster(c);
        fat_set(c, 0);
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
static int dir_sector(uint32_t dir, uint32_t n, uint32_t *lba)
{
    if (dir == 0) {                              /* el raiz */
        uint32_t sectores = (root_entries * 32 + 511) / 512;
        if (n >= sectores) return -1;
        *lba = root_lba + n;
        return 0;
    }

    uint32_t c = dir;
    for (uint32_t saltar = n / sec_per_clus; saltar; saltar--) {
        c = siguiente_cluster(c);
        if (c < 2 || c >= 0xFFF8) return -1;      /* se acabo la cadena */
    }
    *lba = data_lba + (c - 2) * sec_per_clus + (n % sec_per_clus);
    return 0;
}

/* Localizar una entrada dentro de un directorio: en que sector esta y en
 * que posicion. Con eso se puede leer y tambien MODIFICAR, que es lo que
 * hace falta para cambiarle el tamanyo.
 *
 * 'quiero': 0 solo ficheros, 1 solo directorios, -1 lo que sea. Hace
 * falta porque al recorrer una ruta las componentes de en medio TIENEN que
 * ser directorios, y la ultima no. */
static int dir_lookup_en(uint32_t dir, const char *nombre, int quiero,
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
        if (dir_sector(dir, s, &sl) < 0) return -1;

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
static int dir_create_en(uint32_t dir, const char *nombre, uint8_t attr,
                         uint32_t *lba, uint32_t *off)
{
    char patron[11];
    a_8_3(nombre, patron);

    for (uint32_t s = 0; ; s++) {
        uint32_t sl;
        if (dir_sector(dir, s, &sl) < 0) return -1;   /* directorio lleno */

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
static int resolver(const char *ruta, uint32_t *dir, char *ultimo)
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
        if (dir_lookup_en(actual, comp, 1, &lba, &off) < 0) return -1;
        actual = entrada_cluster(lba, off);
    }
}

/* Los dos de siempre, ahora encima de resolver(). */
static int dir_lookup(const char *ruta, uint32_t *lba, uint32_t *off)
{
    uint32_t dir;
    char ultimo[FS_NAME_MAX];
    if (resolver(ruta, &dir, ultimo) < 0) return -1;
    return dir_lookup_en(dir, ultimo, 0, lba, off);
}

static int dir_create(const char *ruta, uint32_t *lba, uint32_t *off)
{
    uint32_t dir;
    char ultimo[FS_NAME_MAX];
    if (resolver(ruta, &dir, ultimo) < 0) return -1;
    return dir_create_en(dir, ultimo, 0x20, lba, off);
}

/* Leer y modificar un campo de la entrada de directorio. */
static int dir_update(uint32_t lba, uint32_t off, uint32_t cluster, uint32_t tam)
{
    uint8_t *b = cached(lba);
    if (!b) return -1;

    uint8_t *d = b + off;
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
static int fichero_escribir(const char *nombre, uint32_t offset,
                            const uint8_t *datos, uint32_t n)
{
    uint32_t dlba, doff;
    if (dir_lookup(nombre, &dlba, &doff) < 0) {
        if (dir_create(nombre, &dlba, &doff) < 0) return -1;
    }

    uint32_t primero, tam;
    dir_read(dlba, doff, &primero, &tam);
    if (offset > tam) return -1;

    uint32_t bytes_por_clus = sec_per_clus * 512;

    /* Un fichero recien creado no tiene ni un cluster. */
    if (!primero) {
        primero = alloc_cluster();
        if (!primero) return -1;
        if (dir_update(dlba, doff, primero, tam) < 0) return -1;
    }

    /* Llegar hasta el cluster donde cae 'offset', creando los que falten. */
    uint32_t c = primero;
    for (uint32_t saltar = offset / bytes_por_clus; saltar; saltar--) {
        uint32_t sig = siguiente_cluster(c);
        if (sig < 2 || sig >= 0xFFF8) {
            sig = alloc_cluster();
            if (!sig) return -1;
            if (fat_set(c, (uint16_t)sig) < 0) return -1;
        }
        c = sig;
    }

    uint32_t dentro = offset % bytes_por_clus;
    uint32_t hechos = 0;

    while (hechos < n) {
        uint32_t lba = data_lba + (c - 2) * sec_per_clus + dentro / 512;
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
            uint32_t sig = siguiente_cluster(c);
            if (sig < 2 || sig >= 0xFFF8) {
                sig = alloc_cluster();
                if (!sig) return -1;
                if (fat_set(c, (uint16_t)sig) < 0) return -1;
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
static int fichero_crear(const char *nombre)
{
    uint32_t dlba, doff;

    if (dir_lookup(nombre, &dlba, &doff) == 0) {
        uint32_t primero, tam;
        dir_read(dlba, doff, &primero, &tam);
        if (primero) free_chain(primero);
        return dir_update(dlba, doff, 0, 0);
    }
    return dir_create(nombre, &dlba, &doff);
}

static int fichero_borrar(const char *nombre)
{
    uint32_t dlba, doff;
    if (dir_lookup(nombre, &dlba, &doff) < 0) return -1;

    uint32_t primero, tam;
    dir_read(dlba, doff, &primero, &tam);
    if (primero) free_chain(primero);

    /* Borrar en FAT es poner un 0xE5 en la primera letra del nombre. El
     * resto de la entrada se queda ahi, y por eso se pueden recuperar
     * ficheros borrados: nadie ha tocado ni los datos ni la cadena. */
    uint8_t *b = cached(dlba);
    if (!b) return -1;
    b[doff] = 0xE5;
    return escribir(dlba, b);
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
static int buscar(const char *ruta, uint32_t *cluster, uint32_t *tam,
                  uint32_t *flags)
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
        return 0;
    }

    uint32_t dir;
    char ultimo[FS_NAME_MAX];
    if (resolver(ruta, &dir, ultimo) < 0) return -1;

    uint32_t lba, off;
    if (dir_lookup_en(dir, ultimo, -1, &lba, &off) < 0) return -1;

    uint8_t *b = cached(lba);
    if (!b) return -1;

    *cluster = le16(b + off + 26);
    *tam     = le32(b + off + 28);
    if (flags) *flags = (b[off + 11] & 0x10) ? FS_ES_DIR : 0;
    return 0;
}

/* El cluster de un directorio dado por su ruta. La raiz es el caso
 * especial y por eso se mira aparte: "/" no tiene ultima componente que
 * buscar, y el cluster 0 no se corresponde con ningun dato del disco. */
static int resolver_dir(const char *ruta, uint32_t *dir)
{
    if (ruta[0] != '/') return -1;

    uint32_t cluster, tam, flags;
    if (buscar(ruta, &cluster, &tam, &flags) < 0) return -1;
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
static int listar(uint32_t dir, uint32_t indice, struct fs_info *out)
{
    uint32_t vistas = 0;

    struct lfn l;
    lfn_reset(&l);

    for (uint32_t s = 0; ; s++) {
        uint32_t lba;
        if (dir_sector(dir, s, &lba) < 0) return -1;

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
            for (int i = 0; i < FS_NAME_MAX; i++) out->name[i] = nombre[i];
            return 0;
        }
    }
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
static int dir_nuevo(const char *ruta)
{
    uint32_t padre;
    char nombre[FS_NAME_MAX];
    if (resolver(ruta, &padre, nombre) < 0) return -1;

    uint32_t lba, off;
    if (dir_lookup_en(padre, nombre, -1, &lba, &off) == 0) return -1;  /* ya existe */

    uint32_t c = alloc_cluster();
    if (!c) return -1;

    /* A ceros, entero. Un cluster reciclado trae la basura del fichero
     * anterior, y esa basura se leeria como entradas de directorio. */
    uint8_t vacio[512];
    for (int i = 0; i < 512; i++) vacio[i] = 0;
    for (uint32_t s = 0; s < sec_per_clus; s++)
        if (escribir(data_lba + (c - 2) * sec_per_clus + s, vacio) < 0) return -1;

    /* "." y "..", a mano, en el primer sector. */
    uint8_t *b = cached(data_lba + (c - 2) * sec_per_clus);
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

    if (escribir(data_lba + (c - 2) * sec_per_clus, b) < 0) return -1;

    /* Y ahora si, la entrada en el padre. La ultima, para que un fallo a
     * mitad no deje un directorio que se ve pero esta sin estrenar. */
    if (dir_create_en(padre, nombre, 0x10, &lba, &off) < 0) return -1;
    return dir_update(lba, off, c, 0);           /* los directorios miden 0 */
}

/* --- Seguir la cadena de clusters ------------------------------------- */
/* La tabla FAT es un array de enteros de 16 bits, uno por cluster: en la
 * casilla N esta el numero del cluster que va DESPUES del N. Un valor
 * >= 0xFFF8 significa "aqui se acaba el fichero". */
static uint32_t siguiente_cluster(uint32_t c)
{
    uint32_t off = c * 2;
    uint8_t *b = cached(fat_lba + off / 512);
    if (!b) return 0xFFFF;
    return le16(b + (off % 512));
}

/* Leer hasta 'n' bytes del fichero que empieza en 'primero', saltandose
 * los primeros 'offset'. Devuelve cuantos ha leido. */
static int leer_fichero(uint32_t primero, uint32_t tam,
                        uint32_t offset, uint8_t *dst, uint32_t n)
{
    if (offset >= tam) return 0;
    if (offset + n > tam) n = tam - offset;

    uint32_t bytes_por_clus = sec_per_clus * 512;
    uint32_t c = primero;

    /* Saltar clusters enteros siguiendo la cadena. */
    for (uint32_t saltar = offset / bytes_por_clus; saltar; saltar--) {
        c = siguiente_cluster(c);
        if (c < 2 || c >= 0xFFF8) return 0;
    }

    uint32_t dentro = offset % bytes_por_clus;
    uint32_t hechos = 0;

    while (hechos < n) {
        if (c < 2 || c >= 0xFFF8) break;

        uint32_t lba = data_lba + (c - 2) * sec_per_clus + dentro / 512;
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
            c = siguiente_cluster(c);
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
    printf("  [fs] FAT16 montada\n");

    for (;;) {
        if (msg_recv(PORT_FILES, &pet) < 0) continue;

        struct fs_request *r = (struct fs_request *)pet.data;
        uint64_t quien = r->port;
        uint32_t cluster = 0, tam = 0, flags = 0;

        r->name[FS_PATH_MAX - 1] = 0;         /* venga de donde venga */

        switch (pet.type) {

        case FS_SIZE:
            if (buscar(r->name, &cluster, &tam, &flags) < 0) {
                responder(quien, FS_ERROR, "", 0);
            } else {
                struct fs_info info;
                info.size  = tam;
                info.flags = flags;

                /* Solo la ultima componente: quien pregunta por
                 * "/DOCS/A.TXT" ya sabe la ruta, lo que no sabe es como
                 * quedo el nombre despues de pasar por 8.3. */
                uint32_t dir;
                if (resolver(r->name, &dir, info.name) < 0) info.name[0] = 0;

                responder(quien, FS_OK, &info, sizeof(info));
            }
            break;

        case FS_READ: {
            if (buscar(r->name, &cluster, &tam, &flags) < 0 ||
                (flags & FS_ES_DIR)) {
                responder(quien, FS_ERROR, "", 0);
                break;
            }
            uint8_t trozo[FS_CHUNK];
            int n = leer_fichero(cluster, tam, (uint32_t)r->arg,
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
            if (resolver_dir(r->name, &dir) < 0)
                responder(quien, FS_ERROR, "", 0);
            else if (listar(dir, (uint32_t)r->arg, &info) < 0)
                responder(quien, FS_EOF, "", 0);
            else
                responder(quien, FS_OK, &info, sizeof(info));
            break;
        }

        case FS_MKDIR:
            responder(quien, dir_nuevo(r->name) < 0 ? FS_ERROR : FS_OK,
                      "", 0);
            break;

        case FS_WRITE: {
            uint32_t n = (uint32_t)pet.len;
            if (n > FS_CHUNK) n = FS_CHUNK;
            if (fichero_escribir(r->name, (uint32_t)r->arg,
                                 (const uint8_t *)r->data, n) < 0)
                responder(quien, FS_ERROR, "", 0);
            else
                responder(quien, FS_OK, "", 0);
            break;
        }

        case FS_CREATE:
            responder(quien, fichero_crear(r->name) < 0 ? FS_ERROR : FS_OK,
                      "", 0);
            break;

        case FS_DELETE:
            responder(quien, fichero_borrar(r->name) < 0 ? FS_ERROR : FS_OK,
                      "", 0);
            break;

        default:
            responder(quien, FS_ERROR, "", 0);
            break;
        }
    }
}
