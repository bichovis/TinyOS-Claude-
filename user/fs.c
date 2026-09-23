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
    uint32_t num_fats     = secbuf[16];
    root_entries          = le16(secbuf + 17);
    uint32_t sec_por_fat  = le16(secbuf + 22);

    if (!sec_per_clus || !num_fats || !root_entries || !sec_por_fat)
        return -1;                                 /* esto seria FAT32 */

    fat_lba  = part_lba + reservados;
    root_lba = fat_lba + num_fats * sec_por_fat;
    data_lba = root_lba + (root_entries * 32 + 511) / 512;

    montado = 1;
    return 0;
}

/* --- El nombre, en formato 8.3 ---------------------------------------- */
/* "HOLA.TXT" se guarda en el disco como "HOLA    TXT": once bytes, sin
 * punto, rellenados con espacios. Convertir es la mitad del trabajo de
 * buscar un fichero. */
static void a_8_3(const char *nombre, char out[11])
{
    for (int i = 0; i < 11; i++) out[i] = ' ';

    int i = 0, o = 0;
    while (nombre[i] && nombre[i] != '.' && o < 8)
        out[o++] = nombre[i++];
    while (nombre[i] && nombre[i] != '.') i++;

    if (nombre[i] == '.') {
        i++;
        o = 8;
        while (nombre[i] && o < 11)
            out[o++] = nombre[i++];
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

void _start(void) __attribute__((section(".text.start")));

void _start(void)
{
    kprint("\n  [fs] servidor de ficheros vivo en EL0\n");

    /* Lo PRIMERO, antes de tocar el hardware: quedarse con el puerto.
     *
     * Si ya hay un servidor sirviendolo, este sobra y tiene que irse sin
     * haber hecho nada. Arrancar la tarjeta primero seria reiniciar el
     * controlador EMMC -sd_init() hace SRST_HC- mientras el servidor que
     * ya funciona esta quiza a mitad de una lectura. El puerto hace aqui
     * de cerrojo entre procesos: quien lo tiene, manda. */
    int64_t puerto = port_create(PORT_FILES);
    if (puerto != PORT_FILES) {
        kprint("  [fs] ya hay un servidor de ficheros: me voy\n");
        exit(1);
    }

    volatile uint32_t *emmc = (volatile uint32_t *)mmio_base();
    if (!emmc) {
        kprint("  [fs] no tengo el MMIO del EMMC, no puedo trabajar\n");
        exit(1);
    }

    {
        char n[24];
        uint64_t l = udec(n, clock_rate(CLK_EMMC));
        n[l] = 0;
        kprint("  [fs] reloj base del EMMC segun la GPU: ");
        kprint(n);
        kprint(" Hz\n");
    }
    kprint("  [fs] arrancando la tarjeta SD...\n");
    if (sd_init(emmc) < 0) {
        char n[16];
        uint64_t l = udec(n, sd_host_version());
        n[l] = 0;
        kprint("  [fs] la tarjeta no arranca\n       controlador SDHCI version ");
        kprint(n);
        kprint("\n       ");
        kprint(sd_last_error());
        kprint("\n");
        exit(1);
    }

    if (montar() < 0) {
        kprint("  [fs] no encuentro una particion FAT16 que entienda\n");
        exit(1);
    }

    {
        char n[24];
        uint64_t l = udec(n, sd_sd_clock());
        n[l] = 0;
        kprint("  [fs] tarjeta a ");
        kprint(n);
        kprint(" Hz\n");
    }

    kprint("  [fs] FAT16 montada\n");

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
                for (int i = 0; i < 32; i++) info.name[i] = r->name[i];
                responder(quien, FS_OK, &info, sizeof(info));
            }
            break;

        case FS_READ: {
            if (buscar(r->name, 0, &cluster, &tam, 0) < 0) {
                responder(quien, FS_ERROR, "", 0);
                break;
            }
            uint8_t trozo[MSG_DATA_MAX];
            int n = leer_fichero(cluster, tam, (uint32_t)r->arg,
                                 trozo, MSG_DATA_MAX);
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
                for (int i = 0; i < 32; i++)
                    info.name[i] = (i < 12) ? nombre[i] : 0;
                responder(quien, FS_OK, &info, sizeof(info));
            }
            break;

        default:
            responder(quien, FS_ERROR, "", 0);
            break;
        }
    }
}
