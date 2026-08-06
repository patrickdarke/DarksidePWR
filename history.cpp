#include "history.h"

#include <Arduino.h>
#include <math.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#ifdef HIST_ENABLE_STORAGE
#include <FS.h>
#include <FFat.h>
#include <SD.h>
#include <SPI.h>
#endif

// Ring of minute averages in PSRAM, keyed by epoch minute (slot = minute %
// kHistMinutes; s_slotMin remembers which minute a slot holds so stale
// laps read as gaps). Recording starts once SNTP has set the clock — GX
// data needs Wi-Fi anyway, so that costs only the first seconds.
//
// Threading: histRecord/histGet run on the UI task, the storage task reads
// the ring for snapshots — all ring access under s_mux with short holds;
// file IO happens strictly OUTSIDE the lock on the storage task.
namespace {

SemaphoreHandle_t s_mux = nullptr;
float* s_val = nullptr;        // [kHistCount][kHistMinutes]
uint32_t* s_slotMin = nullptr; // epoch minute held by each slot (0 = never)

// Current-minute accumulator (UI task only).
uint32_t s_curMin = 0;
float s_sum[kHistCount];
int s_cnt[kHistCount];

inline float* cell(int series, int slot) { return &s_val[series * kHistMinutes + slot]; }

void lock() { xSemaphoreTake(s_mux, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_mux); }

// SD bus/volume mutex, shared with sound.cpp via histSdTake/histSdGive.
// Lives outside the storage #ifdef so the API links on every build.
SemaphoreHandle_t s_sdMux = nullptr;

#ifdef HIST_ENABLE_STORAGE
// ---- storage task state --------------------------------------------------
struct CsvJob {
  char fname[20];  // /2026-08-05.csv (date captured at enqueue — midnight-safe)
  char line[80];
};
QueueHandle_t s_csvQ = nullptr;
TaskHandle_t s_storTask = nullptr;
bool s_ffatOk = false;
bool s_sdOk = false;
bool s_loadedSnapshot = false;   // FFat reload done (deferred until time valid)
uint32_t s_nextSnapshotMs = 0;
uint32_t s_csvWritten = 0, s_csvDropped = 0, s_snapshots = 0;

constexpr uint32_t kSnapshotEveryMs = 15 * 60 * 1000;
constexpr char kSnapFile[] = "/hist.bin";
constexpr uint32_t kSnapMagic = 0x44504831;  // 'DPH1'

SPIClass s_sdSpi(HSPI);

struct SnapHeader {
  uint32_t magic, series, minutes, savedMin;
};

void saveSnapshot() {
  if (!s_ffatOk) return;
  const size_t valBytes = sizeof(float) * kHistCount * kHistMinutes;
  const size_t slotBytes = sizeof(uint32_t) * kHistMinutes;
  static uint8_t* tmp = nullptr;  // PSRAM staging so the lock never spans IO
  if (!tmp) tmp = (uint8_t*)heap_caps_malloc(valBytes + slotBytes, MALLOC_CAP_SPIRAM);
  if (!tmp) return;
  lock();
  memcpy(tmp, s_val, valBytes);
  memcpy(tmp + valBytes, s_slotMin, slotBytes);
  unlock();
  SnapHeader h = {kSnapMagic, kHistCount, kHistMinutes, (uint32_t)(time(nullptr) / 60)};
  // Write to a temp file and swap it in — truncating /hist.bin in place
  // would leave a corrupt snapshot if power dies mid-write.
  File f = FFat.open("/hist.tmp", FILE_WRITE);
  if (!f) return;
  bool ok = f.write((uint8_t*)&h, sizeof h) == sizeof h &&
            f.write(tmp, valBytes + slotBytes) == valBytes + slotBytes;
  f.close();
  if (ok) {
    FFat.remove(kSnapFile);
    ok = FFat.rename("/hist.tmp", kSnapFile);
  }
  if (ok) s_snapshots++;
}

void loadSnapshot() {
  s_loadedSnapshot = true;  // one attempt only
  if (!s_ffatOk || !FFat.exists(kSnapFile)) return;
  File f = FFat.open(kSnapFile, FILE_READ);
  if (!f) return;
  SnapHeader h;
  const size_t valBytes = sizeof(float) * kHistCount * kHistMinutes;
  const size_t slotBytes = sizeof(uint32_t) * kHistMinutes;
  uint8_t* tmp = (uint8_t*)heap_caps_malloc(valBytes + slotBytes, MALLOC_CAP_SPIRAM);
  bool ok = tmp && f.read((uint8_t*)&h, sizeof h) == sizeof h &&
            h.magic == kSnapMagic && h.series == kHistCount &&
            h.minutes == kHistMinutes &&
            f.read(tmp, valBytes + slotBytes) == valBytes + slotBytes;
  f.close();
  if (ok) {
    const uint32_t nowMin = (uint32_t)(time(nullptr) / 60);
    const uint32_t* slots = (const uint32_t*)(tmp + valBytes);
    int kept = 0;
    lock();
    for (int i = 0; i < kHistMinutes; i++) {
      // Keep only slots still inside the trailing 24 h window.
      if (slots[i] != 0 && nowMin >= slots[i] && nowMin - slots[i] < (uint32_t)kHistMinutes) {
        s_slotMin[i] = slots[i];
        for (int sIdx = 0; sIdx < kHistCount; sIdx++)
          *cell(sIdx, i) = ((const float*)tmp)[sIdx * kHistMinutes + i];
        kept++;
      }
    }
    unlock();
    Serial.printf("[hist] snapshot restored: %d minutes carried over\n", kept);
  } else {
    Serial.println("[hist] snapshot unreadable, starting fresh");
  }
  if (tmp) heap_caps_free(tmp);
}

void drainCsv() {
  CsvJob j;
  while (xQueueReceive(s_csvQ, &j, 0) == pdTRUE) {
    if (!s_sdOk) { s_csvDropped++; continue; }
    // May wait a few seconds behind an MP3 the sound player is streaming —
    // the queue (depth 8) rides that out.
    xSemaphoreTake(s_sdMux, portMAX_DELAY);
    const bool fresh = !SD.exists(j.fname);
    File f = SD.open(j.fname, FILE_APPEND);
    if (!f) {
      xSemaphoreGive(s_sdMux);
      s_sdOk = false;  // card yanked?
      s_csvDropped++;
      continue;
    }
    if (fresh) f.print("time,batt_v,batt_a,in_w,ac_w,soc\n");
    f.print(j.line);
    f.close();
    xSemaphoreGive(s_sdMux);
    s_csvWritten++;
  }
}

// Mount (or re-mount) the card under the SD mutex. On success, make sure
// /sounds exists — that is where the sound player and the SOUNDS screen
// look for MP3s, and creating it here means a fresh card shows the folder
// the first time the owner mounts it on a computer.
bool mountSd(bool remount) {
  xSemaphoreTake(s_sdMux, portMAX_DELAY);
  if (remount) SD.end();
  const bool ok = SD.begin(HIST_SD_CS, s_sdSpi, 40000000);
  if (ok && !SD.exists("/sounds")) SD.mkdir("/sounds");
  xSemaphoreGive(s_sdMux);
  return ok;
}

void storageTask(void*) {
  // Mount here, not in histInit — keeps boot instant and IO off the UI task.
  s_ffatOk = FFat.begin(true);  // true = format the partition on first use
  Serial.printf("[hist] ffat %s\n", s_ffatOk ? "mounted" : "unavailable (RAM-only persistence)");
  s_sdSpi.begin(HIST_SD_SCK, HIST_SD_MISO, HIST_SD_MOSI);
  // Vendor lesson runs this bus at 80 MHz; 40 MHz buys margin for a truck.
  s_sdOk = mountSd(false);
  Serial.printf("[hist] sd %s\n", s_sdOk ? "mounted (daily CSV on)" : "not present (CSV off)");
  s_nextSnapshotMs = millis() + kSnapshotEveryMs;
  uint32_t nextSdRetryMs = millis() + 60000;
  for (;;) {
    if (!s_loadedSnapshot && time(nullptr) > 1600000000) loadSnapshot();
    // A pulled/absent card retries once a minute — cards come and go in a
    // truck; CSV resumes when one is back.
    if (!s_sdOk && (int32_t)(millis() - nextSdRetryMs) >= 0) {
      nextSdRetryMs = millis() + 60000;
      s_sdOk = mountSd(true);
      if (s_sdOk) Serial.println("[hist] sd card back, CSV resumed");
    }
    drainCsv();
    if ((int32_t)(millis() - s_nextSnapshotMs) >= 0) {
      s_nextSnapshotMs = millis() + kSnapshotEveryMs;
      saveSnapshot();
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void enqueueCsv(uint32_t epochMin, const float* avg, const int* have) {
  CsvJob j;
  const time_t t = (time_t)epochMin * 60;
  struct tm tmv;
  localtime_r(&t, &tmv);
  snprintf(j.fname, sizeof j.fname, "/%04d-%02d-%02d.csv",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  int off = snprintf(j.line, sizeof j.line, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  static const int kDec[kHistCount] = {2, 1, 0, 0, 0};
  for (int sIdx = 0; sIdx < kHistCount; sIdx++) {
    if (have[sIdx])
      off += snprintf(j.line + off, sizeof j.line - off, ",%.*f", kDec[sIdx], avg[sIdx]);
    else
      off += snprintf(j.line + off, sizeof j.line - off, ",");
  }
  snprintf(j.line + off, sizeof j.line - off, "\n");
  if (xQueueSend(s_csvQ, &j, 0) != pdTRUE) s_csvDropped++;
}
#endif  // HIST_ENABLE_STORAGE

// Fold the finished accumulator minute into the ring (UI task).
void finalizeMinute() {
  if (s_curMin == 0) return;
  float avg[kHistCount];
  int have[kHistCount];
  for (int sIdx = 0; sIdx < kHistCount; sIdx++) {
    have[sIdx] = s_cnt[sIdx] > 0;
    avg[sIdx] = have[sIdx] ? s_sum[sIdx] / s_cnt[sIdx] : NAN;
  }
  const int slot = s_curMin % kHistMinutes;
  lock();
  s_slotMin[slot] = s_curMin;
  for (int sIdx = 0; sIdx < kHistCount; sIdx++) *cell(sIdx, slot) = avg[sIdx];
  unlock();
#ifdef HIST_ENABLE_STORAGE
  enqueueCsv(s_curMin, avg, have);
#endif
}

}  // namespace

void histInit() {
  s_mux = xSemaphoreCreateMutex();
  s_sdMux = xSemaphoreCreateMutex();
  const size_t valBytes = sizeof(float) * kHistCount * kHistMinutes;
  s_val = (float*)heap_caps_malloc(valBytes, MALLOC_CAP_SPIRAM);
  s_slotMin = (uint32_t*)heap_caps_calloc(kHistMinutes, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  if (!s_val || !s_slotMin) {
    Serial.println("[hist] alloc failed — history disabled");
    s_val = nullptr;
    return;
  }
  for (int i = 0; i < kHistCount * kHistMinutes; i++) s_val[i] = NAN;
  memset(s_sum, 0, sizeof s_sum);
  memset(s_cnt, 0, sizeof s_cnt);
#ifdef HIST_ENABLE_STORAGE
  s_csvQ = xQueueCreate(8, sizeof(CsvJob));
  xTaskCreatePinnedToCore(storageTask, "hist", 8192, nullptr, 1, &s_storTask, 0);
#endif
}

void histRecord(const GxData& d) {
  if (!s_val || !d.valid) return;
  const time_t now = time(nullptr);
  if (now < 1600000000) return;  // clock not SNTP-synced yet
  const uint32_t m = (uint32_t)(now / 60);
  if (m != s_curMin) {
    finalizeMinute();
    s_curMin = m;
    memset(s_sum, 0, sizeof s_sum);
    memset(s_cnt, 0, sizeof s_cnt);
  }
  const float v[kHistCount] = {d.battV, d.battA, (float)d.inW, (float)d.acW, (float)d.soc};
  for (int sIdx = 0; sIdx < kHistCount; sIdx++) {
    s_sum[sIdx] += v[sIdx];
    s_cnt[sIdx]++;
  }
}

uint32_t histGet(int series, float* out) {
  if (!s_val || series < 0 || series >= kHistCount) {
    for (int i = 0; i < kHistMinutes; i++) out[i] = NAN;
    return 0;
  }
  const uint32_t nowMin = (uint32_t)(time(nullptr) / 60);
  lock();
  for (int i = 0; i < kHistMinutes; i++) {
    // out[] is oldest..newest ending at the current minute.
    const uint32_t m = nowMin - (kHistMinutes - 1) + i;
    const int slot = m % kHistMinutes;
    out[i] = (s_slotMin[slot] == m) ? *cell(series, slot) : NAN;
  }
  unlock();
  return nowMin;
}

void histStatus() {
  int filled = 0;
  if (s_val) {
    const uint32_t nowMin = (uint32_t)(time(nullptr) / 60);
    lock();
    for (int i = 0; i < kHistMinutes; i++)
      if (s_slotMin[i] != 0 && nowMin - s_slotMin[i] < (uint32_t)kHistMinutes) filled++;
    unlock();
  }
#ifdef HIST_ENABLE_STORAGE
  Serial.printf("[hist] ram=%d/%d min  ffat=%s snaps=%lu  sd=%s csv=%lu dropped=%lu"
                "  stackfree=%u\n",
                filled, kHistMinutes, s_ffatOk ? "ok" : "no",
                (unsigned long)s_snapshots, s_sdOk ? "ok" : "no",
                (unsigned long)s_csvWritten, (unsigned long)s_csvDropped,
                s_storTask ? (unsigned)uxTaskGetStackHighWaterMark(s_storTask) : 0);
#else
  Serial.printf("[hist] ram=%d/%d min (no storage backend on this board)\n",
                filled, kHistMinutes);
#endif
}

bool histSdReady() {
#ifdef HIST_ENABLE_STORAGE
  return s_sdOk;
#else
  return false;
#endif
}

bool histSdTake(uint32_t maxWaitMs) {
  if (!s_sdMux) return false;
  return xSemaphoreTake(s_sdMux, pdMS_TO_TICKS(maxWaitMs)) == pdTRUE;
}

void histSdGive() {
  if (s_sdMux) xSemaphoreGive(s_sdMux);
}
