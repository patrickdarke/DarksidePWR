#include "ui_control.h"

#include <lvgl.h>
#include <stdio.h>

#include "ui_theme.h"
#include "ui_widgets.h"

// Layout: five control rows (MultiPlus mode, shore limit, relays, DVCC
// charge limit, alternator) + a hint line. Writes are queued to the poller
// task via gxWrite(); highlights follow the GX's own answers on the next
// poll, so a rejected/overridden write shows itself within about a second.
namespace {

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_prevScr = nullptr;
lv_obj_t* s_mpBtn[4] = {nullptr};        // OFF / CHG / INV / ON
lv_obj_t* s_shoreVal = nullptr;
lv_obj_t* s_relayBtn[2] = {nullptr};
lv_obj_t* s_dvccVal = nullptr;
lv_obj_t* s_altBtn[2] = {nullptr};       // ON / OFF
lv_obj_t* s_solarBtn[2] = {nullptr};     // ON / OFF
lv_obj_t* s_altNa = nullptr;
lv_obj_t* s_hint = nullptr;
lv_timer_t* s_offTimer = nullptr;        // disarms the OFF confirm

// vebus /Mode values in display order OFF, CHG, INV, ON.
constexpr uint16_t kMpModes[4] = {4, 1, 2, 3};
constexpr const char* kMpLabels[4] = {"OFF", "CHG", "INV", "ON"};

GxData s_last;            // latest sample (for +/- math and highlights)
bool s_offArmed = false;  // MULTI OFF confirm state

void setHint(const char* txt) { lv_label_set_text(s_hint, txt); }

// Queue a write and surface the result — a full queue (e.g. tapping away
// during an outage) must not look like success.
void sendCmd(uint8_t unit, uint16_t reg, uint16_t val) {
  setHint(gxWrite(unit, reg, val) ? "sent - waiting for GX read-back"
                                  : "queue full - command not sent");
}

void styleChip(lv_obj_t* b, bool active, bool known) {
  lv_obj_set_style_bg_color(b, lv_color_hex(active ? kTeal : kTile), 0);
  lv_obj_t* l = lv_obj_get_child(b, 0);
  lv_obj_set_style_text_color(
      l, lv_color_hex(active ? kBg : (known ? kText : kMuted)), 0);
}

void disarmOff() {
  s_offArmed = false;
  lv_obj_t* l = lv_obj_get_child(s_mpBtn[0], 0);
  lv_label_set_text(l, kMpLabels[0]);
  lv_obj_set_style_bg_color(s_mpBtn[0], lv_color_hex(kTile), 0);
}

void offTimerCb(lv_timer_t*) {
  if (s_offArmed) {
    disarmOff();
    uiCtlUpdate(s_last);  // restore normal highlight
  }
  lv_timer_pause(s_offTimer);
}

void mpCb(lv_event_t* e) {
  const int i = (int)(uintptr_t)lv_event_get_user_data(e);
  const uint16_t mode = kMpModes[i];
  if (mode == 4 && !s_offArmed) {
    // OFF kills the AC loads — ask twice.
    s_offArmed = true;
    lv_obj_t* l = lv_obj_get_child(s_mpBtn[0], 0);
    lv_label_set_text(l, "SURE?");
    lv_obj_set_style_bg_color(s_mpBtn[0], lv_color_hex(kRed), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(kBg), 0);
    lv_timer_reset(s_offTimer);
    lv_timer_resume(s_offTimer);
    setHint("tap OFF again to confirm");
    return;
  }
  if (mode == 4) disarmOff();
  sendCmd(GX_VEBUS_UNIT, 33, mode);
}

void shoreCb(lv_event_t* e) {
  if (!s_last.shoreLimOk) return;
  const int dir = (int)(uintptr_t)lv_event_get_user_data(e);
  float a = s_last.shoreLimA + dir * 5.0f;
  if (a < 5.0f) a = 5.0f;
  if (a > 50.0f) a = 50.0f;
  sendCmd(GX_VEBUS_UNIT, 22, (uint16_t)(a * 10.0f + 0.5f));
}

void relayCb(lv_event_t* e) {
  if (!s_last.relayOk) return;
  const int i = (int)(uintptr_t)lv_event_get_user_data(e);
  sendCmd(100, (uint16_t)(806 + i), s_last.relayClosed[i] ? 0 : 1);
}

void dvccCb(lv_event_t* e) {
  if (!s_last.dvccOk) return;
  const int dir = (int)(uintptr_t)lv_event_get_user_data(e);
  // Ordered 10..100 A, then NO LIMIT (-1) above 100.
  int a = s_last.dvccLimA;
  if (dir > 0) {
    if (a < 0) return;               // already NO LIMIT
    a = (a >= 100) ? -1 : a + 10;
  } else {
    a = (a < 0) ? 100 : ((a <= 10) ? 10 : a - 10);
  }
  sendCmd(100, 2705, (uint16_t)(int16_t)a);
}

void solarCb(lv_event_t* e) {
  if (GX_SOLAR_UNIT == 0 || !s_last.solarModeOk) return;
  const int on = (int)(uintptr_t)lv_event_get_user_data(e);
  sendCmd(GX_SOLAR_UNIT, 774, on ? 1 : 4);
}

void altCb(lv_event_t* e) {
  if (!s_last.altModeOk) return;
  const int on = (int)(uintptr_t)lv_event_get_user_data(e);
  sendCmd(GX_ALT_UNIT, 4119, on ? 1 : 4);
}

void closeCb(lv_event_t*) {
  disarmOff();
  lv_scr_load(s_prevScr);
}

lv_obj_t* mkChip(int x, int y, int w, int h, const char* txt, lv_event_cb_t cb,
                 int userData) {
  return uiwButton(s_scr, x, y, w, h, txt, kTile, kText, cb,
                   (void*)(uintptr_t)userData);
}

lv_obj_t* mkRowLabel(int y, const char* txt) {
  lv_obj_t* l = lv_label_create(s_scr);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(kMuted), 0);
  lv_label_set_text(l, txt);
  lv_obj_set_pos(l, 16, y);
  return l;
}

