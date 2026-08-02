#include "backlight.h"

#include <Arduino.h>
#include <Preferences.h>

#if defined(BOARD_CROWPANEL_50)
#include <Wire.h>

// ELECROW CrowPanel Advance 5.0" (800x480): no LEDC PWM pin available for
// backlight — GPIO 38 (the 3.5" board's backlight pin) is an RGB data line
// on this board. Brightness instead goes through an onboard supervisor MCU
// at I2C address 0x30: single-byte command, 0 = brightest, 245 = off
// (linear, inverted vs. a percent). Verified against ELECROW's
// V1.2_and_V1.3 lesson-03 vendor example — the detect/handshake loop below
// is their bring-up sequence, unmodified in spirit: it also arms the touch
// controller's I2C address (0x5D), so this MUST run before LovyanGFX's own
// touch init claims the I2C_NUM_0 port (see DarksidePWR.ino setup()).
namespace {

constexpr uint8_t kBacklightI2cAddr = 0x30;
constexpr uint8_t kTouchI2cAddr = 0x5D;      // probed only to confirm bring-up
constexpr int kMaxCmd = 245;                 // 245 = backlight off
int s_pct = 100;

bool i2cScanForAddress(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void sendI2cCommand(uint8_t cmd) {
  Wire.beginTransmission(kBacklightI2cAddr);
  Wire.write(cmd);
  Wire.endTransmission();
}

int clampPct(int pct) {
  if (pct < kBacklightMinPct) return kBacklightMinPct;
  if (pct > 100) return 100;
  return pct;
}

void apply(int pct) {
  s_pct = clampPct(pct);
  // 0 = brightest, 245 = off — inverted linear scale from the vendor's
  // supervisor MCU protocol, not a duty cycle.
  const int cmd = ((100 - s_pct) * kMaxCmd) / 100;
  sendI2cCommand((uint8_t)cmd);
}

}  // namespace

void backlightInit() {
  Wire.begin(15, 16);
  delay(50);

  // Detect/handshake loop, mirrors the vendor's lesson-03 bring-up: the
  // supervisor MCU (0x30) also arms the touch controller's I2C address
  // (0x5D) via the pin-1 toggle below, so this has to succeed before
  // LovyanGFX's own touch init runs. Blocks until both respond — same as
  // the vendor example, which never proceeds otherwise.
  int attempt = 0;
  while (!(i2cScanForAddress(kBacklightI2cAddr) && i2cScanForAddress(kTouchI2cAddr))) {
    if ((attempt++ % 20) == 0)
      Serial.printf("[pwr] backlight/touch MCU not detected yet (attempt %d)\n", attempt);
    sendI2cCommand(250);  // "activate touch screen" per vendor bring-up
    pinMode(1, OUTPUT);
    digitalWrite(1, LOW);
    delay(120);
    pinMode(1, INPUT);
    delay(100);
  }
  Serial.println("[pwr] backlight/touch MCU detected (0x30, 0x5D)");

  int pct = 100;
  Preferences p;
  if (p.begin("darkside", /*readOnly=*/true)) {
    pct = p.getInt("bright", 100);
    p.end();
  }
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

void backlightBlank() { sendI2cCommand((uint8_t)kMaxCmd); }  // true off, s_pct untouched
void backlightWake() { apply(s_pct); }

#else

// ELECROW CrowPanel Advance 3.5" (the truck's install): 20 kHz keeps the
// backlight supply silent; 10-bit duty gives a smooth low end. esp32 core
// 3.x pin-based LEDC API (ledcAttach/ledcWrite).
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

void backlightBlank() { ledcWrite(kPin, 0); }  // true off, s_pct untouched
void backlightWake() { apply(s_pct); }

#endif
