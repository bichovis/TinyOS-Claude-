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
UCFLAGS := -Wall -Wextra -Werror -O2 -std=c11 -ffreestanding -nostdlib \
           -nostartfiles -mcpu=cortex-a53 -mgeneral-regs-only -mstrict-align \
           -fno-stack-protector -fno-pie -fno-common -Iuser
ULDFLAGS := -nostdlib -nostartfiles -T user/user.ld \
            -Wl,--no-warn-rwx-segments

CSRCS   := $(wildcard $(SRCDIR)/*.c) $(BUILD)/hello_bin.c
ASRCS   := $(wildcard $(SRCDIR)/*.S)
OBJS    := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(wildcard $(SRCDIR)/*.c)) \
           $(patsubst $(SRCDIR)/%.S,$(BUILD)/%.S.o,$(ASRCS)) \
           $(BUILD)/hello_bin.o
DEPS    := $(OBJS:.o=.d)

.PHONY: all clean run debug dump
all: $(BUILD)/kernel8.img

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: $(SRCDIR)/%.S | $(BUILD)
	@echo "  AS    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# --- Cadena del programa de usuario ---
$(BUILD)/hello.elf: user/hello.c user/syscall.h user/user.ld | $(BUILD)
	@echo "  CC-U  user/hello.c"
	@$(CC) $(UCFLAGS) $(ULDFLAGS) user/hello.c -o $@

$(BUILD)/hello.bin: $(BUILD)/hello.elf
	@$(OBJCOPY) -O binary $< $@

$(BUILD)/hello_bin.c: $(BUILD)/hello.bin tools/bin2c.py
	@echo "  BIN2C $@"
	@python3 tools/bin2c.py $< $@ user_hello

$(BUILD)/hello_bin.o: $(BUILD)/hello_bin.c
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

clean:
	@rm -rf $(BUILD)

-include $(DEPS)
