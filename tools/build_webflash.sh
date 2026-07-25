#!/usr/bin/env bash
# Build the browser-flashable image for the GitHub Pages flasher
# (docs/index.html + esp-web-tools). Produces docs/firmware/darksidepwr.bin,
# a merged image (bootloader + partitions + boot_app0 + app) flashed at 0x0.
#
# The web image is NEUTRAL: it builds against secrets.h.example (placeholder
# Wi-Fi — the on-device setup screen provisions the panel), with config.h
# committed defaults for everything else. Any local secrets.h is moved
# aside for the build and restored afterwards, untouched.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"

FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB"
BUILD="$HERE/build/webflash"
OUT="$HERE/docs/firmware"

restore() {
  rm -f "$HERE/secrets.h"
  [ -f "$HERE/secrets.h.webflash-bak" ] && mv "$HERE/secrets.h.webflash-bak" "$HERE/secrets.h"
  return 0
}
[ -f "$HERE/secrets.h" ] && mv "$HERE/secrets.h" "$HERE/secrets.h.webflash-bak"
trap restore EXIT
cp "$HERE/secrets.h.example" "$HERE/secrets.h"

arduino-cli compile --fqbn "$FQBN" --libraries "$HERE/lib" --build-path "$BUILD" "$HERE"

ESPTOOL="$(ls "$HOME"/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | head -1)"
BOOT_APP0="$(ls "$HOME"/Library/Arduino15/packages/esp32/hardware/esp32/*/tools/partitions/boot_app0.bin 2>/dev/null | head -1)"
[ -n "$ESPTOOL" ] && [ -n "$BOOT_APP0" ] || { echo "esptool or boot_app0.bin not found"; exit 1; }

mkdir -p "$OUT"
"$ESPTOOL" --chip esp32s3 merge-bin -o "$OUT/darksidepwr.bin" \
  0x0 "$BUILD/DarksidePWR.ino.bootloader.bin" \
  0x8000 "$BUILD/DarksidePWR.ino.partitions.bin" \
  0xe000 "$BOOT_APP0" \
  0x10000 "$BUILD/DarksidePWR.ino.bin"

echo "==> $OUT/darksidepwr.bin ($(du -h "$OUT/darksidepwr.bin" | cut -f1))"
