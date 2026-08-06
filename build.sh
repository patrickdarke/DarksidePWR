#!/usr/bin/env bash
# Build (and optionally flash) Darkside PWR for the CrowPanel Advance 3.5"
# (ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB octal PSRAM, native USB CDC).
#
#   ./build.sh            build only
#   ./build.sh flash      build + flash the attached usbmodem port
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB"

[ -f "$HERE/secrets.h" ] || { echo "✗ secrets.h missing — cp secrets.h.example secrets.h and fill it in"; exit 1; }

arduino-cli compile --fqbn "$FQBN" --libraries "$HERE/lib" "$HERE"

if [ "${1:-}" = "flash" ]; then
  # macOS enumerates the panel's native USB as cu.usbmodem* (Linux: ttyACM*);
  # the board's UART header port shows as cu.wchusbserial* (CH340) and works
  # for flashing too, so it is the fallback when no native port is present.
  # `|| true`: under set -euo pipefail, ls failing on an unmatched glob
  # (always the case for the other OS's pattern) would kill the script at
  # this assignment — silently, before the error message below could print.
  PORT="$(ls /dev/cu.usbmodem* /dev/ttyACM* /dev/cu.wchusbserial* 2>/dev/null | head -1 || true)"
  [ -n "$PORT" ] || { echo "✗ no panel port found (cu.usbmodem*/ttyACM*/cu.wchusbserial*)"; exit 1; }
  arduino-cli upload --fqbn "$FQBN" -p "$PORT" "$HERE"
  echo "==> flashed $PORT"
fi
