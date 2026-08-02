#include "beeper.h"

#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if defined(BOARD_CROWPANEL_50)
// ELECROW CrowPanel Advance 5.0" (800x480): the physical board has a
// dedicated BUZZER footprint (silkscreened next to the I2C-OUT header, with
// its own driver transistor — confirmed from the board photo), but its
// GPIO is NOT silkscreened and wasn't in the fetched ELECROW vendor example
// code, so it is NOT verified. Rather than guess a pin number on a board
// where the wrong guess could collide with an already-claimed RGB/I2C/mic
// line, this stays disabled (BEEPER_PIN_50 = -1) until confirmed on
// hardware or from the schematic — override BEEPER_PIN_50 here or from
// secrets.h once known. Chirps become real the moment that's set; nothing
// else in this file needs to change.
#ifndef BEEPER_PIN_50
#define BEEPER_PIN_50 -1
#endif

namespace {

constexpr int kPinBeep = BEEPER_PIN_50;
constexpr uint32_t kChirpHz = 4000;  // matches the 3.5" board's piezo resonance;
                                      // re-check once the part is known
constexpr int kChirpMs = 70;
constexpr int kGapMs = 80;

volatile bool s_busy = false;

void chirpTask(void*) {
  for (int i = 0; i < 2; i++) {
    ledcWriteTone(kPinBeep, kChirpHz);
    vTaskDelay(pdMS_TO_TICKS(kChirpMs));
    ledcWriteTone(kPinBeep, 0);
    if (i == 0) vTaskDelay(pdMS_TO_TICKS(kGapMs));
  }
  s_busy = false;
  vTaskDelete(nullptr);
}

}  // namespace

void beeperInit() {
  if (kPinBeep < 0) {
    Serial.println("[pwr] beeper: BUZZER GPIO not yet confirmed for this "
                    "board — chime disabled (see beeper.cpp TODO)");
    return;
  }
  ledcAttach(kPinBeep, kChirpHz, 10);
  ledcWrite(kPinBeep, 0);  // silent until asked
}

void beeperChirp() {
  if (kPinBeep < 0 || s_busy) return;
  s_busy = true;
  xTaskCreatePinnedToCore(chirpTask, "chirp", 2048, nullptr, 1, nullptr, 0);
}

#else

// ELECROW CrowPanel Advance 3.5" (the truck's install).
namespace {

constexpr int kPinBeep = 8;      // BEEP_5025 via SS8050 (schematic IO8_BEEP)
constexpr int kPinAmpCtrl = 21;  // NS4168 CTRL — held LOW, vendor-known state
constexpr uint32_t kChirpHz = 4000;  // near the 5025 piezo's resonance
constexpr int kChirpMs = 70;
constexpr int kGapMs = 80;

volatile bool s_busy = false;

void chirpTask(void*) {
  for (int i = 0; i < 2; i++) {
    ledcWriteTone(kPinBeep, kChirpHz);
    vTaskDelay(pdMS_TO_TICKS(kChirpMs));
    ledcWriteTone(kPinBeep, 0);
    if (i == 0) vTaskDelay(pdMS_TO_TICKS(kGapMs));
  }
  s_busy = false;
  vTaskDelete(nullptr);
}

}  // namespace

void beeperInit() {
  pinMode(kPinAmpCtrl, OUTPUT);
  digitalWrite(kPinAmpCtrl, LOW);
  ledcAttach(kPinBeep, kChirpHz, 10);
  ledcWrite(kPinBeep, 0);  // silent until asked
}

void beeperChirp() {
  if (s_busy) return;
  s_busy = true;
  xTaskCreatePinnedToCore(chirpTask, "chirp", 2048, nullptr, 1, nullptr, 0);
}

#endif
