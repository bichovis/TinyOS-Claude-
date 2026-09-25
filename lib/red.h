/* lib/red.h - La red, para un programa cualquiera
 *
 * Por debajo todo son mensajes a la pila (PORT_RED, ver net_abi.h); esto es
 * la forma de llamarlo que un programa espera: abrir un enchufe UDP, mandar,
 * recibir con un tiempo de espera, cerrar, y resolver un nombre. Sin
 * descriptores: un enchufe es un entero pequenyo y no hay mas de cuatro por
 * programa, que para lo que hace un programa de TinyOS sobra.
 *
 * Las direcciones IP son un uint32_t en orden natural (192.168.1.11 es
 * 0xC0A8010B): ip_leer() y ip_texto() convierten.
 */
#pragma once
#include <stdint.h>

#define RED_SIEMPRE  0                 /* udp_recibir: esperar sin limite */

int  udp_abrir(int puerto_local);      /* 0 = el que sea; devuelve enchufe o -1 */
int  udp_puerto(int s);                /* el puerto UDP local de ese enchufe     */
int  udp_enviar(int s, uint32_t ip, int puerto, const void *datos, int n);
/* Bytes recibidos, 0 si se agoto la espera (en decimas de segundo), -1 si
 * fue mal. Rellena de donde vino. */
int  udp_recibir(int s, uint32_t *ip, int *puerto, void *datos, int max, int decimas);
void udp_cerrar(int s);

int  resolver(const char *nombre, uint32_t *ip);   /* 0 si va bien; errno dice por que */

/* Un ping: manda un eco y espera el suyo. Devuelve 1 si contesto (y rellena
 * el viaje en milisegundos y el TTL), 0 si se agoto la espera -que es lo que
 * significa "paquete perdido"- y -1 si la red dijo que no se puede llegar,
 * con el motivo en errno. 'seq' distingue esta respuesta de la anterior. */
int  ping(uint32_t ip, int seq, int bytes, int *ms, int *ttl, int decimas);

struct red_estado {
    uint32_t ip, mascara, router, dns, ntp;
    int      estado;                   /* 0 sin tarjeta, 1 buscando, 2 pidiendo, 3 con ip */
    uint8_t  mac[6];
    char     tarjeta[16];
};
int  red_estado(struct red_estado *e);

uint32_t ip_leer(const char *texto);   /* "1.2.3.4" -> numero; 0 si no lo es */
void     ip_texto(uint32_t ip, char *dst);   /* al menos 16 bytes */
