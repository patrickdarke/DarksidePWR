#include "backlight.h"

#include <Arduino.h>
#include <Preferences.h>

// 20 kHz keeps the backlight supply silent; 10-bit duty gives a smooth low
// end. esp32 core 3.x pin-based LEDC API (ledcAttach/ledcWrite).
namespace {

constexpr int kPin = 38;
constexpr uint32_t kFreqHz = 20000;
constexpr uint8_t kResBits = 10;
int s_pct = 100;

int clampPct(int pct) {
  if (pct < kBacklightMinPct) return kBacklightMinPct;
  if (pct > 100) return 100;
  return pct;
}

void apply(int pct) {
  s_pct = clampPct(pct);
  ledcWrite(kPin, (1023u * s_pct) / 100);
}

}  // namespace

void backlightInit() {
  int pct = 100;
  Preferences p;
  if (p.begin("darkside", /*readOnly=*/true)) {
    pct = p.getInt("bright", 100);
    p.end();
  }
  ledcAttach(kPin, kFreqHz, kResBits);
  apply(pct);
  Serial.printf("[pwr] backlight %d%%\n", s_pct);
}

void backlightSet(int pct) { apply(pct); }

void backlightSave(int pct) {
  apply(pct);
  Preferences p;
  p.begin("darkside", false);
  p.putInt("bright", s_pct);
  p.end();
  Serial.printf("[setup] backlight saved %d%%\n", s_pct);
}

int backlightGet() { return s_pct; }
