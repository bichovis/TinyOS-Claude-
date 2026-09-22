#!/bin/sh
# Descarga el firmware propietario que la Raspberry Pi 3B necesita para
# arrancar. No se puede generar: son binarios de Broadcom.
#
#   bootcode.bin   lo ejecuta la ROM de la GPU; carga start.elf
#   start.elf      el firmware de verdad: lee config.txt y carga kernel8.img
#   fixup.dat      tabla de reparto de memoria entre CPU y GPU
#
# Y el device tree, que hace falta para que 'dtoverlay=disable-bt' surta
# efecto. Sin estos dos ficheros el firmware ignora la linea del config.txt
# en silencio, la PL011 se queda cableada al Bluetooth y no sale nada por
# el conector.
#
#   bcm2710-rpi-3-b.dtb        descripcion del hardware de la placa
#   overlays/disable-bt.dtbo   la modificacion que libera la PL011
set -e

DEST="${1:-build/sdcard}"
BASE="https://raw.githubusercontent.com/raspberrypi/firmware/master/boot"

mkdir -p "$DEST/overlays"

get() {
    if [ -f "$DEST/$1" ]; then
        echo "  ya esta   $1"
    else
        echo "  bajando   $1"
        curl -fsSL "$BASE/$1" -o "$DEST/$1"
    fi
}

for f in bootcode.bin start.elf fixup.dat \
         bcm2710-rpi-3-b.dtb bcm2710-rpi-3-b-plus.dtb \
         overlays/disable-bt.dtbo; do
    get "$f"
done

echo "Firmware en $DEST"
