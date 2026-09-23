# ==========================================================================
#  TinyOS - microkernel para Raspberry Pi 3B (BCM2837, Cortex-A53, AArch64)
# ==========================================================================

CROSS   ?= aarch64-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
AR      := $(CROSS)ar

# La fecha con la que nace el reloj del kernel, en HORA LOCAL y no en UTC.
#
# FAT no tiene zona horaria: guarda la hora del sitio donde se escribio el
# fichero, y punto. Si el kernel arrancara en UTC, todo lo que escribiera
# TinyOS apareceria dos horas mas viejo que lo que escribio el Mac hace un
# momento... y make, que compara fechas, haria justo lo contrario de lo que
# se le pide. Un reloj mal puesto no es un detalle cosmetico cuando alguien
# ordena cosas con el.
FECHA_LOCAL := $(shell python3 -c "import time; print(int(time.time()) + time.localtime().tm_gmtoff)")
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
           -I$(INCDIR) -MMD -MP \
           -DFECHA_COMPILACION=$(FECHA_LOCAL)UL

LDFLAGS := -nostdlib -nostartfiles -T linker.ld \
           -Wl,--gc-sections -Wl,--no-warn-rwx-segments -Wl,-Map,$(BUILD)/kernel8.map

# --- Programa de usuario: se compila aparte y se empotra en el kernel ---
UPROGS  := hello conserver client fs ls cat run sh write rm cp mem deep forkd trap kill upper wc fp mkdir map rmdir mv env echo malo init fecha libc

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
LIBCASM := lib/setjmp.S
LIBCSRC := lib/errno.c lib/file.c lib/string.c lib/stdio.c lib/stdlib.c lib/malloc.c lib/signal.c
LIBCOBJ := $(patsubst lib/%.c,$(BUILD)/lib/%.o,$(LIBCSRC)) \
           $(patsubst lib/%.S,$(BUILD)/lib/%.S.o,$(LIBCASM))
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

