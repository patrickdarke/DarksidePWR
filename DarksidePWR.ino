// Darkside PWR — Victron power monitor, two board targets selected at
// compile time by BOARD_CROWPANEL_50 (see build.sh):
//   - default: CrowPanel Advance 3.5" (ESP32-S3, 480x320 ILI9488 SPI,
//     GT911 touch) — the truck's install.
//   - BOARD_CROWPANEL_50: CrowPanel Advance 5.0" (ESP32-S3, 800x480
//     ST7262 RGB-parallel, GT911 touch) — a second, separate install with
//     its own Victron device roster (see config_50.h, CLAUDE.md).
// Polls the GX over Modbus TCP (gx_poller.h owns the register map; framing
// in modbus_transport, runtime settings in gx_settings) and renders the
// DSODash-style power screen (ui.cpp).
//
// Panel bring-up mirrors ELECROW's lesson-03 for each board
// (LovyanGFX_Driver_35.h / LovyanGFX_Driver_50.h are their driver configs,
// unmodified).
#include <lvgl.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include "LovyanGFX_Driver.h"
#include "backlight.h"
#include "beeper.h"
#include "gx_poller.h"
#include "gx_settings.h"
#include "night_mode.h"
#include "secrets.h"
#include "ui.h"
#include "ui_control.h"
#include "ui_setup.h"

namespace {

#if defined(BOARD_CROWPANEL_50)
constexpr int kLcdW = 800;
constexpr int kLcdH = 480;
#else
constexpr int kLcdW = 480;
constexpr int kLcdH = 320;
#endif

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

// Full-charge chime: arm after >= 30 consecutive charging samples (~30 s,
// so absorption flapping and passing clouds can't arm it), fire once when
// charging stops with SOC at/above the threshold, re-arm on the next
// charge session.
constexpr int kChimeSocPct = 99;
int s_chargeRun = 0;
bool s_chimeArmed = false;

#if defined(BOARD_CROWPANEL_50)
// RGB-parallel panel: no per-flush setAddrWindow/writePixels (that's an
// SPI-panel command sequence) — push straight into the continuously-scanned
// PSRAM frame buffer via DMA. Pattern verified against ELECROW's
// V1.2_and_V1.3 lesson-03 vendor example.
void dispFlush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  const uint32_t w = area->x2 - area->x1 + 1;
  const uint32_t h = area->y2 - area->y1 + 1;
  if (gfx.getStartCount() > 0) gfx.endWrite();
  gfx.pushImageDMA(area->x1, area->y1, w, h, (lgfx::rgb565_t*)&px->full);
  lv_disp_flush_ready(drv);
}
#else
void dispFlush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  const uint32_t w = area->x2 - area->x1 + 1;
  const uint32_t h = area->y2 - area->y1 + 1;
  gfx.startWrite();
  gfx.setAddrWindow(area->x1, area->y1, w, h);
  gfx.writePixels((lgfx::rgb565_t*)&px->full, w * h);
  gfx.endWrite();
  lv_disp_flush_ready(drv);
}
#endif

// Debug: dump the active screen over serial as hex RGB565 (LV_USE_SNAPSHOT
// renders the widget tree into a PSRAM buffer). Framed by "[shot] begin/end"
// lines; other tasks' log lines may interleave BETWEEN hex lines — decoders
// should keep only pure-hex lines. Triggered by serial command 'S'.
void dumpScreen() {
  lv_obj_t* scr = lv_scr_act();
  const uint32_t sz = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
  uint8_t* buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
  if (!buf) {
    Serial.println("[shot] alloc failed");
    return;
  }
  lv_img_dsc_t dsc;
  if (lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &dsc, buf, sz) != LV_RES_OK) {
    Serial.println("[shot] snapshot failed");
    heap_caps_free(buf);
    return;
  }
  // The dump must not lose bytes, so give TX a real timeout for its
  // duration (the caller is a draining capture script, and the UI stalling
  // during an explicit screenshot is fine); restored to 0 below.
  // setTxTimeoutMs is HWCDC-only (native USB-CDC, the 3.5" board) — the
  // 5" board's Serial is plain HardwareSerial over its UART bridge, which
  // has no equivalent API and doesn't have the same backpressure failure
  // mode this works around (see CLAUDE.md's HWCDC backpressure law).
