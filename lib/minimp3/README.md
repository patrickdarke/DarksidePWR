# minimp3 (vendored)

Single-header MP3 decoder used by `sound.cpp` to play `/sounds/*.mp3`
from the SD card.

- Source: https://github.com/lieff/minimp3 (`minimp3.h`, fetched 2026-08-06)
- License: CC0-1.0 / public domain (dedication in the file header) —
  chosen over the common Arduino audio libraries, which are GPL-3 and
  incompatible with this repo's MIT license
- Compiled once, in `sound.cpp`, with `MINIMP3_ONLY_MP3`
