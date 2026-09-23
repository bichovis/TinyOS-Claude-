# ==========================================================================
#  TinyOS - microkernel para Raspberry Pi 3B (BCM2837, Cortex-A53, AArch64)
# ==========================================================================

CROSS   ?= aarch64-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
AR      := $(CROSS)ar
OBJDUMP := $(CROSS)objdump
QEMU    ?= qemu-system-aarch64

BUILD   := build
SRCDIR  := src
INCDIR  := include

# -ffreestanding     : no hay libc; no asumas nada del entorno
# -nostdlib          : no enlaces libc ni el crt0 de arranque
# -mgeneral-regs-only: prohibido usar registros FP/SIMD (el kernel no los salva)
# -mstrict-align     : sin MMU la RAM es "device memory": accesos desalineados fallan
# -fno-pie           : direcciones absolutas, cargamos siempre en 0x80000
CFLAGS  := -Wall -Wextra -Werror -O2 -std=c11 \
           -ffreestanding -nostdlib -nostartfiles \
           -mcpu=cortex-a53 -mgeneral-regs-only -mstrict-align \
           -fno-stack-protector -fno-pie -fno-common \
           -I$(INCDIR) -MMD -MP

LDFLAGS := -nostdlib -nostartfiles -T linker.ld \
           -Wl,--gc-sections -Wl,--no-warn-rwx-segments -Wl,-Map,$(BUILD)/kernel8.map

# --- Programa de usuario: se compila aparte y se empotra en el kernel ---
UPROGS  := hello conserver client fs ls cat run sh write rm cp mem deep forkd trap kill upper wc fp

# Programas de usuario con mas de un fichero fuente
EXTRA_fs := user/sd.c
# Fijate en lo que NO esta aqui: -mgeneral-regs-only, que si lleva el
# kernel. Los programas de usuario pueden usar coma flotante y SIMD desde
# el paso 30; el kernel sigue sin poder, y eso es a proposito (ver fpu.h).
UCFLAGS := -Wall -Wextra -Werror -O2 -std=c11 -ffreestanding -nostdlib \
           -nostartfiles -mcpu=cortex-a53 -mstrict-align \
           -fno-stack-protector -fno-pie -fno-common \
           -ffunction-sections -fdata-sections -Iuser -Ilib -I$(INCDIR)
# -z max-page-size=4096 : sin esto el enlazador de AArch64 alinea los
#                         segmentos a 64 KB y el ELF engorda quince veces
# -s                    : fuera simbolos y secciones; al cargador no le
#                         hacen falta y ocupan mas que el programa
ULDFLAGS := -nostdlib -nostartfiles -T user/user.ld \
            -Wl,--no-warn-rwx-segments -Wl,-z,max-page-size=4096 -s \
            -Wl,--gc-sections

# --- La libc de los programas de usuario ------------------------------
# Una biblioteca de verdad: se archiva con ar y se enlaza al final. El
# enlazador saca de ella SOLO los objetos que hagan falta, asi que un
# programa que no use printf no lo lleva dentro.
LIBCSRC := lib/string.c lib/stdio.c lib/stdlib.c lib/malloc.c lib/signal.c
LIBCOBJ := $(patsubst lib/%.c,$(BUILD)/lib/%.o,$(LIBCSRC))
CRT0    := $(BUILD)/lib/crt0.o
LIBC    := $(BUILD)/libc.a