lv_obj_t* mkValLabel(int x, int y, int w) {
  lv_obj_t* l = lv_label_create(s_scr);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(kText), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(l, w);
  lv_obj_set_pos(l, x, y);
  lv_label_set_text(l, "--");
  return l;
}

}  // namespace

void uiCtlBuild() {
  s_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(kBg), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

  lv_obj_t* hdr = lv_label_create(s_scr);
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hdr, lv_color_hex(kMuted), 0);
  lv_label_set_text(hdr, "CONTROL");
  lv_obj_set_pos(hdr, 16, 10);

  lv_obj_t* close = mkChip(368, 6, 96, 30, LV_SYMBOL_CLOSE "  CLOSE", closeCb, 0);
  lv_obj_set_style_bg_color(close, lv_color_hex(kRing), 0);

  // Six rows at a 44 px pitch, 36 px chips (compressed when SOLAR joined).
  mkRowLabel(52, "MULTI");
  for (int i = 0; i < 4; i++)
    s_mpBtn[i] = mkChip(136 + i * 86, 40, 78, 36, kMpLabels[i], mpCb, i);

  mkRowLabel(96, "SHORE A");
  mkChip(136, 84, 56, 36, LV_SYMBOL_MINUS, shoreCb, -1);
  s_shoreVal = mkValLabel(200, 92, 96);
  mkChip(304, 84, 56, 36, LV_SYMBOL_PLUS, shoreCb, +1);

  mkRowLabel(140, "RELAYS");
  s_relayBtn[0] = mkChip(136, 128, 150, 36, "R1  --", relayCb, 0);
  s_relayBtn[1] = mkChip(296, 128, 150, 36, "R2  --", relayCb, 1);

  mkRowLabel(184, "CHG LIMIT");
  mkChip(136, 172, 56, 36, LV_SYMBOL_MINUS, dvccCb, -1);
  s_dvccVal = mkValLabel(200, 180, 120);
  mkChip(328, 172, 56, 36, LV_SYMBOL_PLUS, dvccCb, +1);

  mkRowLabel(228, "ALTERNATOR");
  s_altBtn[0] = mkChip(136, 216, 90, 36, "ON", altCb, 1);
  s_altBtn[1] = mkChip(234, 216, 90, 36, "OFF", altCb, 0);
  s_altNa = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_altNa, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_altNa, lv_color_hex(kMuted), 0);
  lv_label_set_text(s_altNa, "needs newer\nGX firmware");
  lv_obj_set_width(s_altNa, 126);
  lv_obj_set_pos(s_altNa, 338, 220);
  lv_obj_add_flag(s_altNa, LV_OBJ_FLAG_HIDDEN);

  mkRowLabel(272, "SOLAR");
  s_solarBtn[0] = mkChip(136, 260, 90, 36, "ON", solarCb, 1);
  s_solarBtn[1] = mkChip(234, 260, 90, 36, "OFF", solarCb, 0);

  s_hint = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_hint, lv_color_hex(kMuted), 0);
  lv_label_set_text(s_hint, "states show GX read-back   OFF asks twice");
  lv_obj_set_pos(s_hint, 16, 302);

  s_offTimer = lv_timer_create(offTimerCb, 3000, nullptr);
  lv_timer_pause(s_offTimer);
}

