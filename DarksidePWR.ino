// Darkside PWR — Victron power monitor for the CrowPanel Advance 3.5"
// (ESP32-S3, 480x320 ILI9488 SPI, GT911 touch). Polls the Ekrano GX over
// Modbus TCP (see gx_modbus.h for the verified register map) and renders
// the DSODash-style power screen (ui.cpp).
//
// Panel bring-up mirrors ELECROW's lesson-03 for the V1.2-V1.4 boards
// (LovyanGFX_Driver.h is their driver config, unmodified).
#include <lvgl.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include "LovyanGFX_Driver.h"
#include "backlight.h"
#include "gx_modbus.h"
#include "secrets.h"
#include "ui.h"
#include "ui_setup.h"

namespace {

constexpr int kLcdW = 480;
constexpr int kLcdH = 320;

LGFX gfx;
lv_disp_draw_buf_t s_drawBuf;
lv_color_t* s_buf1 = nullptr;
lv_color_t* s_buf2 = nullptr;

GxData s_gx;
uint32_t s_lastSeq = 0;      // last poller sample rendered/logged
uint32_t s_nextDotMs = 0;    // link-dot refresh cadence
bool s_mdnsUp = false;

// UI-health metric: worst gap between loop passes, reported every 10 s.
// With polling on its own task this should stay in single digits.
uint32_t s_lastLoopMs = 0;
uint32_t s_maxGapMs = 0;
uint32_t s_nextGapReportMs = 0;

void dispFlush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  const uint32_t w = area->x2 - area->x1 + 1;
  const uint32_t h = area->y2 - area->y1 + 1;
  gfx.startWrite();
  gfx.setAddrWindow(area->x1, area->y1, w, h);
  gfx.writePixels((lgfx::rgb565_t*)&px->full, w * h);
  gfx.endWrite();
  lv_disp_flush_ready(drv);
}

void touchRead(lv_indev_drv_t*, lv_indev_data_t* data) {
  uint16_t x, y;
  if (gfx.getTouch(&x, &y)) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("\n[pwr] Darkside PWR boot");

  gfx.begin();
  gfx.fillScreen(TFT_BLACK);
  backlightInit();           // LEDC PWM on GPIO 38, percent from NVS

  lv_init();
  const size_t bufSz = sizeof(lv_color_t) * kLcdW * kLcdH;
  s_buf1 = (lv_color_t*)heap_caps_malloc(bufSz, MALLOC_CAP_SPIRAM);
  s_buf2 = (lv_color_t*)heap_caps_malloc(bufSz, MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&s_drawBuf, s_buf1, s_buf2, kLcdW * kLcdH);

  static lv_disp_drv_t dispDrv;
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = kLcdW;
  dispDrv.ver_res = kLcdH;
  dispDrv.flush_cb = dispFlush;
  dispDrv.draw_buf = &s_drawBuf;
  lv_disp_drv_register(&dispDrv);

  static lv_indev_drv_t indevDrv;
  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touchRead;
  lv_indev_drv_register(&indevDrv);

  uiBuild();
  uiSetupBuild();
  uiSetLink(0);

  // Wi-Fi credentials: NVS (written by the on-device setup screen) wins;
  // secrets.h is only the compile-time fallback for a fresh panel.
  String ssid = WIFI_SSID, pass = WIFI_PASS;
  bool fromNvs = false;
  Preferences prefs;
  if (prefs.begin("darkside", /*readOnly=*/true)) {
    if (prefs.isKey("wifi.ssid")) {
      ssid = prefs.getString("wifi.ssid", ssid);
      pass = prefs.getString("wifi.pass", pass);
      fromNvs = true;
    }
    prefs.end();
  }

  gxStart();  // GX poller task on core 0 — the LVGL loop never blocks on it

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (!fromNvs && ssid == "YOUR_TRUCK_SSID") {
    // Placeholder secrets and nothing in NVS: don't start a doomed join —
    // its retry loop keeps the radio busy and blocks the setup screen's scan.
    Serial.println("[pwr] no wifi credentials — tap the gear to set up");
  } else {
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("[pwr] wifi connecting to %s (%s)\n",
                  ssid.c_str(), fromNvs ? "nvs" : "secrets.h");
  }
}

void loop() {
  lv_timer_handler();

  const bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp && !s_mdnsUp) {
    s_mdnsUp = MDNS.begin("darksidepwr");
    Serial.printf("[pwr] wifi up %s, mdns %s\n",
                  WiFi.localIP().toString().c_str(), s_mdnsUp ? "ok" : "FAILED");
  }

  // Track the worst LVGL-loop stall; this is the number the net-task
  // refactor exists to keep small (it was 250-1800 ms with inline polling).
  const uint32_t now = millis();
  if (s_lastLoopMs) {
    const uint32_t gap = now - s_lastLoopMs;
    if (gap > s_maxGapMs) s_maxGapMs = gap;
  }
  s_lastLoopMs = now;
  if ((int32_t)(now - s_nextGapReportMs) >= 0) {
    s_nextGapReportMs = now + 10000;
    Serial.printf("[ui] max loop gap %lums\n", (unsigned long)s_maxGapMs);
    s_maxGapMs = 0;
  }

  // Render/log each new sample the poller task publishes (~1 Hz).
  uint32_t pollMs = 0;
  const uint32_t seq = gxSnapshot(s_gx, &pollMs);
  if (seq != s_lastSeq) {
    s_lastSeq = seq;
    if (s_gx.valid) {
      uiUpdate(s_gx);
      // One line per poll carrying every displayed value; sensor sections
      // size themselves to the secrets.h lists (missing sensors: -99 / -1).
      char tel[224];
      int off = snprintf(tel, sizeof tel,
                         "[gx] soc=%d%% %.2fV %+.1fA %+dW pv=%dW alt=%dW ac=%dW dc=%dW st=%d t=",
                         s_gx.soc, s_gx.battV, s_gx.battA, s_gx.battW,
                         s_gx.pvW, s_gx.altW, s_gx.acW, s_gx.dcW, s_gx.battState);
      for (int i = 0; i < GxData::kNumTemps && off < (int)sizeof tel; i++)
        off += snprintf(tel + off, sizeof tel - off, "%s%.1f",
                        i ? "/" : "", s_gx.tempOk[i] ? s_gx.temp[i] : -99.0f);
      if (off < (int)sizeof tel)
        off += snprintf(tel + off, sizeof tel - off, "%s lpg=", gxTempsInF() ? "F" : "C");
      for (int i = 0; i < GxData::kNumTanks && off < (int)sizeof tel; i++)
        off += snprintf(tel + off, sizeof tel - off, "%s%.0f",
                        i ? "/" : "", s_gx.tankOk[i] ? s_gx.tankPct[i] : -1.0f);
      if (off < (int)sizeof tel)
        snprintf(tel + off, sizeof tel - off, "%% poll=%lums", (unsigned long)pollMs);
      Serial.println(tel);
    }
  }

  if ((int32_t)(millis() - s_nextDotMs) >= 0) {
    s_nextDotMs = millis() + 1000;
    const bool live = s_gx.valid && (millis() - s_gx.lastOkMs) < 10000;
    uiSetLink(!wifiUp ? 0 : (live ? 2 : 1));
  }

  delay(5);
}
