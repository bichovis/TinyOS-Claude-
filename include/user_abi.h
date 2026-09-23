/* user_abi.h - La cabecera de una imagen de proceso
 *
 * El kernel recibe un binario PLANO: una tira de bytes sin secciones ni
 * simbolos, porque objcopy se los ha comido. Mirandolo no hay forma de
 * saber donde acaba el codigo y empiezan los datos, y esa diferencia es
 * justo la que decide los permisos de cada pagina.
 *
 * Asi que el programa lo dice de su puno y letra: los primeros 48 bytes de
 * la imagen son esta estructura, que rellena el enlazador con los simbolos
 * de user/user.ld.
 *
 * Son direcciones absolutas y no tamanos porque el ensamblador no puede
 * restar dos simbolos que todavia no existen cuando el ensambla; restarlos
 * es trabajo del kernel, que para eso ya sabe donde empieza la imagen.
 *
 *   [text_start, text_end)   solo lectura, ejecutable   <- viene de la imagen
 *   [text_end,   data_end)   lectura/escritura          <- viene de la imagen
 *   [data_end,   bss_end )   lectura/escritura          <- ceros
 *
 * Los bytes los emite user/header.S. Si cambias un campo aqui, cambialo
 * alli: son el mismo contrato visto desde los dos lados.
 */
#pragma once
#include <stdint.h>

#define USER_MAGIC    0x55534F54UL      /* "TOSU" en little endian */
#define USER_ABI_VER  1

struct user_header {
    uint32_t magic;
    uint32_t version;
    uint64_t entry;         /* VA por la que empezar a ejecutar           */
    uint64_t text_start;    /* principio de la imagen (== USER_BASE)      */
    uint64_t text_end;      /* fin del tramo ejecutable, en frontera de   */
                            /* pagina: una pagina no puede ser medio      */
                            /* ejecutable                                 */
    uint64_t data_end;      /* fin de lo que hay que copiar de la imagen  */
    uint64_t bss_end;       /* fin del tramo escribible                   */
};