CSRCS   := $(wildcard $(SRCDIR)/*.c)
ASRCS   := $(wildcard $(SRCDIR)/*.S)
OBJS    := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(wildcard $(SRCDIR)/*.c)) \
           $(patsubst $(SRCDIR)/%.S,$(BUILD)/%.S.o,$(ASRCS)) \
           $(patsubst %,$(BUILD)/%_bin.o,$(UPROGS))
DEPS    := $(OBJS:.o=.d)

.PHONY: all clean run debug dump sdcard sd firmware sdtest
all: $(BUILD)/kernel8.img

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: $(SRCDIR)/%.S | $(BUILD)
	@echo "  AS    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# --- Cadena de los programas de usuario ---
# Cada uno se compila y enlaza por separado (en 0x80000000, ver user/user.ld),
# se pasa a binario plano y se empotra en el kernel como un array de C.
.PRECIOUS: $(BUILD)/%.elf $(BUILD)/%_bin.c

$(BUILD)/lib:
	@mkdir -p $(BUILD)/lib

$(BUILD)/lib/%.o: lib/%.c lib/stdio.h lib/string.h lib/stdlib.h | $(BUILD)/lib
	@echo "  CC-L  $<"
	@$(CC) $(UCFLAGS) -c $< -o $@

# string.c y solo string.c lleva un flag de mas.
# -ftree-loop-distribute-patterns reconoce un bucle de copia byte a byte y
# lo sustituye por una llamada a memcpy; dentro de memcpy eso es recursion
# infinita. Con -ffreestanding no llega a pasar, pero asi la correccion de
# memcpy no depende de un efecto secundario de otro flag. Ver lib/string.c.
$(BUILD)/lib/string.o: lib/string.c lib/string.h | $(BUILD)/lib
	@echo "  CC-L  $< (sin reconocimiento de patrones)"
	@$(CC) $(UCFLAGS) -fno-tree-loop-distribute-patterns -c $< -o $@

$(CRT0): lib/crt0.S | $(BUILD)/lib
	@echo "  AS-L  $<"
	@$(CC) $(UCFLAGS) -c $< -o $@

$(LIBC): $(LIBCOBJ)
	@echo "  AR    $@"
	@rm -f $@
	@$(AR) rcs $@ $(LIBCOBJ)

# El crt0 va SUELTO y delante; la libc va archivada y al final. El orden
# importa: el enlazador recorre los archivos una vez y solo saca de ellos
# lo que ya sabe que le falta, asi que una biblioteca puesta antes que
# quien la usa no aporta nada.
$(BUILD)/%.elf: user/%.c user/syscall.h user/sd.c user/sd.h \
                $(CRT0) $(LIBC) \
                $(INCDIR)/ipc_abi.h $(INCDIR)/fs_abi.h user/user.ld | $(BUILD)
	@echo "  CC-U  user/$*.c"
	@$(CC) $(UCFLAGS) $(ULDFLAGS) $(CRT0) user/$*.c $(EXTRA_$*) $(LIBC) -o $@

# Lo que se empotra en el kernel es el ELF tal cual. Antes era un binario
# plano con una cabecera que nos habiamos inventado; ahora el formato ya
# trae donde va cada tramo y con que permisos, asi que no hay nada que
# traducir.
$(BUILD)/%_bin.c: $(BUILD)/%.elf tools/bin2c.py
	@echo "  BIN2C $@"
	@python3 tools/bin2c.py $< $@ user_$*

$(BUILD)/%_bin.o: $(BUILD)/%_bin.c
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel8.elf: $(OBJS) linker.ld
	@echo "  LD    $@"
	@$(CC) $(LDFLAGS) $(OBJS) -o $@

# El firmware de la Pi no entiende ELF: necesita un binario plano.
$(BUILD)/kernel8.img: $(BUILD)/kernel8.elf
	@echo "  IMG   $@"
	@$(OBJCOPY) -O binary $< $@
	@echo "  ---> $$(wc -c < $@) bytes"

$(BUILD):
	@mkdir -p $(BUILD)

# Una imagen de tarjeta para probar el servidor de ficheros sin tocar la SD
# de verdad: FAT16 con tabla de particiones, como la placa.
sdtest: all | $(BUILD)
	@rm -f $(BUILD)/sd.img
	@dd if=/dev/zero of=$(BUILD)/sd.img bs=1m count=64 2>/dev/null
	@DEV=$$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage \
	        $(BUILD)/sd.img 2>/dev/null | head -1 | awk '{print $$1}');       \
	 diskutil eraseDisk "MS-DOS FAT16" TINYOS MBRFormat $$DEV >/dev/null;     \
	 printf 'Hola desde la tarjeta SD.\nEste fichero lo ha puesto un Mac y lo va a leer TinyOS.\n' > /Volumes/TINYOS/HOLA.TXT; \
	 for p in hello ls cat run write rm cp mem deep forkd trap kill upper wc fp; do \
	   cp $(BUILD)/$$p.elf /Volumes/TINYOS/$$(echo $$p | tr a-z A-Z).ELF; \
	 done;                         \
	 sync; diskutil eject $$DEV >/dev/null
	@echo "  $(BUILD)/sd.img lista (FAT16, con HOLA.TXT y HELLO.ELF)"
	@echo "  'make run' la usa automaticamente."


SDIMG ?= $(BUILD)/sd.img
SDOPT  = $(if $(wildcard $(SDIMG)),-drive file=$(SDIMG)$(,)if=sd$(,)format=raw,)
, := ,

# QEMU emula la raspi3b; la PL011 sale por stdio.
run: $(BUILD)/kernel8.elf
	$(QEMU) -M raspi3b -kernel $< -serial stdio -serial null -display none $(SDOPT)

# Igual pero esperando a gdb en el puerto 1234 (aarch64-elf-gdb, target remote :1234)
debug: $(BUILD)/kernel8.elf
	$(QEMU) -M raspi3b -kernel $< -serial stdio -serial null -display none -S -s

dump: $(BUILD)/kernel8.elf
	@$(OBJDUMP) -d $<

# ====================== Arranque en hardware real ======================
SD ?= /Volumes/BOOT

# Descarga el firmware propietario de Broadcom (solo la primera vez)
firmware:
	@sh tools/fetch-firmware.sh $(BUILD)/sdcard

# Prepara en build/sdcard todo lo que hay que copiar a la particion FAT32
sdcard: all firmware
	@cp config.txt $(BUILD)/sdcard/
	@cp $(BUILD)/kernel8.img $(BUILD)/sdcard/
	@# Para el servidor de ficheros: algo que leer y algo que ejecutar.
	@for p in hello ls cat run write rm cp mem deep forkd trap kill upper wc fp; do \
	   cp $(BUILD)/$$p.elf $(BUILD)/sdcard/$$(echo $$p | tr a-z A-Z).ELF; \
	 done
	@printf 'Hola desde la tarjeta SD.\nEste fichero esta en la particion de arranque de la Pi.\n' > $(BUILD)/sdcard/HOLA.TXT
	@echo
	@echo "Listo en $(BUILD)/sdcard:"
	@ls -l $(BUILD)/sdcard
	@echo
	@echo "Copialo a una SD con una particion FAT32, o usa:  make sd SD=/Volumes/TUSD"

# Copia directamente a una SD ya montada.
# 'cp -R .../.' y no 'cp .../*': hay un subdirectorio (overlays/) y el glob
# solo pasa nombres, asi que un cp a secas se lo salta y falla.
sd: sdcard
	@test -d "$(SD)" || { echo "No existe $(SD). Usa: make sd SD=/Volumes/TUSD"; exit 1; }
	@cp -R $(BUILD)/sdcard/. "$(SD)/"
	@sync
	@echo "Copiado a $(SD):"
	@ls -R "$(SD)" | head -20
	@echo "Expulsala y arranca la Pi."

clean:
	@rm -rf $(BUILD)

-include $(DEPS)