#if !defined(BOARD_CROWPANEL_50)
  Serial.setTxTimeoutMs(250);
#endif
  Serial.printf("[shot] begin %dx%d rgb565le\n", dsc.header.w, dsc.header.h);
  static const char kHex[] = "0123456789abcdef";
  char line[129];
  const uint8_t* p = dsc.data;
  // data_size is not filled in by lv_snapshot_take_to_buf (LVGL 8.3) —
  // compute it: TRUE_COLOR at LV_COLOR_DEPTH 16 is 2 bytes per pixel.
  uint32_t n = (uint32_t)dsc.header.w * dsc.header.h * 2;
  while (n) {
    const uint32_t chunk = (n > 64) ? 64 : n;
    for (uint32_t i = 0; i < chunk; i++) {
      line[2 * i] = kHex[p[i] >> 4];
      line[2 * i + 1] = kHex[p[i] & 0xF];
    }
    line[2 * chunk] = '\0';
    Serial.println(line);
    p += chunk;
    n -= chunk;
  }
  Serial.println("[shot] end");
#if !defined(BOARD_CROWPANEL_50)
  Serial.setTxTimeoutMs(0);
#endif
  heap_caps_free(buf);
}

// Append " key=value" (or " key=-" when the read didn't answer) to the
// telemetry line.
void telKV(char* tel, int& off, size_t cap, const char* key, bool ok, int v) {
  if (off >= (int)cap) return;
  off += ok ? snprintf(tel + off, cap - off, " %s=%d", key, v)
            : snprintf(tel + off, cap - off, " %s=-", key);
}

// Recurring (forever, every poll/every 10s) telemetry — must never block.
// The 3.5" board's HWCDC Serial has setTxTimeoutMs(0) for exactly this; a
// HardwareSerial (this project's only other Serial) has no such API, so a
// host that has the port open but isn't draining it (e.g. plugged into a
// laptop for power with nothing reading) blocks every Serial write until
// there's room — measured live on the 5" board: 400+ seconds solid, the
// whole UI frozen. Drop the line instead of blocking when full. Explicit
// debug output (dumpScreen(), the 'M'/'V' command replies, etc.) is exempt
// on purpose — those are one-shot, user-triggered, and losing bytes there
// is worse than the brief stall, same as the 3.5" board's screenshot dump.
void telPrintln(const char* s) {
#if defined(BOARD_CROWPANEL_50)
  if (Serial.availableForWrite() < (int)strlen(s) + 2) return;
#endif
  Serial.println(s);
}

