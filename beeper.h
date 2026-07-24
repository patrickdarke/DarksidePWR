#pragma once

// Onboard speaker via I2S (vendor-verified pins for the Advance 3.5"
// V1.2-V1.4: BCLK 13, LRC 11, DOUT 12; GPIO 21 must be held LOW — the
// vendor's audio lesson calls this "necessary"). The chime plays on its own
// short-lived task so neither the LVGL loop nor the GX poller ever blocks.
void beeperInit();   // pin setup only; I2S is opened per chime
void beeperChime();  // two-note "charge complete" chime (no-op if playing)
