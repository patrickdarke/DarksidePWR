#include "beeper.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// I2S is begun/ended around each chime: nothing held open, no idle amp
// hiss, and a failed begin just means a silent chime.
namespace {

constexpr int kPinBclk = 13;
constexpr int kPinLrc = 11;
constexpr int kPinDout = 12;
constexpr int kPinAmpQuirk = 21;   // vendor lesson: hold LOW for audio

constexpr uint32_t kRate = 22050;
constexpr float kAmp = 0.25f * 32767.0f;  // polite volume
constexpr int kRampMs = 6;                // attack/release to avoid clicks

volatile bool s_busy = false;

// Append one tone (or silence for freqHz == 0) to the I2S stream in small
// interleaved-stereo chunks.
void playTone(I2SClass& i2s, float freqHz, int ms) {
  const int frames = (int)((int64_t)kRate * ms / 1000);
  const int rampFrames = (int)(kRate * kRampMs / 1000);
  static int16_t buf[256 * 2];
  float phase = 0.0f;
  const float step = 2.0f * (float)M_PI * freqHz / kRate;
  int done = 0;
  while (done < frames) {
    const int n = ((frames - done) < 256) ? (frames - done) : 256;
    for (int i = 0; i < n; i++) {
      float a = kAmp;
      const int idx = done + i;
      if (idx < rampFrames) a *= (float)idx / rampFrames;
      const int left = frames - idx;
      if (left < rampFrames) a *= (float)left / rampFrames;
      const int16_t s = (freqHz > 0) ? (int16_t)(a * sinf(phase)) : 0;
      phase += step;
      buf[2 * i] = s;
      buf[2 * i + 1] = s;
    }
    i2s.write((uint8_t*)buf, n * 2 * sizeof(int16_t));
    done += n;
  }
}

void chimeTask(void*) {
  I2SClass i2s;
  i2s.setPins(kPinBclk, kPinLrc, kPinDout, -1, -1);
  if (i2s.begin(I2S_MODE_STD, kRate, I2S_DATA_BIT_WIDTH_16BIT,
                I2S_SLOT_MODE_STEREO)) {
    playTone(i2s, 1568.0f, 130);  // G6
    playTone(i2s, 0.0f, 60);
    playTone(i2s, 2093.0f, 180);  // C7
    playTone(i2s, 0.0f, 40);      // let the tail drain before end()
    i2s.end();
  } else {
    Serial.println("[beep] i2s begin failed");
  }
  s_busy = false;
  vTaskDelete(nullptr);
}

}  // namespace

void beeperInit() {
  pinMode(kPinAmpQuirk, OUTPUT);
  digitalWrite(kPinAmpQuirk, LOW);
}

void beeperChime() {
  if (s_busy) return;
  s_busy = true;
  xTaskCreatePinnedToCore(chimeTask, "chime", 4096, nullptr, 1, nullptr, 0);
}
