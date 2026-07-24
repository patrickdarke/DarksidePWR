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
  PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)"
  [ -n "$PORT" ] || { echo "✗ no usbmodem port found"; exit 1; }
  arduino-cli upload --fqbn "$FQBN" -p "$PORT" "$HERE"
  echo "==> flashed $PORT"
fi