$(BUILD)/lib/%.S.o: lib/%.S | $(BUILD)/lib
	@echo "  AS-L  $<"
	@$(CC) $(UCFLAGS) -c $< -o $@

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
# Una imagen con LAS DOS PARTICIONES de la tarjeta de verdad: BOOT en
# FAT16 (donde iria el firmware) y DATA en FAT32 (que es el raiz).
#
# 512 MB no es capricho: FAT32 exige 65.525 clusters como minimo, y por
# debajo de eso las herramientas formatean FAT16 aunque les pidas FAT32.
# El tipo de un volumen FAT es una consecuencia de su tamanyo.
sdtest: all | $(BUILD)
	@rm -f $(BUILD)/sd.img
	@dd if=/dev/zero of=$(BUILD)/sd.img bs=1m count=512 2>/dev/null
	@# Los puntos de montaje se PREGUNTAN, nunca se adivinan.
	@#
	@# Aqui ponia /Volumes/BOOT y /Volumes/DATA a pelo, y eso es una trampa
	@# con dientes: si hay una tarjeta de verdad puesta en el Mac con esos
	@# mismos nombres -que es justo lo que pasa cuando estas trabajando en
	@# esto- macOS monta la imagen con otro nombre y el script escribe en la
	@# TARJETA. Salio bien de milagro; podria haber borrado algo.
	@DEV=$$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage \
	        $(BUILD)/sd.img 2>/dev/null | head -1 | awk '{print $$1}');       \
	 test -n "$$DEV" || { echo "  no he podido montar la imagen"; exit 1; };  \
	 diskutil partitionDisk $$DEV MBR                                         \
	          "MS-DOS FAT16" BOOT 64M "MS-DOS FAT32" DATA R >/dev/null;       \
	 B=$$(diskutil info -plist $${DEV}s1 | plutil -extract MountPoint raw -); \
	 D=$$(diskutil info -plist $${DEV}s2 | plutil -extract MountPoint raw -); \
	 case "$$B$$D" in /Volumes/*) ;; *)                                       \
	   echo "  puntos de montaje raros: '$$B' '$$D'";                          \
	   diskutil eject $$DEV >/dev/null; exit 1;; esac;                        \
	 echo "  BOOT en $$B, DATA en $$D";                                       \
	 cp config.txt "$$B/";                                                    \
	 cp $(BUILD)/kernel8.img "$$B/";                                          \
	 printf 'Soy el de la particion de arranque.\n' > "$$B/AVISO.TXT";        \
	 printf 'Hola desde la tarjeta SD.\nEste fichero lo ha puesto un Mac y lo va a leer TinyOS.\n' > "$$D/HOLA.TXT"; \
	 mkdir -p "$$D/DOCS/NOTAS"; \
	 printf 'Estoy dentro de un subdirectorio.\n' > "$$D/DOCS/LEEME.TXT"; \
	 printf 'Y yo dos niveles abajo.\n' > "$$D/DOCS/NOTAS/HONDO.TXT"; \
	 printf 'Mi nombre no cabe en 8.3.\n' > "$$D/un nombre bastante largo.txt"; \
	 printf 'Y el mio tampoco, pero sin espacios.\n' > "$$D/ensamblador-de-prueba.txt"; \
	 mkdir -p "$$D/ETC"; \
	 printf '# /etc/rc - lo que lee init al arrancar\n# Cada linea NOMBRE=valor se mete en el entorno, y de ahi se hereda\n# a todo lo que se ejecute. Cambiar el PATH es editar esto, no\n# recompilar el sistema operativo.\nPATH=/usr/bin:.\nHOME=/\nTERM=serie\nSISTEMA=TinyOS\n' > "$$D/ETC/RC"; \
	 mkdir -p "$$D/USR/BIN"; \
	 for p in hello ls cat run write rm cp mem deep forkd trap kill upper wc fp mkdir map rmdir mv env echo malo fecha libc malo; do \
	   cp $(BUILD)/$$p.elf "$$D/USR/BIN/$$(echo $$p | tr a-z A-Z).ELF"; \
	 done;                         \
	 sync; diskutil eject $$DEV >/dev/null
	@echo "  $(BUILD)/sd.img lista: BOOT (FAT16) -> /boot, DATA (FAT32) -> /"
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
# Ahora la tarjeta tiene dos particiones y cada una lleva cosas distintas:
#   build/sdcard -> BOOT (FAT16): firmware, config.txt y el kernel
#   build/sddata -> DATA (FAT32): los programas y los datos, o sea el raiz
sdcard: all firmware
	@rm -rf $(BUILD)/sddata && mkdir -p $(BUILD)/sddata
	@# Los programas vivian aqui hasta el paso 35 y ahora van en DATA.
	@# Sin esto se quedarian, y la Pi arrancaria con dos copias de cada uno.
	@rm -f $(BUILD)/sdcard/*.ELF $(BUILD)/sdcard/HOLA.TXT
	@cp config.txt $(BUILD)/sdcard/
	@cp $(BUILD)/kernel8.img $(BUILD)/sdcard/
	@printf 'Soy el de la particion de arranque, y cuelgo de /boot.\n' > $(BUILD)/sdcard/AVISO.TXT
	@mkdir -p $(BUILD)/sddata/ETC
	@printf '# /etc/rc - lo que lee init al arrancar\n# Cada linea NOMBRE=valor se mete en el entorno, y de ahi se hereda\n# a todo lo que se ejecute. Cambiar el PATH es editar esto, no\n# recompilar el sistema operativo.\nPATH=/usr/bin:.\nHOME=/\nTERM=serie\nSISTEMA=TinyOS\n' > $(BUILD)/sddata/ETC/RC
	@mkdir -p $(BUILD)/sddata/USR/BIN
	@for p in hello ls cat run write rm cp mem deep forkd trap kill upper wc fp mkdir map rmdir mv env echo malo fecha libc malo; do \
	   cp $(BUILD)/$$p.elf $(BUILD)/sddata/USR/BIN/$$(echo $$p | tr a-z A-Z).ELF; \
	 done
	@printf 'Hola desde la tarjeta SD.\nEste fichero esta en la particion de datos de la Pi.\n' > $(BUILD)/sddata/HOLA.TXT
	@echo
	@echo "BOOT (FAT16, va en /boot):"
	@ls $(BUILD)/sdcard
	@echo
	@echo "DATA (FAT32, es el raiz):"
	@ls $(BUILD)/sddata
	@echo "  USR/BIN:"
	@ls $(BUILD)/sddata/USR/BIN | tr '\n' ' '; echo
	@echo
	@echo "Copialas con:  make sd SD=/Volumes/boot DATA=/Volumes/DATA"

# Copia directamente a una SD ya montada.
# 'cp -R .../.' y no 'cp .../*': hay un subdirectorio (overlays/) y el glob
# solo pasa nombres, asi que un cp a secas se lo salta y falla.
# Copiar a la tarjeta BORRANDO ANTES lo que sobra.
#
# Esto solo copiaba, y eso resulto ser un fallo con dientes: los
# ejecutables que vivian en la particion de arranque antes del paso 35 se
# quedaron ahi para siempre. Con "." primero en el PATH, un "cd /boot" y
# un "ls" ejecutaban el viejo, que leia mal las respuestas del servidor y
# ensenyaba basura. Parecia un fallo de FAT16 y era una copia de hace
# quince pasos.
#
# Una herramienta de despliegue que nunca borra deja el destino contando
# la historia entera en vez del estado actual.
sd: sdcard
	@test -d "$(SD)" || { echo "No existe $(SD). Usa: make sd SD=/Volumes/boot DATA=/Volumes/DATA"; exit 1; }
	@# Los .ELF no pintan nada en la particion de arranque desde el paso 35.
	@if ls "$(SD)"/*.ELF >/dev/null 2>&1; then                            \
	   echo "  quitando ejecutables viejos de $(SD):";                     \
	   ls "$(SD)"/*.ELF | sed 's|.*/|    |';                               \
	   rm -f "$(SD)"/*.ELF "$(SD)/HOLA.TXT";                               \
	 fi
	@cp -R $(BUILD)/sdcard/. "$(SD)/"
	@if [ -n "$(DATA)" ]; then                                            \
	   test -d "$(DATA)" || { echo "No existe $(DATA)"; exit 1; };        \
	   rm -rf "$(DATA)/USR/BIN";                                          \
	   cp -R $(BUILD)/sddata/. "$(DATA)/";                                \
	   echo "Copiado a $(DATA) (el raiz).";                               \
	 else                                                                 \
	   echo "AVISO: sin DATA=... no se han copiado los programas.";        \
	   echo "       Usa: make sd SD=$(SD) DATA=/Volumes/DATA";             \
	 fi
	@sync
	@echo "Copiado a $(SD) (el /boot)."
	@echo "Expulsalas y arranca la Pi."

clean:
	@rm -rf $(BUILD)

-include $(DEPS)
