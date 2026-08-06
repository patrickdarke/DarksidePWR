#pragma once

// Piezo alert chirps (BEEP_5025, net IO8_BEEP on the V1.4 schematic,
// SS8050 driver). Passive part: it only sounds while driven, so chirps
// are LEDC square waves at its ~4 kHz resonance, played from a
// short-lived task (the LVGL loop never blocks). Everything richer —
// baked-in voice, SD MP3s, the per-event sound choices — lives in
// sound.h; the chirp is the bottom of that fallback chain.
void beeperInit();   // attach LEDC (silent); park the NS4168 CTRL pin
void beeperChirp();  // double chirp (no-op while already chirping)
