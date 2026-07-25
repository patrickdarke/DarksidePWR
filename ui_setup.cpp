#include "ui_setup.h"

#include <lvgl.h>
#include <Preferences.h>
#include <WiFi.h>

#include "backlight.h"
#include "gx_settings.h"  // runtime GX target + temperature unit
#include "secrets.h"      // compile-time fallback creds for rejoin-on-CANCEL
#include "ui_theme.h"
#include "ui_widgets.h"

// Layout (480x320): header row, SSID+SCAN row, PASSWORD+SAVE row, status
// line, then the bottom region (y 152..320) shared by the scan list and the
// keyboard — exactly one of the two is visible at a time.
namespace {

constexpr int kMaxNets = 12;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_prevScr = nullptr;  // screen BACK/SAVE returns to
lv_obj_t* s_ssidTa = nullptr;
lv_obj_t* s_passTa = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_list = nullptr;
lv_obj_t* s_kb = nullptr;
lv_obj_t* s_briSlider = nullptr;
lv_obj_t* s_briLbl = nullptr;
lv_obj_t* s_gxTa = nullptr;
lv_obj_t* s_unitLbl = nullptr;  // label inside the UNITS toggle button
lv_timer_t* s_scanTimer = nullptr;
char s_ssids[kMaxNets][33];     // bare SSIDs backing the list rows
char s_ssidPrefill[33] = "";    // values shown on open — SAVE only applies
char s_gxPrefill[33] = "";      // fields that differ from these

constexpr char kUnitF[] = "UNITS  \xC2\xB0" "F";
constexpr char kUnitC[] = "UNITS  \xC2\xB0" "C";

void setStatus(const char* txt) { lv_label_set_text(s_status, txt); }

void showKeyboard(lv_obj_t* ta) {
  lv_keyboard_set_textarea(s_kb, ta);
  lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

void hideKeyboard() {
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
}

void listBtnCb(lv_event_t* e) {
  const int i = (int)(uintptr_t)lv_event_get_user_data(e);
  lv_textarea_set_text(s_ssidTa, s_ssids[i]);
  lv_textarea_set_text(s_passTa, "");
  setStatus("enter the password, then SAVE");
  showKeyboard(s_passTa);  // picked a network -> straight to password entry
}

void scanTimerCb(lv_timer_t*) {
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  lv_timer_pause(s_scanTimer);
  if (n <= 0) {
    Serial.printf("[setup] scan finished empty (%d)\n", n);
    setStatus(n == 0 ? "no networks found - tap SCAN to retry"
                     : "scan failed - tap SCAN to retry");
    WiFi.scanDelete();
    return;
  }

  // Strongest first, dedupe repeated SSIDs (mesh APs), skip hidden ones.
  int order[64];
  const int cnt = (n < 64) ? n : 64;
  for (int i = 0; i < cnt; i++) order[i] = i;
  for (int i = 1; i < cnt; i++) {
    const int v = order[i];
    int j = i;
    while (j > 0 && WiFi.RSSI(order[j - 1]) < WiFi.RSSI(v)) { order[j] = order[j - 1]; j--; }
    order[j] = v;
  }

  int shown = 0;
  for (int k = 0; k < cnt && shown < kMaxNets; k++) {
    const String ssid = WiFi.SSID(order[k]);
    if (ssid.length() == 0 || ssid.length() > 32) continue;
    bool dup = false;
    for (int j = 0; j < shown && !dup; j++) dup = (ssid == s_ssids[j]);
    if (dup) continue;
    snprintf(s_ssids[shown], sizeof(s_ssids[0]), "%s", ssid.c_str());

    char item[48];
    snprintf(item, sizeof item, "%s   %d", s_ssids[shown], (int)WiFi.RSSI(order[k]));
    lv_obj_t* btn = lv_list_add_btn(s_list, LV_SYMBOL_WIFI, item);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kTile), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kRing), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, lv_color_hex(kText), 0);
    lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(btn, listBtnCb, LV_EVENT_CLICKED, (void*)(uintptr_t)shown);
    shown++;
  }
  WiFi.scanDelete();
  Serial.printf("[setup] scan done: %d seen, %d listed\n", n, shown);
  char st[48];
  snprintf(st, sizeof st, "%d networks - tap one to choose", shown);
  setStatus(st);
}

