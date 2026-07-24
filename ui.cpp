#include "ui.h"

#include <lvgl.h>
#include <stdio.h>

#include "secrets.h"  // GX_TEMP_LABELS for the temps line

// Darkside PWR — single 480x320 power screen in the DSODash design language:
// SOC arc on the left, four metric tiles on the right, totals bar below.
// Palette mirrors the DSODash instrument colors.
namespace {

constexpr uint32_t kBg = 0x0C1018;
constexpr uint32_t kTile = 0x111826;
constexpr uint32_t kRing = 0x1D2634;
constexpr uint32_t kText = 0xE8F0FA;
constexpr uint32_t kMuted = 0x8C96AA;
constexpr uint32_t kTeal = 0x46C8A5;
constexpr uint32_t kGreen = 0x30C860;
constexpr uint32_t kAmber = 0xFFC85A;
constexpr uint32_t kBlue = 0x4696FF;
constexpr uint32_t kRed = 0xFF5050;

lv_obj_t* s_arc = nullptr;
lv_obj_t* s_socLbl = nullptr;
lv_obj_t* s_stateLbl = nullptr;
lv_obj_t* s_dot = nullptr;
lv_obj_t* s_tileVal[4] = {nullptr};   // house V, battery A, solar W, AC W
lv_obj_t* s_tempLbl = nullptr;
lv_obj_t* s_tankLbl = nullptr;
lv_obj_t* s_footLbl = nullptr;
const char* kTempNames[GxData::kNumTemps] = GX_TEMP_LABELS;
const char* kTankNames[GxData::kNumTanks] = GX_TANK_LABELS;
constexpr float kTankLowPct = 20.0f;  // below this the level renders RED

lv_obj_t* mkTile(lv_obj_t* parent, int x, int y, const char* title, uint32_t valColor) {
  lv_obj_t* t = lv_obj_create(parent);
  lv_obj_remove_style_all(t);
  lv_obj_set_style_bg_color(t, lv_color_hex(kTile), 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(t, 8, 0);
  lv_obj_set_size(t, 128, 92);
  lv_obj_set_pos(t, x, y);

  lv_obj_t* lbl = lv_label_create(t);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(kMuted), 0);
  lv_label_set_text(lbl, title);
  lv_obj_set_pos(lbl, 12, 10);

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
  lv_obj_t* hdr = lv_label_create(scr);
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hdr, lv_color_hex(kMuted), 0);
  lv_label_set_text(hdr, "DARKSIDE  PWR");
  lv_obj_set_pos(hdr, 16, 10);

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
  s_tileVal[0] = mkTile(scr, 216, 46, "HOUSE", kText);
  s_tileVal[1] = mkTile(scr, 348, 46, "CURRENT", kAmber);
  s_tileVal[2] = mkTile(scr, 216, 142, "POWER IN", kGreen);
  s_tileVal[3] = mkTile(scr, 348, 142, "AC LOADS", kBlue);

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
  lv_label_set_text(s_tempLbl, "TEMPS --");
  lv_obj_set_pos(s_tempLbl, 16, 254);

  s_tankLbl = lv_label_create(scr);
  lv_obj_set_style_text_font(s_tankLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_tankLbl, lv_color_hex(kMuted), 0);
  lv_label_set_recolor(s_tankLbl, true);
  lv_label_set_text(s_tankLbl, "LPG --");
  lv_obj_set_pos(s_tankLbl, 16, 276);

  s_footLbl = lv_label_create(scr);
  lv_obj_set_style_text_font(s_footLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_footLbl, lv_color_hex(kMuted), 0);
  lv_label_set_text(s_footLbl, "DC -- W   ·   NET -- W");
  lv_obj_set_pos(s_footLbl, 16, 298);
}

void uiUpdate(const GxData& d) {
  if (!d.valid) return;
  char buf[48];

  lv_arc_set_value(s_arc, d.soc);
  snprintf(buf, sizeof buf, "%d%%", d.soc);
  lv_label_set_text(s_socLbl, buf);

  const char* st = (d.battState == 1) ? "CHARGING"
                   : (d.battState == 2) ? "DISCHARGING" : "IDLE";
  lv_label_set_text(s_stateLbl, st);

  snprintf(buf, sizeof buf, "%.2f V", d.battV);
  lv_label_set_text(s_tileVal[0], buf);
  snprintf(buf, sizeof buf, "%+.1f A", d.battA);
  lv_label_set_text(s_tileVal[1], buf);
  snprintf(buf, sizeof buf, "%d W", d.inW);
  lv_label_set_text(s_tileVal[2], buf);
  snprintf(buf, sizeof buf, "%d W", d.acW);
  lv_label_set_text(s_tileVal[3], buf);

  // Temps line: muted names, bright values (LVGL recolor markup); a sensor
  // that didn't answer shows "--" rather than a stale number.
  char temps[128];
  int off = 0;
  for (int i = 0; i < GxData::kNumTemps; i++) {
    if (d.tempOk[i])
      off += snprintf(temps + off, sizeof(temps) - off,
                      "%s#8c96aa %s# #e8f0fa %.1f\xC2\xB0#",
                      i ? "   " : "", kTempNames[i], d.tempF[i]);
    else
      off += snprintf(temps + off, sizeof(temps) - off,
                      "%s#8c96aa %s# #8c96aa --#", i ? "   " : "", kTempNames[i]);
  }
  lv_label_set_text(s_tempLbl, temps);

  // Tanks line: level goes RED below kTankLowPct; dead sensors show '--'.
  char tanks[144];
  off = 0;
  for (int i = 0; i < GxData::kNumTanks; i++) {
    if (d.tankOk[i]) {
      const char* col = (d.tankPct[i] < kTankLowPct) ? "ff5050" : "e8f0fa";
      off += snprintf(tanks + off, sizeof(tanks) - off,
                      "%s#8c96aa %s# #%s %.0f%%#",
                      i ? "   " : "", kTankNames[i], col, d.tankPct[i]);
    } else {
      off += snprintf(tanks + off, sizeof(tanks) - off,
                      "%s#8c96aa %s# #8c96aa --#", i ? "   " : "", kTankNames[i]);
    }
  }
  lv_label_set_text(s_tankLbl, tanks);

  snprintf(buf, sizeof buf, "PV %d W  ·  ALT %d W  ·  DC %d W  ·  NET %+d W",
           d.pvW, d.altW, d.dcW, d.battW);
  lv_label_set_text(s_footLbl, buf);
}

void uiSetLink(int state) {
  uint32_t c = (state >= 2) ? kGreen : (state == 1) ? kAmber : kRed;
  lv_obj_set_style_bg_color(s_dot, lv_color_hex(c), 0);
}
