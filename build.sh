#!/usr/bin/env bash
# Build (and optionally flash) Darkside PWR for either board target:
#
#   ./build.sh              build only, 3.5" (default, the truck's install)
#   ./build.sh flash        build + flash, 3.5"
#   ./build.sh 50           build only, 5.0" (second, new install)
#   ./build.sh 50 flash     build + flash, 5.0"
#
# Board defaults to 35 so the existing truck workflow is never silently
# affected by adding the second board.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

BOARD="35"
DO_FLASH=""
for arg in "$@"; do
  case "$arg" in
    35|50) BOARD="$arg" ;;
    flash) DO_FLASH="flash" ;;
    *) echo "✗ unknown argument: $arg (expected 35, 50, and/or flash)"; exit 1 ;;
  esac
done

EXTRA_ARGS=""
if [ "$BOARD" = "50" ]; then
  # CDCOnBoot=cdc targets the S3's native USB peripheral — this board
  # doesn't route that to its USB connector (confirmed live: it enumerates
  # as a WCH UART-bridge chip, not native CDC; see CLAUDE.md). `default`
  # here is a first guess, not yet verified against a real flash.
  FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=default,PartitionScheme=app3M_fat9M_16MB"
  EXTRA_ARGS='--build-property build.extra_flags=-DBOARD_CROWPANEL_50'
else
  FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB"
fi

[ -f "$HERE/secrets.h" ] || { echo "✗ secrets.h missing — cp secrets.h.example secrets.h and fill it in"; exit 1; }

arduino-cli compile --fqbn "$FQBN" --libraries "$HERE/lib" $EXTRA_ARGS "$HERE"

if [ "$DO_FLASH" = "flash" ]; then
  # Plain `ls glob1 glob2` under `set -o pipefail` exits non-zero (and kills
  # the script via errexit) whenever ONE of the glob patterns has no match —
  # which is every time on macOS, since only one of usbmodem*/ttyACM* or
  # wchusbserial*/ttyUSB* is ever real on a given platform. Test candidates
  # individually with `-e` instead (an unmatched glob just stays a literal
  # non-existent path, which `-e` reports false for — no ls, no exit-code
  # poisoning).
  findPort() {
    for cand in "$@"; do
      [ -e "$cand" ] && { echo "$cand"; return 0; }
    done
    return 1
  }
  if [ "$BOARD" = "50" ]; then
    # WCH CH34x UART bridge (confirmed live) — needs the CH34x VCP driver
    # installed AND approved in System Settings before it enumerates at all.
    # macOS: cu.wchusbserial*. Linux: ttyUSB*.
    PORT="$(findPort /dev/cu.wchusbserial* /dev/ttyUSB*)" || PORT=""
  else
    # macOS: cu.usbmodem* (native USB-CDC). Linux: ttyACM*.
    PORT="$(findPort /dev/cu.usbmodem* /dev/ttyACM*)" || PORT=""
  fi
  [ -n "$PORT" ] || { echo "✗ no panel port found for board $BOARD"; exit 1; }
  arduino-cli upload --fqbn "$FQBN" -p "$PORT" "$HERE"
  echo "==> flashed $PORT (board $BOARD)"
fi
