#pragma once

// Alert sounds on the board's piezo buzzer. Passive = it only sounds while
// driven, so chirps are LEDC square waves at the part's resonance, played
// from a short-lived task (the LVGL loop never blocks). Board-specific pin
// facts live in CLAUDE.md and in the .cpp's per-board sections — on the
// 3.5" board this is BEEP_5025 (net IO8_BEEP, SS8050 driver), with the
// NS4168 I2S speaker path deprecated for alerts. On the 5.0" board the
// on-board BUZZER footprint's exact GPIO is a documented open TODO.
void beeperInit();   // attach LEDC (silent); park any needed control pins
void beeperChirp();  // double chirp — "charge complete" (no-op if playing)
