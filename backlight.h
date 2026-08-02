#pragma once

// LEDC-PWM backlight on GPIO 38 (the CrowPanel wires it as a plain enable;
// vendor firmware just drives it HIGH). Percent is clamped to
// [kBacklightMinPct, 100] so a touch-only device can never dim itself to an
// invisible screen. The chosen percent persists in NVS ("darkside"/"bright").
constexpr int kBacklightMinPct = 10;

void backlightInit();        // attach LEDC and apply the NVS-stored percent
void backlightSet(int pct);  // apply now (live preview, no NVS write)
void backlightSave(int pct); // apply + persist
int backlightGet();          // current percent

// True off / restore, bypassing kBacklightMinPct on purpose — for
// deliberate scheduled blanking (night_mode.h), not general use. Neither
// touches the persisted percent, so backlightWake() restores exactly what
// backlightGet() reported before blanking.
void backlightBlank();
void backlightWake();
