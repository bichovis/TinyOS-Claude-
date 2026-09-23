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

/* Las dos viven mas abajo, con la parte de lectura, porque las comparten
 * los dos lados. */
static void     a_8_3(const char *nombre, char out[11]);
static uint32_t siguiente_cluster(uint32_t c);


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

/* Localizar la entrada de directorio de un fichero: en que sector esta y
 * en que posicion dentro de el. Con eso se puede leer y tambien MODIFICAR,
 * que es lo que hace falta para cambiarle el tamanyo. */
static int dir_lookup(const char *nombre, uint32_t *lba, uint32_t *off)
{
    char patron[11];
    a_8_3(nombre, patron);

    uint32_t sectores = (root_entries * 32 + 511) / 512;
    for (uint32_t s = 0; s < sectores; s++) {
        uint8_t *b = cached(root_lba + s);
        if (!b) return -1;

        for (int e = 0; e < 512; e += 32) {
            uint8_t *d = b + e;
            if (d[0] == 0x00) return -1;
            if (d[0] == 0xE5) continue;
            if ((d[11] & 0x0F) == 0x0F) continue;
            if (d[11] & (0x08 | 0x10)) continue;

            int igual = 1;
            for (int i = 0; i < 11; i++)
                if (d[i] != (uint8_t)patron[i]) { igual = 0; break; }
            if (!igual) continue;

            *lba = root_lba + s;
            *off = (uint32_t)e;
            return 0;
        }
    }
    return -1;
}

/* Crear una entrada nueva. Se reaprovecha la primera casilla borrada que
 * aparezca, y si no hay ninguna se usa la primera sin estrenar. */
static int dir_create(const char *nombre, uint32_t *lba, uint32_t *off)
{
    char patron[11];
    a_8_3(nombre, patron);

    uint32_t sectores = (root_entries * 32 + 511) / 512;
    for (uint32_t s = 0; s < sectores; s++) {
        uint8_t *b = cached(root_lba + s);
        if (!b) return -1;

        for (int e = 0; e < 512; e += 32) {
            uint8_t *d = b + e;
            if (d[0] != 0x00 && d[0] != 0xE5) continue;

            int ultima = (d[0] == 0x00);

            for (int i = 0; i < 11; i++) d[i] = (uint8_t)patron[i];
            for (int i = 11; i < 32; i++) d[i] = 0;
            d[11] = 0x20;                       /* archivo normal */

            /* Si esta era la marca de "aqui se acaba el directorio", la
             * siguiente casilla tiene que heredarla o el recorrido no
             * pararia nunca. */
            if (ultima && e + 32 < 512) d[32] = 0x00;

            if (escribir(root_lba + s, b) < 0) return -1;

            *lba = root_lba + s;
            *off = (uint32_t)e;
            return 0;
        }
    }
    return -1;                                   /* directorio raiz lleno */
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

/* --- Buscar en el directorio raiz ------------------------------------- */
/* Devuelve 0 y rellena cluster/tamanyo, o -1 si no esta. Si 'nombre' es 0,
 * devuelve la entrada numero 'indice' (para listar). */
static int buscar(const char *nombre, uint32_t indice,
                  uint32_t *cluster, uint32_t *tam, char salida[12])
{
    char patron[11];
    if (nombre) a_8_3(nombre, patron);

    uint32_t vistas = 0;
    uint32_t sectores = (root_entries * 32 + 511) / 512;

    for (uint32_t s = 0; s < sectores; s++) {
        uint8_t *b = cached(root_lba + s);
        if (!b) return -1;

        for (int e = 0; e < 512; e += 32) {
            uint8_t *d = b + e;

            if (d[0] == 0x00) return -1;          /* fin del directorio */
            if (d[0] == 0xE5) continue;           /* borrada            */
            if ((d[11] & 0x0F) == 0x0F) continue; /* nombre largo       */
            if (d[11] & 0x08) continue;           /* etiqueta de volumen*/
            if (d[11] & 0x10) continue;           /* subdirectorio      */
            if (d[11] & 0x02) continue;           /* oculta: un Mac deja */
                                                  /* un "._loquesea" al  */
                                                  /* lado de cada fichero*/

            if (nombre) {
                int igual = 1;
                for (int i = 0; i < 11; i++)
                    if (d[i] != (uint8_t)patron[i]) { igual = 0; break; }
                if (!igual) continue;
            } else if (vistas++ != indice) {
                continue;
            }

            *cluster = le16(d + 26);
            *tam     = le32(d + 28);

            if (salida) {
                int o = 0;
                for (int i = 0; i < 8 && d[i] != ' '; i++) salida[o++] = (char)d[i];
                if (d[8] != ' ') {
                    salida[o++] = '.';
                    for (int i = 8; i < 11 && d[i] != ' '; i++)
                        salida[o++] = (char)d[i];
                }
                salida[o] = 0;
            }
            return 0;
        }
    }
    return -1;
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
        uint32_t cluster = 0, tam = 0;
        char nombre[12];

        switch (pet.type) {

        case FS_SIZE:
            if (buscar(r->name, 0, &cluster, &tam, 0) < 0) {
                responder(quien, FS_ERROR, "", 0);
            } else {
                struct fs_info info;
                info.size = tam;
                for (int i = 0; i < FS_NAME_MAX; i++) info.name[i] = r->name[i];
                responder(quien, FS_OK, &info, sizeof(info));
            }
            break;

        case FS_READ: {
            if (buscar(r->name, 0, &cluster, &tam, 0) < 0) {
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

        case FS_LIST:
            if (buscar(0, (uint32_t)r->arg, &cluster, &tam, nombre) < 0) {
                responder(quien, FS_EOF, "", 0);
            } else {
                struct fs_info info;
                info.size = tam;
                for (int i = 0; i < FS_NAME_MAX; i++)
                    info.name[i] = (i < 12) ? nombre[i] : 0;
                responder(quien, FS_OK, &info, sizeof(info));
            }
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