void startScan() {
  lv_obj_clean(s_list);
  setStatus("scanning...");
  // esp_wifi_scan_start fails while a join attempt is in flight, and a bad
  // SSID (e.g. the placeholder) keeps the radio in a join-retry loop — abort
  // it before scanning. A live association is left alone; scanning while
  // connected works.
  if (WiFi.status() != WL_CONNECTED) WiFi.disconnect();
  WiFi.scanDelete();
  if (WiFi.scanNetworks(/*async=*/true) == WIFI_SCAN_FAILED) {
    Serial.println("[setup] scan start failed");
    setStatus("scan failed - tap SCAN to retry");
    return;
  }
  Serial.println("[setup] scan started");
  lv_timer_resume(s_scanTimer);
}

void scanBtnCb(lv_event_t*) {
  hideKeyboard();
  startScan();
}

void taCb(lv_event_t* e) { showKeyboard(lv_event_get_target(e)); }

void briCb(lv_event_t* e) {
  const int v = lv_slider_get_value(s_briSlider);
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    backlightSet(v);  // live preview while dragging
    lv_label_set_text_fmt(s_briLbl, "%d%%", v);
  } else {            // LV_EVENT_RELEASED
    backlightSave(v);
  }
}

void unitCb(lv_event_t*) {
  // Applies immediately (next poll re-converts), persists at once — like
  // the brightness slider, CANCEL does not undo it.
  const bool f = !gxTempsInF();
  gxSetTempsInF(f);
  lv_label_set_text(s_unitLbl, f ? kUnitF : kUnitC);
}

void kbCb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    hideKeyboard();
    setStatus("tap SAVE to join");
  } else if (code == LV_EVENT_CANCEL) {
    hideKeyboard();
  }
}

void saveCb(lv_event_t*) {
  const char* ssid = lv_textarea_get_text(s_ssidTa);
  const char* pass = lv_textarea_get_text(s_passTa);  // empty = open network
  const char* gx = lv_textarea_get_text(s_gxTa);

  // GX target: apply only when edited; a cleared field reverts to the
  // secrets.h defaults.
  if (strcmp(gx, s_gxPrefill) != 0) gxSetTarget(gx);

  // Wi-Fi: only when actually touched — a changed SSID (open networks may
  // have no password) or a typed password. The prefilled SSID alone doesn't
  // count, so saving other settings can't wipe a stored password with the
  // always-empty password field.
  const bool wifiTouched = (strcmp(ssid, s_ssidPrefill) != 0) || pass[0] != '\0';
  if (wifiTouched) {
    if (ssid[0] == '\0') {
      setStatus("pick a network or type an SSID first");
      return;
    }
    Preferences p;
    p.begin("darkside", false);
    p.putString("wifi.ssid", ssid);
    p.putString("wifi.pass", pass);
    p.end();
    Serial.printf("[setup] saved ssid=%s, joining\n", ssid);  // never log the password
    WiFi.disconnect();
    WiFi.begin(ssid, pass);
  }
  hideKeyboard();
  lv_scr_load(s_prevScr);  // main screen's link dot shows join progress
}