void uiCtlOpen() {
  s_prevScr = lv_scr_act();
  disarmOff();
  lv_scr_load(s_scr);
}

void uiCtlDemoArmOff() {
  // Real first-tap path via the button's own event — but never a second
  // one, so 'D' can't confirm an armed OFF.
  if (!s_offArmed) lv_event_send(s_mpBtn[0], LV_EVENT_CLICKED, nullptr);
}

void uiCtlUpdate(const GxData& d) {
  s_last = d;
  char buf[24];

  for (int i = 0; i < 4; i++) {
    if (i == 0 && s_offArmed) continue;  // don't clobber the SURE? state
    styleChip(s_mpBtn[i], d.mpModeOk && d.mpMode == (int)kMpModes[i], d.mpModeOk);
  }

  if (d.shoreLimOk) {
    snprintf(buf, sizeof buf, "%.1f A", d.shoreLimA);
    lv_label_set_text(s_shoreVal, buf);
  } else {
    lv_label_set_text(s_shoreVal, "--");
  }

  for (int i = 0; i < 2; i++) {
    if (d.relayOk) {
      snprintf(buf, sizeof buf, "R%d  %s", i + 1,
               d.relayClosed[i] ? "CLOSED" : "OPEN");
      lv_obj_t* l = lv_obj_get_child(s_relayBtn[i], 0);
      lv_label_set_text(l, buf);
      styleChip(s_relayBtn[i], d.relayClosed[i], true);
    } else {
      lv_obj_t* l = lv_obj_get_child(s_relayBtn[i], 0);
      snprintf(buf, sizeof buf, "R%d  --", i + 1);
      lv_label_set_text(l, buf);
      styleChip(s_relayBtn[i], false, false);
    }
  }

  if (d.dvccOk) {
    if (d.dvccLimA < 0) lv_label_set_text(s_dvccVal, "NO LIMIT");
    else {
      snprintf(buf, sizeof buf, "%d A", d.dvccLimA);
      lv_label_set_text(s_dvccVal, buf);
    }
  } else {
    lv_label_set_text(s_dvccVal, "--");
  }

  if (d.solarModeOk) {
    styleChip(s_solarBtn[0], d.solarMode == 1, true);
    styleChip(s_solarBtn[1], d.solarMode == 4, true);
  } else {
    styleChip(s_solarBtn[0], false, false);
    styleChip(s_solarBtn[1], false, false);
  }

  if (d.altModeOk) {
    lv_obj_add_flag(s_altNa, LV_OBJ_FLAG_HIDDEN);
    styleChip(s_altBtn[0], d.altMode == 1, true);
    styleChip(s_altBtn[1], d.altMode == 4, true);
  } else {
    // The firmware note means "this GX lacks the register" — a plain
    // disconnect or fault round just mutes the chips like every other row.
    if (!d.altModeSupported) lv_obj_clear_flag(s_altNa, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_altNa, LV_OBJ_FLAG_HIDDEN);
    styleChip(s_altBtn[0], false, false);
    styleChip(s_altBtn[1], false, false);
  }
}
