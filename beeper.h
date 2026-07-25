#pragma once

// Alert sounds on the passive piezo buzzer (BEEP_5025, net IO8_BEEP on the
// V1.4 schematic, SS8050 transistor driver). Passive = it only sounds while
// driven, so chirps are LEDC square waves at the part's ~4 kHz resonance,
// played from a short-lived task (the LVGL loop never blocks). The NS4168
// I2S speaker path is deprecated for alerts — pins remain documented in
// CLAUDE.md if music/voice output is ever wanted.
void beeperInit();   // attach LEDC (silent); park the NS4168 CTRL pin
void beeperChirp();  // double chirp — "charge complete" (no-op if playing)
