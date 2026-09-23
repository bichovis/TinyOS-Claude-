/* user/sd.h - Driver de la tarjeta SD, en espacio de usuario
 *
 * El controlador EMMC del BCM2837 es un SDHCI estandar de Arasan. Vive en
 * 0x3F300000 y el kernel nos concede esa pagina, igual que le concede la
 * PL011 al servidor de consola: a partir de ahi hablamos con el hardware
 * sin pasar por el kernel ni una vez.
 *
 * Solo sabe leer, y de uno en uno. Es suficiente para lo que hace falta y
 * deja el driver en algo que se puede leer de una sentada.
 */
#pragma once
#include <stdint.h>

int sd_init(volatile uint32_t *base);          /* 0 si la tarjeta responde */
int sd_read_block(uint64_t lba, void *dst);    /* 512 bytes, 0 si va bien  */
const char *sd_last_error(void);
uint32_t    sd_host_version(void);      /* decide como se codifica el reloj */
uint32_t    sd_base_clock(void);        /* Hz del reloj base del controlador*/
uint32_t    sd_sd_clock(void);          /* Hz a los que va la tarjeta       */
