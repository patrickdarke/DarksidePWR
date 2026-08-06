#include "gx_settings.h"

#include <Arduino.h>
#include <Preferences.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "secrets.h"
#include "config.h"
#include "gx_poller.h"  // GxData::kNumTemps/kNumTanks for the label roster

namespace {

SemaphoreHandle_t s_mux = nullptr;
String s_host;                   // GX target: mDNS host or literal IP
bool s_overridden = false;       // true when the target came from NVS
volatile bool s_tempsF = (TEMPS_IN_F != 0);
char s_pending[40] = "";         // target change posted by the UI
volatile bool s_dirty = false;
char s_shown[40] = "";           // stable buffer returned by gxGetTarget
bool s_loaded = false;           // gxSettingsLoad is called from the sketch
                                 // (before uiBuild) AND from gxStart — idempotent

// UI label slots (see gx_settings.h for the layout). Defaults come from the
// same config.h/secrets.h defines the UI used to bake in; the length
// static_asserts moved here from ui.cpp along with the arrays.
const char* kLblDefTemp[] = GX_TEMP_LABELS;
const char* kLblDefTank[] = GX_TANK_LABELS;
static_assert(sizeof(kLblDefTemp) / sizeof(kLblDefTemp[0]) == GxData::kNumTemps,
              "GX_TEMP_LABELS length must match GX_TEMP_UNITS");
static_assert(sizeof(kLblDefTank) / sizeof(kLblDefTank[0]) == GxData::kNumTanks,
              "GX_TANK_LABELS length must match GX_TANK_UNITS");
const char* kLblFixedDef[kGxLblFixedCount] = {UI_TITLE, UI_BATT_LABEL,
                                              "CURRENT", "POWER IN", "AC LOADS"};
// NVS keys are semantic, not positional, so stored overrides survive
// changes to the temp/tank list lengths.
const char* kLblFixedKey[kGxLblFixedCount] = {"lbl.title", "lbl.batt", "lbl.cur",
                                              "lbl.in", "lbl.ac"};
constexpr int kLblCount = kGxLblFixedCount + GxData::kNumTemps + GxData::kNumTanks;
constexpr int kLblCap = 24;
char s_lbl[kLblCount][kLblCap];

void lblKey(int i, char* out, size_t n) {
  if (i < kGxLblFixedCount) snprintf(out, n, "%s", kLblFixedKey[i]);
  else if (i < kGxLblFixedCount + GxData::kNumTemps)
    snprintf(out, n, "lbl.tmp%d", i - kGxLblFixedCount);
  else
    snprintf(out, n, "lbl.tnk%d", i - kGxLblFixedCount - GxData::kNumTemps);
}

void lock() { if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY); }
void unlock() { if (s_mux) xSemaphoreGive(s_mux); }

}  // namespace

void gxSettingsLoad() {
  if (s_loaded) return;
  s_loaded = true;
  s_mux = xSemaphoreCreateMutex();
  s_host = GX_MDNS_HOST;
  for (int i = 0; i < kLblCount; i++)
    snprintf(s_lbl[i], kLblCap, "%s", gxLabelDefault(i));
  Preferences p;
  if (p.begin("darkside", /*readOnly=*/true)) {
    if (p.isKey("gx.addr")) {
      s_host = p.getString("gx.addr", s_host);
      s_overridden = true;
    }
    if (p.isKey("tempF")) s_tempsF = p.getUChar("tempF", s_tempsF ? 1 : 0) != 0;
    for (int i = 0; i < kLblCount; i++) {
      char key[16];
      lblKey(i, key, sizeof key);
      if (p.isKey(key)) {
        const String v = p.getString(key, "");
        if (v.length()) snprintf(s_lbl[i], kLblCap, "%s", v.c_str());
      }
    }
    p.end();
  }
}

bool gxTempsInF() { return s_tempsF; }

void gxSetTempsInF(bool fahrenheit) {
  s_tempsF = fahrenheit;
  Preferences p;
  p.begin("darkside", false);
  p.putUChar("tempF", fahrenheit ? 1 : 0);
  p.end();
  Serial.printf("[setup] temps unit %s saved\n", fahrenheit ? "F" : "C");
}

const char* gxGetTarget() {
  lock();
  snprintf(s_shown, sizeof s_shown, "%s", s_host.c_str());
  unlock();
  return s_shown;
}

void gxSetTarget(const char* addr) {
  // NVS is written here (thread-safe); the host swap itself is applied by
  // the poller task via gxTargetConsumePending — never yank its socket
  // from the UI task.
  Preferences p;
  p.begin("darkside", false);
  if (addr && addr[0]) p.putString("gx.addr", addr);
  else p.remove("gx.addr");
  p.end();
  lock();
  snprintf(s_pending, sizeof s_pending, "%s", addr ? addr : "");
  unlock();
  s_dirty = true;
}

bool gxTargetConsumePending() {
  if (!s_dirty) return false;
  s_dirty = false;
  lock();
  if (s_pending[0]) {
    s_host = s_pending;
    s_overridden = true;
  } else {
    s_host = GX_MDNS_HOST;
    s_overridden = false;
  }
  const String applied = s_host;
  unlock();
  Serial.printf("[setup] gx target set to '%s'\n", applied.c_str());
  return true;
}

void gxTargetCopy(String& host, bool& overridden) {
  lock();
  host = s_host;
  overridden = s_overridden;
  unlock();
}

int gxLabelCount() { return kLblCount; }

const char* gxLabel(int i) {
  return (i >= 0 && i < kLblCount) ? s_lbl[i] : "";
}

const char* gxLabelDefault(int i) {
  if (i < 0 || i >= kLblCount) return "";
  if (i < kGxLblFixedCount) return kLblFixedDef[i];
  if (i < kGxLblFixedCount + GxData::kNumTemps)
    return kLblDefTemp[i - kGxLblFixedCount];
  return kLblDefTank[i - kGxLblFixedCount - GxData::kNumTemps];
}

void gxLabelCaption(int i, char* out, int cap) {
  static const char* kFix[kGxLblFixedCount] = {"TITLE", "BATT TILE", "CURRENT TILE",
                                               "POWER TILE", "AC TILE"};
  if (i < kGxLblFixedCount) snprintf(out, cap, "%s", kFix[i]);
  else if (i < kGxLblFixedCount + GxData::kNumTemps)
    snprintf(out, cap, "TEMP %d", i - kGxLblFixedCount + 1);
  else
    snprintf(out, cap, "TANK %d", i - kGxLblFixedCount - GxData::kNumTemps + 1);
}

void gxSetLabel(int i, const char* txt) {
  if (i < 0 || i >= kLblCount) return;
  char key[16];
  lblKey(i, key, sizeof key);
  Preferences p;
  p.begin("darkside", false);
  if (txt && txt[0]) p.putString(key, txt);
  else p.remove(key);
  p.end();
  snprintf(s_lbl[i], kLblCap, "%s", (txt && txt[0]) ? txt : gxLabelDefault(i));
  Serial.printf("[setup] label %s = '%s'%s\n", key, s_lbl[i],
                (txt && txt[0]) ? "" : " (default)");
}
