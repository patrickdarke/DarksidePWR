#include "ui.h"

#include <WiFi.h>
#include <lvgl.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gx_settings.h"
#include "history.h"
#include "ui_control.h"
#include "ui_history.h"
#include "ui_setup.h"
#include "ui_theme.h"
#include "ui_widgets.h"

// Every installation-flavored string renders through gxLabel() — config.h/
// secrets.h supply the defaults, the setup screen's LABELS page overrides
// via NVS. Title rule: a non-empty title label wins; "" = show the GX
// system name as soon as a poll delivers it.

// Darkside PWR — 480x320 power screen in the DSODash design language:
// SOC arc on the left, four metric tiles on the right, totals bar below,
// gear button (lower right) into the Wi-Fi setup screen.
namespace {

lv_obj_t* s_hdrLbl = nullptr;
lv_obj_t* s_clockLbl = nullptr;
lv_obj_t* s_arc = nullptr;
lv_obj_t* s_socLbl = nullptr;
lv_obj_t* s_stateLbl = nullptr;
lv_obj_t* s_dot = nullptr;
lv_obj_t* s_tileVal[4] = {nullptr};   // house V, battery A, solar W, AC W
lv_obj_t* s_tileTitle[4] = {nullptr}; // their name labels, for live renames
lv_obj_t* s_tempLbl = nullptr;
lv_obj_t* s_tankLbl = nullptr;
lv_obj_t* s_footLbl = nullptr;
char s_sysShown[17] = "";             // auto-title cache (GX system name)
// Label slot indexes for the sensor names (fixed slots come first).
constexpr int kTempLblBase = kGxLblFixedCount;
constexpr int kTankLblBase = kGxLblFixedCount + GxData::kNumTemps;
constexpr float kTankLowPct = 20.0f;  // below this the level renders RED

// Top-right rotator: the wall clock and the panel's IP trade places every
// 4 s (the IP is the door to the sound-upload page). The 1 Hz timer makes
// each phase exactly four ticks. Clock is blank until the first SNTP sync
// (unsynced ESP time starts in 1970), the IP while Wi-Fi is down —
// whichever side is empty cedes its slot to the other.
void clockTimerCb(lv_timer_t*) {
  static char last[28] = "";
  char clk[24] = "";
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  if (t.tm_year + 1900 >= 2020) {
    static const char* kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    int h = t.tm_hour % 12;
    if (h == 0) h = 12;
    snprintf(clk, sizeof clk, "%s %d/%d  %d:%02d %s", kDays[t.tm_wday],
             t.tm_mon + 1, t.tm_mday, h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  }
  char ip[20] = "";
  if (WiFi.status() == WL_CONNECTED)
    snprintf(ip, sizeof ip, "%s", WiFi.localIP().toString().c_str());

  const bool ipTurn = (millis() / 4000) & 1;
  const char* show = ipTurn ? (ip[0] ? ip : clk) : (clk[0] ? clk : ip);
  if (strcmp(show, last) != 0) {
    snprintf(last, sizeof last, "%s", show);
    lv_label_set_text(s_clockLbl, show);
  }
}

// Set a label only when the text actually changed — steady-state polls
// mostly repeat values, and every set_text costs a heap realloc + region
// invalidate + redraw + flush even when identical.
void setIfChanged(lv_obj_t* l, const char* txt) {
  if (strcmp(lv_label_get_text(l), txt) != 0) lv_label_set_text(l, txt);
}

lv_obj_t* mkTile(lv_obj_t* parent, int x, int y, const char* title, uint32_t valColor,
                 int series, lv_obj_t** titleOut) {
  lv_obj_t* t = lv_obj_create(parent);
  lv_obj_remove_style_all(t);
  lv_obj_set_style_bg_color(t, lv_color_hex(kTile), 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(t, 8, 0);
  lv_obj_set_size(t, 128, 92);
  lv_obj_set_pos(t, x, y);
  // Tap a tile -> that value's 24 h chart.
  lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      t, [](lv_event_t* e) { uiHistOpen((int)(uintptr_t)lv_event_get_user_data(e)); },
      LV_EVENT_CLICKED, (void*)(uintptr_t)series);

  lv_obj_t* lbl = lv_label_create(t);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(kMuted), 0);
  lv_label_set_text(lbl, title);
  lv_obj_set_pos(lbl, 12, 10);
  if (titleOut) *titleOut = lbl;

  lv_obj_t* val = lv_label_create(t);
  lv_obj_set_style_text_font(val, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(val, lv_color_hex(valColor), 0);
  lv_label_set_text(val, "--");
  lv_obj_set_pos(val, 12, 38);
  return val;
}

}  // namespace

void uiBuild() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Header
  s_hdrLbl = lv_label_create(scr);
  lv_obj_set_style_text_font(s_hdrLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_hdrLbl, lv_color_hex(kMuted), 0);
  const char* title = gxLabel(kGxLblTitle);
  lv_label_set_text(s_hdrLbl, title[0] ? title : "PWR MONITOR");
  lv_obj_set_pos(s_hdrLbl, 16, 10);