void touchRead(lv_indev_drv_t*, lv_indev_data_t* data) {
  uint16_t x, y;
  const bool pressed = gfx.getTouch(&x, &y);

  // Night mode (5" board only — a no-op stub elsewhere, so no #ifdef
  // needed here): a fresh touch-down wakes a blanked screen, but must NOT
  // also land on whatever's underneath (e.g. popping open CONTROL in the
  // dark), so a blanked screen consumes the touch here and never reports
  // it to LVGL.
  static bool s_wasPressed = false;
  const bool edge = pressed && !s_wasPressed;
  s_wasPressed = pressed;
  if (nightModeTick(edge)) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  if (pressed) {
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
  // Never let USB-CDC backpressure block us: a host that opens the port but
  // pauses draining (paused terminal, capture script mid-setup) would stall
  // every Serial print for its TX timeout — measured as multi-second UI
  // freezes. Timeout 0 = drop output when the buffer is full instead.
  // HWCDC-only API (native USB-CDC, 3.5" board) — the 5" board's Serial is
  // plain HardwareSerial over its UART bridge, with no equivalent call and
  // not the same failure mode (see CLAUDE.md's HWCDC backpressure law).
#if !defined(BOARD_CROWPANEL_50)
  Serial.setTxTimeoutMs(0);
#endif
  const char* rr;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: rr = "poweron"; break;
    case ESP_RST_SW: rr = "sw"; break;
    case ESP_RST_USB: rr = "usb"; break;
    case ESP_RST_PANIC: rr = "PANIC"; break;
    case ESP_RST_INT_WDT: rr = "INT_WDT"; break;
    case ESP_RST_TASK_WDT: rr = "TASK_WDT"; break;
    case ESP_RST_BROWNOUT: rr = "BROWNOUT"; break;
    default: rr = "other"; break;
  }
  Serial.printf("\n[pwr] Darkside PWR boot (reset: %s)\n", rr);

  // backlightInit() must run BEFORE gfx bring-up on the 50 board: its I2C
  // handshake also arms the touch controller's address (0x5D), which has to
  // happen before LovyanGFX's own touch init claims the I2C_NUM_0 port.
  // Harmless reorder for the 35 board, which doesn't touch gfx/I2C at all.
  backlightInit();           // LEDC PWM on GPIO 38 [35] / I2C 0x30 cmd [50],
                              // percent from NVS either way
#if defined(BOARD_CROWPANEL_50)
  gfx.init();
  gfx.initDMA();
  gfx.startWrite();
  gfx.fillScreen(TFT_BLACK);
#else
  gfx.begin();
  gfx.fillScreen(TFT_BLACK);
#endif
  beeperInit();               // piezo LEDC (silent) + NS4168 CTRL parked low
                               // [35] / TODO GPIO not yet confirmed [50]

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
  uiCtlBuild();
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

  nightModeInit();  // configures NTP (non-blocking) + loads the schedule
}