void cancelCb(lv_event_t*) {
  hideKeyboard();
  lv_timer_pause(s_scanTimer);  // an in-flight scan result is now moot
  Serial.println("[setup] cancelled, no changes");
  // startScan may have aborted a join to free the radio; if the user bails
  // without saving, restart the stored-credential join (NVS, else secrets.h;
  // placeholder secrets aren't worth rejoining).
  if (WiFi.status() != WL_CONNECTED) {
    String ssid = WIFI_SSID, pass = WIFI_PASS;
    bool fromNvs = false;
    Preferences p;
    if (p.begin("darkside", true)) {
      if (p.isKey("wifi.ssid")) {
        ssid = p.getString("wifi.ssid", ssid);
        pass = p.getString("wifi.pass", pass);
        fromNvs = true;
      }
      p.end();
    }
    if (fromNvs || ssid != "YOUR_TRUCK_SSID") WiFi.begin(ssid.c_str(), pass.c_str());
  }
  lv_scr_load(s_prevScr);
}

lv_obj_t* mkBtn(int x, int y, int w, int h, const char* txt, uint32_t bg,
                uint32_t fg, lv_event_cb_t cb) {
  return uiwButton(s_scr, x, y, w, h, txt, bg, fg, cb);
}

lv_obj_t* mkTa(int x, int y, int w, const char* placeholder, int maxLen) {
  lv_obj_t* ta = lv_textarea_create(s_scr);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_placeholder_text(ta, placeholder);
  lv_textarea_set_max_length(ta, maxLen);
  lv_obj_set_size(ta, w, 36);
  lv_obj_set_pos(ta, x, y);
  lv_obj_set_style_bg_color(ta, lv_color_hex(kTile), 0);
  lv_obj_set_style_text_color(ta, lv_color_hex(kText), 0);
  lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
  lv_obj_set_style_border_color(ta, lv_color_hex(kRing), 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_radius(ta, 6, 0);
  lv_obj_set_style_text_color(ta, lv_color_hex(kMuted), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_add_event_cb(ta, taCb, LV_EVENT_CLICKED, nullptr);
  return ta;
}

}  // namespace

void uiSetupBuild() {
  s_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(kBg), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  // Fixed layout — never let focus changes scroll the screen around.
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* hdr = lv_label_create(s_scr);
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hdr, lv_color_hex(kMuted), 0);
  lv_label_set_text(hdr, "SETUP");
  lv_obj_set_pos(hdr, 16, 10);

  // Cancel: kRing chip so it clearly reads as a button on the dark bg
  // (the original kTile "BACK" chip was invisible on the physical panel).
  mkBtn(352, 6, 112, 30, LV_SYMBOL_CLOSE "  CANCEL", kRing, kText, cancelCb);

  // Status line lives in the header so three settings rows fit below.
  s_status = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_status, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_status, lv_color_hex(kMuted), 0);
  lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_status, 236);
  lv_label_set_text(s_status, "");
  lv_obj_set_pos(s_status, 110, 12);

  s_ssidTa = mkTa(16, 40, 300, "SSID", 32);
  mkBtn(324, 40, 140, 36, LV_SYMBOL_REFRESH "  SCAN", kRing, kText, scanBtnCb);

  s_passTa = mkTa(16, 84, 300, "PASSWORD", 63);
  lv_textarea_set_password_mode(s_passTa, true);
  mkBtn(324, 84, 140, 36, "SAVE", kTeal, kBg, saveCb);

  // GX target (mDNS host or IP; clear the field to revert to secrets.h) and
  // the temperature unit toggle.
  s_gxTa = mkTa(16, 128, 300, "GX ADDRESS", 32);
  lv_obj_t* unitBtn = mkBtn(324, 128, 140, 36, kUnitF, kRing, kText, unitCb);
  s_unitLbl = lv_obj_get_child(unitBtn, 0);

  // Brightness row: live while dragging, persisted on release.
  lv_obj_t* briTitle = lv_label_create(s_scr);
  lv_obj_set_style_text_font(briTitle, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(briTitle, lv_color_hex(kMuted), 0);
  lv_label_set_text(briTitle, "BRIGHT");
  lv_obj_set_pos(briTitle, 16, 178);

  s_briSlider = lv_slider_create(s_scr);
  lv_slider_set_range(s_briSlider, kBacklightMinPct, 100);
  lv_slider_set_value(s_briSlider, backlightGet(), LV_ANIM_OFF);
  lv_obj_set_size(s_briSlider, 290, 14);
  lv_obj_set_pos(s_briSlider, 76, 176);
  lv_obj_set_ext_click_area(s_briSlider, 12);  // finger-sized touch target
  lv_obj_set_style_bg_color(s_briSlider, lv_color_hex(kRing), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_briSlider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_briSlider, lv_color_hex(kTeal), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_briSlider, lv_color_hex(kText), LV_PART_KNOB);
  lv_obj_set_style_pad_all(s_briSlider, 4, LV_PART_KNOB);
  lv_obj_add_event_cb(s_briSlider, briCb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_briSlider, briCb, LV_EVENT_RELEASED, nullptr);

  s_briLbl = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_briLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_briLbl, lv_color_hex(kText), 0);
  lv_label_set_text_fmt(s_briLbl, "%d%%", backlightGet());
  lv_obj_set_pos(s_briLbl, 384, 178);

  s_list = lv_list_create(s_scr);
  lv_obj_set_size(s_list, 448, 114);
  lv_obj_set_pos(s_list, 16, 198);
  lv_obj_set_style_bg_color(s_list, lv_color_hex(kTile), 0);
  lv_obj_set_style_border_width(s_list, 0, 0);
  lv_obj_set_style_radius(s_list, 8, 0);
  lv_obj_set_style_pad_row(s_list, 2, 0);

  // Created last so it stacks above the list when shown. Tall keys (152 px /
  // 4 rows) while still clearing the GX row — the slim 36 px input rows above
  // buy the height back. NOTE: lv_keyboard self-aligns BOTTOM_MID at create,
  // so set_pos() coordinates become offsets from that anchor (this once put
  // the keyboard 16 px past the screen bottom, with the screen silently
  // scrolling to chase it) — dock it with align, never set_pos.
  s_kb = lv_keyboard_create(s_scr);
  lv_obj_set_size(s_kb, 480, 152);
  lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(s_kb, lv_color_hex(kBg), 0);
  lv_obj_set_style_bg_color(s_kb, lv_color_hex(kTile), LV_PART_ITEMS);
  lv_obj_set_style_text_color(s_kb, lv_color_hex(kText), LV_PART_ITEMS);
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_kb, kbCb, LV_EVENT_ALL, nullptr);

  s_scanTimer = lv_timer_create(scanTimerCb, 300, nullptr);
  lv_timer_pause(s_scanTimer);
}

void uiSetupOpen() {
  s_prevScr = lv_scr_act();
  // Prefill every field with its current value; saveCb applies only what
  // differs from these prefills. Password always starts blank.
  String ssid;
  Preferences p;
  if (p.begin("darkside", true)) {
    ssid = p.getString("wifi.ssid", "");
    p.end();
  }
  snprintf(s_ssidPrefill, sizeof s_ssidPrefill, "%s", ssid.c_str());
  lv_textarea_set_text(s_ssidTa, s_ssidPrefill);
  lv_textarea_set_text(s_passTa, "");
  snprintf(s_gxPrefill, sizeof s_gxPrefill, "%s", gxGetTarget());
  lv_textarea_set_text(s_gxTa, s_gxPrefill);
  lv_label_set_text(s_unitLbl, gxTempsInF() ? kUnitF : kUnitC);
  lv_slider_set_value(s_briSlider, backlightGet(), LV_ANIM_OFF);
  lv_label_set_text_fmt(s_briLbl, "%d%%", backlightGet());
  hideKeyboard();
  lv_scr_load(s_scr);
  startScan();
}

void uiSetupShowKeyboard() {
  uiSetupOpen();
  showKeyboard(s_passTa);
}