  s_clockLbl = lv_label_create(scr);
  lv_obj_set_style_text_font(s_clockLbl, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(s_clockLbl, lv_color_hex(kMuted), 0);
  lv_obj_set_style_text_align(s_clockLbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_width(s_clockLbl, 190);
  lv_label_set_text(s_clockLbl, "");
  lv_obj_set_pos(s_clockLbl, 256, 8);
  lv_timer_create(clockTimerCb, 1000, nullptr);

  s_dot = lv_obj_create(scr);
  lv_obj_remove_style_all(s_dot);
  lv_obj_set_size(s_dot, 10, 10);
  lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_dot, lv_color_hex(kRed), 0);
  lv_obj_set_pos(s_dot, 454, 14);

  // SOC arc, left half
  s_arc = lv_arc_create(scr);
  lv_obj_set_size(s_arc, 190, 190);
  lv_obj_set_pos(s_arc, 18, 46);
  lv_arc_set_rotation(s_arc, 135);
  lv_arc_set_bg_angles(s_arc, 0, 270);
  lv_arc_set_range(s_arc, 0, 100);
  lv_arc_set_value(s_arc, 0);
  lv_obj_remove_style(s_arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_arc, lv_color_hex(kRing), LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_arc, lv_color_hex(kTeal), LV_PART_INDICATOR);

  s_socLbl = lv_label_create(scr);
  lv_obj_set_style_text_font(s_socLbl, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(s_socLbl, lv_color_hex(kText), 0);
  lv_label_set_text(s_socLbl, "--");
  lv_obj_set_style_text_align(s_socLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(s_socLbl, 190);
  lv_obj_set_pos(s_socLbl, 18, 116);

  s_stateLbl = lv_label_create(scr);
  lv_obj_set_style_text_font(s_stateLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_stateLbl, lv_color_hex(kMuted), 0);
  lv_label_set_text(s_stateLbl, "WAITING FOR GX");
  lv_obj_set_style_text_align(s_stateLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(s_stateLbl, 190);
  lv_obj_set_pos(s_stateLbl, 18, 166);

  // 2x2 tiles, right half
  s_tileVal[0] = mkTile(scr, 216, 46, gxLabel(kGxLblBatt), kText, kHistV, &s_tileTitle[0]);
  s_tileVal[1] = mkTile(scr, 348, 46, gxLabel(kGxLblCurrent), kAmber, kHistA, &s_tileTitle[1]);
  s_tileVal[2] = mkTile(scr, 216, 142, gxLabel(kGxLblPowerIn), kGreen, kHistInW, &s_tileTitle[2]);
  s_tileVal[3] = mkTile(scr, 348, 142, gxLabel(kGxLblAcLoads), kBlue, kHistAcW, &s_tileTitle[3]);

  // Footer block: rule + temps / tanks / power lines
  lv_obj_t* rule = lv_obj_create(scr);
  lv_obj_remove_style_all(rule);
  lv_obj_set_style_bg_color(rule, lv_color_hex(kRing), 0);
  lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
  lv_obj_set_size(rule, 448, 1);
  lv_obj_set_pos(rule, 16, 248);

  s_tempLbl = lv_label_create(scr);
  lv_obj_set_style_text_font(s_tempLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_tempLbl, lv_color_hex(kMuted), 0);
  lv_label_set_recolor(s_tempLbl, true);
  lv_label_set_text(s_tempLbl, GxData::kNumTemps ? "TEMPS --" : "");
  lv_obj_set_pos(s_tempLbl, 16, 254);

  s_tankLbl = lv_label_create(scr);
  lv_obj_set_style_text_font(s_tankLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_tankLbl, lv_color_hex(kMuted), 0);
  lv_label_set_recolor(s_tankLbl, true);
  lv_label_set_text(s_tankLbl, GxData::kNumTanks ? "LPG --" : "");
  lv_obj_set_pos(s_tankLbl, 16, 276);

  s_footLbl = lv_label_create(scr);
  lv_obj_set_style_text_font(s_footLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_footLbl, lv_color_hex(kMuted), 0);
  lv_label_set_text(s_footLbl, "DC -- W   NET -- W");
  lv_obj_set_pos(s_footLbl, 16, 298);

  // Chip buttons, lower right (footer text lines end well left of them):
  // bolt -> CONTROL page, gear -> setup screen. Radius 8 marks them as
  // quiet corner affordances.
  struct Chip { int x; const char* sym; void (*open)(); };
  static const Chip kChips[] = {
      {384, LV_SYMBOL_CHARGE, uiCtlOpen},
      {432, LV_SYMBOL_SETTINGS, uiSetupOpen},
  };
  for (const Chip& c : kChips) {
    lv_obj_t* b = uiwButton(
        scr, c.x, 280, 40, 32, c.sym, kTile, kMuted,
        [](lv_event_t* e) { ((void (*)())lv_event_get_user_data(e))(); },
        (void*)c.open);
    lv_obj_set_style_radius(b, 8, 0);
  }
}

void uiUpdate(const GxData& d) {
  if (!d.valid) return;
  char buf[64];

  // Auto title: follow the GX system name when no title label is set.
  if (gxLabel(kGxLblTitle)[0] == '\0' && d.sysNameOk) {
    if (strcmp(s_sysShown, d.sysName) != 0) {
      strcpy(s_sysShown, d.sysName);
      char up[17];
      for (int i = 0; i < 17; i++) up[i] = (char)toupper((unsigned char)s_sysShown[i]);
      lv_label_set_text(s_hdrLbl, up);
    }
  }

  lv_arc_set_value(s_arc, d.soc);
  snprintf(buf, sizeof buf, "%d%%", d.soc);
  setIfChanged(s_socLbl, buf);

  const char* st = (d.battState == 1) ? "CHARGING"
                   : (d.battState == 2) ? "DISCHARGING" : "IDLE";
  setIfChanged(s_stateLbl, st);

  snprintf(buf, sizeof buf, "%.2f V", d.battV);
  setIfChanged(s_tileVal[0], buf);
  snprintf(buf, sizeof buf, "%+.1f A", d.battA);
  setIfChanged(s_tileVal[1], buf);
  snprintf(buf, sizeof buf, "%d W", d.inW);
  setIfChanged(s_tileVal[2], buf);
  snprintf(buf, sizeof buf, "%d W", d.acW);
  setIfChanged(s_tileVal[3], buf);

  // Temps line: muted names, bright values (LVGL recolor markup); a sensor
  // that didn't answer shows "--" rather than a stale number.
  char temps[192];
  int off = 0;
  for (int i = 0; i < GxData::kNumTemps && off < (int)sizeof(temps); i++) {
    if (d.tempOk[i])
      off += snprintf(temps + off, sizeof(temps) - off,
                      "%s#8c96aa %s# #e8f0fa %.1f\xC2\xB0#",
                      i ? "   " : "", gxLabel(kTempLblBase + i), d.temp[i]);
    else
      off += snprintf(temps + off, sizeof(temps) - off,
                      "%s#8c96aa %s# #8c96aa --#", i ? "   " : "",
                      gxLabel(kTempLblBase + i));
  }
  setIfChanged(s_tempLbl, temps);

  // Tanks line: level goes RED below kTankLowPct; dead sensors show '--'.
  char tanks[192];
  off = 0;
  for (int i = 0; i < GxData::kNumTanks && off < (int)sizeof(tanks); i++) {
    if (d.tankOk[i]) {
      const char* col = (d.tankPct[i] < kTankLowPct) ? "ff5050" : "e8f0fa";
      off += snprintf(tanks + off, sizeof(tanks) - off,
                      "%s#8c96aa %s# #%s %.0f%%#",
                      i ? "   " : "", gxLabel(kTankLblBase + i), col, d.tankPct[i]);
    } else {
      off += snprintf(tanks + off, sizeof(tanks) - off,
                      "%s#8c96aa %s# #8c96aa --#", i ? "   " : "",
                      gxLabel(kTankLblBase + i));
    }
  }
  setIfChanged(s_tankLbl, tanks);

  // Triple-space separators like the lines above — the middle dot (U+00B7)
  // is outside LVGL's montserrat glyph range and renders as a box.
  snprintf(buf, sizeof buf, "PV %d W   ALT %d W   DC %d W   NET %+d W",
           d.pvW, d.altW, d.dcW, d.battW);
  setIfChanged(s_footLbl, buf);
}

void uiSetLink(int state) {
  uint32_t c = (state >= 2) ? kGreen : (state == 1) ? kAmber : kRed;
  lv_obj_set_style_bg_color(s_dot, lv_color_hex(c), 0);
}

void uiLabelsApply() {
  const char* title = gxLabel(kGxLblTitle);
  lv_label_set_text(s_hdrLbl, title[0] ? title : "PWR MONITOR");
  s_sysShown[0] = '\0';  // title cleared to auto: let the next poll repaint
  static const int kTileSlot[4] = {kGxLblBatt, kGxLblCurrent, kGxLblPowerIn,
                                   kGxLblAcLoads};
  for (int i = 0; i < 4; i++)
    if (s_tileTitle[i]) lv_label_set_text(s_tileTitle[i], gxLabel(kTileSlot[i]));
}