void loop() {
  lv_timer_handler();

  // Debug serial commands: 'S' = screenshot dump, 'U' = open setup screen,
  // 'C' = open control page, 'B' = play the charge-complete chirps,
  // 'V' = sweep unit ids for the vebus /Mode register (new installs),
  // 'K' = setup screen with keyboard up, 'D' = arm the MULTI OFF confirm
  // (the last two exist for capturing documentation screenshots),
  // 'M' = memory watermarks (LVGL pool, heap, PSRAM).
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == 'S') dumpScreen();
    else if (c == 'U') uiSetupOpen();
    else if (c == 'C') uiCtlOpen();
    else if (c == 'B') beeperChirp();
    else if (c == 'V') gxRequestSweep();
    else if (c == 'K') uiSetupShowKeyboard();
    else if (c == 'D') uiCtlDemoArmOff();
    else if (c == 'M') {
      // LV_MEM_CUSTOM=1 in this build: LVGL allocates from the ESP heap,
      // so heap-free IS the LVGL watermark (lv_mem_monitor reads zero).
      Serial.printf("[mem] heap %u (min %u)  psram %u  lvgl=heap (LV_MEM_CUSTOM)\n",
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                    (unsigned)ESP.getFreePsram());
    }
  }

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
    char gapMsg[40];
    snprintf(gapMsg, sizeof gapMsg, "[ui] max loop gap %lums", (unsigned long)s_maxGapMs);
    telPrintln(gapMsg);
    s_maxGapMs = 0;
  }

  // Render/log each new sample the poller task publishes (~1 Hz).
  uint32_t pollMs = 0;
  const uint32_t seq = gxSnapshot(s_gx, &pollMs);
  if (seq != s_lastSeq) {
    s_lastSeq = seq;
    if (s_gx.valid) {
      uiUpdate(s_gx);
      uiCtlUpdate(s_gx);

      // Full-charge chirp state machine (see the comment at the top).
      if (s_gx.battState == 1) {
        if (++s_chargeRun >= 30) s_chimeArmed = true;
      } else {
        if (s_chimeArmed && s_gx.soc >= kChimeSocPct) {
          Serial.printf("[beep] charge complete at %d%%, chime\n", s_gx.soc);
          beeperChirp();
        }
        s_chimeArmed = false;
        s_chargeRun = 0;
      }
      // One line per poll carrying every displayed value; sensor sections
      // size themselves to the secrets.h lists (missing sensors: -99 / -1).
      char tel[288];
      int off = snprintf(tel, sizeof tel,
                         "[gx] soc=%d%% %.2fV %+.1fA %+dW pv=%dW alt=%dW ac=%dW dc=%dW st=%d",
                         s_gx.soc, s_gx.battV, s_gx.battA, s_gx.battW,
                         s_gx.pvW, s_gx.altW, s_gx.acW, s_gx.dcW, s_gx.battState);
      // Sensor sections appear only when configured (counts can be zero).
      if (GxData::kNumTemps > 0 && off < (int)sizeof tel) {
        off += snprintf(tel + off, sizeof tel - off, " t=");
        for (int i = 0; i < GxData::kNumTemps && off < (int)sizeof tel; i++)
          off += snprintf(tel + off, sizeof tel - off, "%s%.1f",
                          i ? "/" : "", s_gx.tempOk[i] ? s_gx.temp[i] : -99.0f);
        if (off < (int)sizeof tel)
          off += snprintf(tel + off, sizeof tel - off, "%s", gxTempsInF() ? "F" : "C");
      }
      if (GxData::kNumTanks > 0 && off < (int)sizeof tel) {
        off += snprintf(tel + off, sizeof tel - off, " lpg=");
        for (int i = 0; i < GxData::kNumTanks && off < (int)sizeof tel; i++)
          off += snprintf(tel + off, sizeof tel - off, "%s%.0f",
                          i ? "/" : "", s_gx.tankOk[i] ? s_gx.tankPct[i] : -1.0f);
        if (off < (int)sizeof tel)
          off += snprintf(tel + off, sizeof tel - off, "%%");
      }
      // Controls read-back: MultiPlus mode, shore limit, relays, DVCC
      // charge limit, alternator mode ('-' = register didn't answer).
      telKV(tel, off, sizeof tel, "mp", s_gx.mpModeOk, s_gx.mpMode);
      if (off < (int)sizeof tel)
        off += s_gx.shoreLimOk
                   ? snprintf(tel + off, sizeof tel - off, " sh=%.1f", s_gx.shoreLimA)
                   : snprintf(tel + off, sizeof tel - off, " sh=-");
      telKV(tel, off, sizeof tel, "r1", s_gx.relayOk, s_gx.relayClosed[0] ? 1 : 0);
      telKV(tel, off, sizeof tel, "r2", s_gx.relayOk, s_gx.relayClosed[1] ? 1 : 0);
      telKV(tel, off, sizeof tel, "chg", s_gx.dvccOk, s_gx.dvccLimA);
      // Solar is a list now (an install can have more than one MPPT) — same
      // slash-joined shape as the temps/tanks sections above.
      if (GxData::kNumSolar > 0 && off < (int)sizeof tel) {
        off += snprintf(tel + off, sizeof tel - off, " sm=");
        for (int i = 0; i < GxData::kNumSolar && off < (int)sizeof tel; i++)
          off += s_gx.solarModeOk[i]
                     ? snprintf(tel + off, sizeof tel - off, "%s%d", i ? "/" : "", s_gx.solarMode[i])
                     : snprintf(tel + off, sizeof tel - off, "%s-", i ? "/" : "");
      }
      telKV(tel, off, sizeof tel, "am", s_gx.altModeOk, s_gx.altMode);
      if (off < (int)sizeof tel)
        snprintf(tel + off, sizeof tel - off, " poll=%lums", (unsigned long)pollMs);
      telPrintln(tel);
    }
  }

  if ((int32_t)(millis() - s_nextDotMs) >= 0) {
    s_nextDotMs = millis() + 1000;
    // Staleness window only — a failed round with seconds-fresh data is
    // within tolerance, so brief reconnect cycles don't blink the dot amber.
    const bool live = s_gx.lastOkMs != 0 && (millis() - s_gx.lastOkMs) < 10000;
    uiSetLink(!wifiUp ? 0 : (live ? 2 : 1));
  }

  delay(5);
}
