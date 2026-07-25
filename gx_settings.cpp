#include "gx_settings.h"

#include <Arduino.h>
#include <Preferences.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "secrets.h"
#include "config.h"

namespace {

SemaphoreHandle_t s_mux = nullptr;
String s_host;                   // GX target: mDNS host or literal IP
bool s_overridden = false;       // true when the target came from NVS
volatile bool s_tempsF = (TEMPS_IN_F != 0);
char s_pending[40] = "";         // target change posted by the UI
volatile bool s_dirty = false;
char s_shown[40] = "";           // stable buffer returned by gxGetTarget

void lock() { if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY); }
void unlock() { if (s_mux) xSemaphoreGive(s_mux); }

}  // namespace

void gxSettingsLoad() {
  s_mux = xSemaphoreCreateMutex();
  s_host = GX_MDNS_HOST;
  Preferences p;
  if (p.begin("darkside", /*readOnly=*/true)) {
    if (p.isKey("gx.addr")) {
      s_host = p.getString("gx.addr", s_host);
      s_overridden = true;
    }
    if (p.isKey("tempF")) s_tempsF = p.getUChar("tempF", s_tempsF ? 1 : 0) != 0;
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
