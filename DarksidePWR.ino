// Darkside PWR — Victron power monitor for the CrowPanel Advance 3.5"
// (ESP32-S3, 480x320 ILI9488 SPI, GT911 touch). Polls the Ekrano GX over
// Modbus TCP (see gx_modbus.h for the verified register map) and renders
// the DSODash-style power screen (ui.cpp).
//
// Panel bring-up mirrors ELECROW's lesson-03 for the V1.2-V1.4 boards
// (LovyanGFX_Driver.h is their driver config, unmodified).
#include <lvgl.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include "LovyanGFX_Driver.h"
#include "gx_modbus.h"
#include "secrets.h"
#include "ui.h"

namespace {

constexpr int kLcdW = 480;
constexpr int kLcdH = 320;

LGFX gfx;
lv_disp_draw_buf_t s_drawBuf;
lv_color_t* s_buf1 = nullptr;
lv_color_t* s_buf2 = nullptr;

GxData s_gx;
uint32_t s_nextPollMs = 0;
bool s_mdnsUp = false;

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
  pinMode(38, OUTPUT);       // backlight on (vendor: plain GPIO, no PWM)
  digitalWrite(38, HIGH);

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
  uiSetLink(0);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[pwr] wifi connecting to %s\n", WIFI_SSID);
}

void loop() {
  lv_timer_handler();

  const bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp && !s_mdnsUp) {
    s_mdnsUp = MDNS.begin("darksidepwr");
    Serial.printf("[pwr] wifi up %s, mdns %s\n",
                  WiFi.localIP().toString().c_str(), s_mdnsUp ? "ok" : "FAILED");
  }

  if ((int32_t)(millis() - s_nextPollMs) >= 0) {
    s_nextPollMs = millis() + 1000;
    if (wifiUp) {
      if (gxPoll(s_gx)) {
        uiUpdate(s_gx);
        Serial.printf("[gx] soc=%d%% %.2fV %+.1fA %+dW pv=%dW alt=%dW ac=%dW dc=%dW st=%d"
                      " t=%.1f/%.1f/%.1fF lpg=%.0f/%.0f/%.0f%%\n",
                      s_gx.soc, s_gx.battV, s_gx.battA, s_gx.battW,
                      s_gx.pvW, s_gx.altW, s_gx.acW, s_gx.dcW, s_gx.battState,
                      s_gx.tempOk[0] ? s_gx.tempF[0] : -99.0f,
                      s_gx.tempOk[1] ? s_gx.tempF[1] : -99.0f,
                      s_gx.tempOk[2] ? s_gx.tempF[2] : -99.0f,
                      s_gx.tankOk[0] ? s_gx.tankPct[0] : -1.0f,
                      s_gx.tankOk[1] ? s_gx.tankPct[1] : -1.0f,
                      s_gx.tankOk[2] ? s_gx.tankPct[2] : -1.0f);
      }
    }
    const bool live = s_gx.valid && (millis() - s_gx.lastOkMs) < 10000;
    uiSetLink(!wifiUp ? 0 : (live ? 2 : 1));
  }

  delay(5);
}
