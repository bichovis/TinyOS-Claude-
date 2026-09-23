/* elf.h - Lo justo de ELF64 para cargar un programa
 *
 * En el paso 12 nos inventamos una cabecera propia (48 bytes, magia
 * "TOSU") para decirle al kernel donde acababa el codigo y empezaban los
 * datos. Funcionaba. Pero ese problema ya estaba resuelto: ELF lleva
 * haciendolo desde 1999, lo emite el enlazador sin que se lo pidamos, y
 * ademas trae los PERMISOS de cada tramo, que nosotros tuvimos que deducir
 * por convenio.
 *
 * De ELF solo hace falta una parte minuscula. Un fichero ejecutable tiene
 * una cabecera y una lista de "program headers", y de esos solo importan
 * los de tipo PT_LOAD: cada uno dice "coge estos bytes del fichero, ponlos
 * en esta direccion, rellena el resto con ceros, y dale estos permisos".
 * Todo lo demas -tabla de simbolos, secciones, reubicaciones- es para el
 * enlazador y para el depurador, no para quien ejecuta.
 *
 * El detalle mas elegante: p_memsz puede ser mayor que p_filesz, y esa
 * diferencia es exactamente .bss. No hace falta decir nada mas.
 */
#pragma once
#include <stdint.h>

struct elf64_ehdr {
    uint8_t  e_ident[16];    /* 0x7F 'E' 'L' 'F', clase, endianness...   */
    uint16_t e_type;         /* 2 = ejecutable                            */
    uint16_t e_machine;      /* 183 = AArch64                             */
    uint32_t e_version;
    uint64_t e_entry;        /* por donde empezar                         */
    uint64_t e_phoff;        /* donde esta la lista de program headers    */
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;    /* tamanyo de cada uno                       */
    uint16_t e_phnum;        /* cuantos hay                               */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;         /* 1 = PT_LOAD, lo unico que miramos         */
    uint32_t p_flags;        /* permisos: X=1, W=2, R=4                   */
    uint64_t p_offset;       /* desde donde copiar, en el fichero         */
    uint64_t p_vaddr;        /* a donde va en el espacio del proceso      */
    uint64_t p_paddr;
    uint64_t p_filesz;       /* cuantos bytes hay en el fichero           */
    uint64_t p_memsz;        /* cuantos ocupa en memoria (lo de mas, .bss)*/
    uint64_t p_align;
};

#define ELF_CLASS64     2
#define ELF_DATA_LSB    1
#define ET_EXEC         2
#define EM_AARCH64      183
#define PT_LOAD         1

#define PF_X            1
#define PF_W            2
#define PF_R            4
