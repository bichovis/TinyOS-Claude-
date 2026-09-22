#!/usr/bin/env python3
"""Convierte un binario plano en un array de C, para empotrar un programa
de usuario dentro de la imagen del kernel. Mas adelante, cuando haya un
sistema de ficheros, esto lo sustituira una carga de verdad desde disco."""
import sys

src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, "rb").read()

with open(dst, "w") as f:
    f.write("/* Generado por tools/bin2c.py. No editar. */\n")
    f.write("#include <stdint.h>\n\n")
    f.write(f"const uint64_t {name}_size = {len(data)};\n")
    f.write(f"const uint8_t {name}[] __attribute__((aligned(8))) = {{\n")
    for i in range(0, len(data), 12):
        row = ", ".join(f"0x{b:02X}" for b in data[i:i+12])
        f.write(f"    {row},\n")
    f.write("};\n")
print(f"{dst}: {len(data)} bytes")
