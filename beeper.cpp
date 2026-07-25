#include "beeper.h"

#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
