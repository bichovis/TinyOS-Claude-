# ==========================================================================
#  TinyOS - microkernel para Raspberry Pi 3B (BCM2837, Cortex-A53, AArch64)
# ==========================================================================

CROSS   ?= aarch64-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
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
UPROGS  := hello conserver client
UCFLAGS := -Wall -Wextra -Werror -O2 -std=c11 -ffreestanding -nostdlib \
           -nostartfiles -mcpu=cortex-a53 -mgeneral-regs-only -mstrict-align \
           -fno-stack-protector -fno-pie -fno-common -Iuser -I$(INCDIR)
ULDFLAGS := -nostdlib -nostartfiles -T user/user.ld \
            -Wl,--no-warn-rwx-segments

CSRCS   := $(wildcard $(SRCDIR)/*.c)
ASRCS   := $(wildcard $(SRCDIR)/*.S)
OBJS    := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(wildcard $(SRCDIR)/*.c)) \
           $(patsubst $(SRCDIR)/%.S,$(BUILD)/%.S.o,$(ASRCS)) \
           $(patsubst %,$(BUILD)/%_bin.o,$(UPROGS))
DEPS    := $(OBJS:.o=.d)

.PHONY: all clean run debug dump sdcard sd firmware
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
.PRECIOUS: $(BUILD)/%.elf $(BUILD)/%.bin $(BUILD)/%_bin.c

$(BUILD)/%.elf: user/%.c user/header.S user/syscall.h $(INCDIR)/ipc_abi.h \
                $(INCDIR)/user_abi.h user/user.ld | $(BUILD)
	@echo "  CC-U  user/$*.c"
	@$(CC) $(UCFLAGS) $(ULDFLAGS) user/$*.c user/header.S -o $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	@$(OBJCOPY) -O binary $< $@

$(BUILD)/%_bin.c: $(BUILD)/%.bin tools/bin2c.py
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

# QEMU emula la raspi3b; la PL011 sale por stdio.
run: $(BUILD)/kernel8.elf
	$(QEMU) -M raspi3b -kernel $< -serial stdio -serial null -display none

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
sdcard: $(BUILD)/kernel8.img firmware
	@cp config.txt $(BUILD)/sdcard/
	@cp $(BUILD)/kernel8.img $(BUILD)/sdcard/
	@echo
	@echo "Listo en $(BUILD)/sdcard:"
	@ls -l $(BUILD)/sdcard
	@echo
	@echo "Copialo a una SD con una particion FAT32, o usa:  make sd SD=/Volumes/TUSD"

# Copia directamente a una SD ya montada
sd: sdcard
	@test -d "$(SD)" || { echo "No existe $(SD). Usa: make sd SD=/Volumes/TUSD"; exit 1; }
	@cp $(BUILD)/sdcard/* "$(SD)/"
	@sync
	@echo "Copiado a $(SD). Expulsala y arranca la Pi."

clean:
	@rm -rf $(BUILD)

-include $(DEPS)
