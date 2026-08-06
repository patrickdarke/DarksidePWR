#include "ui_sounds.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "history.h"  // histSdReady — for the "no SD card" hint
#include "sound.h"
#include "ui_theme.h"
#include "ui_widgets.h"

// Layout (480x320): header row (SOUNDS + status + BACK), BOOT/CHARGE slot
// chips, then a list of choices. Selections apply and persist immediately
// (brightness/units precedent) and the tapped sound plays as a preview, so
// picking is done by ear — there is no SAVE/CANCEL pair here.
namespace {

constexpr int kMaxFiles = 20;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_prevScr = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_list = nullptr;
lv_obj_t* s_volSlider = nullptr;
lv_obj_t* s_volLbl = nullptr;
lv_obj_t* s_slotBtn[2] = {nullptr};
int s_slot = kSndCharge;  // charge is what people come here to change
char s_files[kMaxFiles][32];
int s_nFiles = 0;

void rebuildList();  // rowCb and rebuildList reference each other

// Choice string and label for the built-in rows; SD files follow them.
const char* kBuiltinChoice[] = {"!off", "!chirp", "!voice"};
const char* kBuiltinLabel[] = {"SILENT", "CHIRP", "VOICE (BUILT-IN)"};

int builtinCount() { return soundHasVoice() ? 3 : 2; }

void styleSlotBtns() {
  for (int i = 0; i < 2; i++) {
    const bool on = (s_slot == i);
    lv_obj_set_style_bg_color(s_slotBtn[i], lv_color_hex(on ? kTeal : kTile), 0);
    lv_obj_t* lbl = lv_obj_get_child(s_slotBtn[i], 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(on ? kBg : kMuted), 0);
  }
}

void setStatus() {
  const char* cur = soundGet(s_slot);
  if (strcmp(cur, "!off") == 0) cur = "silent";
  else if (strcmp(cur, "!chirp") == 0) cur = "chirp";
  else if (strcmp(cur, "!voice") == 0) cur = "built-in voice";
  char buf[64];
  snprintf(buf, sizeof buf, "%s plays: %s",
           s_slot == kSndBoot ? "boot" : "charge", cur);
  lv_label_set_text(s_status, buf);
}

void rowCb(lv_event_t* e) {
  const int idx = (int)(uintptr_t)lv_event_get_user_data(e);
  const char* choice = (idx < builtinCount())
                           ? kBuiltinChoice[idx]
                           : s_files[idx - builtinCount()];
  soundSet(s_slot, choice);
  soundPreview(choice);  // pick by ear (no-op while something is playing)
  setStatus();
  rebuildList();  // move the check mark
}

void rebuildList() {
  lv_obj_clean(s_list);
  const char* cur = soundGet(s_slot);
  const int nRows = builtinCount() + s_nFiles;
  for (int i = 0; i < nRows; i++) {
    const bool builtin = i < builtinCount();
    const char* choice = builtin ? kBuiltinChoice[i] : s_files[i - builtinCount()];
    const char* label = builtin ? kBuiltinLabel[i] : s_files[i - builtinCount()];
    const bool sel = strcmp(choice, cur) == 0;
    lv_obj_t* btn = lv_list_add_btn(s_list, sel ? LV_SYMBOL_OK : LV_SYMBOL_AUDIO, label);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kTile), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kRing), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, lv_color_hex(sel ? kText : kMuted), 0);
    lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(btn, rowCb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
  }
  // Trailing hint: where new files come from (upload URL once Wi-Fi is up).
  char hint[72];
  if (!histSdReady()) {
    snprintf(hint, sizeof hint, "no SD card - built-ins only");
  } else if (WiFi.status() == WL_CONNECTED) {
    snprintf(hint, sizeof hint, "add MP3s: http://%s/  (%s.local)",
             WiFi.localIP().toString().c_str(), MDNS_HOST);
  } else {
    snprintf(hint, sizeof hint, "drop .mp3 files in /sounds on the SD card");
  }
  lv_obj_t* hintLbl = lv_list_add_text(s_list, hint);
  lv_obj_set_style_text_color(hintLbl, lv_color_hex(kMuted), 0);
  lv_obj_set_style_text_font(hintLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_bg_opa(hintLbl, LV_OPA_TRANSP, 0);
}

void slotCb(lv_event_t* e) {
  s_slot = (int)(uintptr_t)lv_event_get_user_data(e);
  styleSlotBtns();
  setStatus();
  rebuildList();
}

// Play the active slot's sound through the REAL event entry point —
// fallback chain included — so the test is exactly what the event does.
void testCb(lv_event_t*) {
  if (s_slot == kSndBoot) soundPlayBoot();
  else soundPlayCharge();
}

void volCb(lv_event_t* e) {
  const int v = lv_slider_get_value(s_volSlider);
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    soundSetVol(v);  // live — a playing clip follows the drag
    lv_label_set_text_fmt(s_volLbl, "%d%%", v);
  } else {  // LV_EVENT_RELEASED
    soundSaveVol(v);
  }
}

void backCb(lv_event_t*) { lv_scr_load(s_prevScr); }

}  // namespace

void uiSoundsBuild() {
  s_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(kBg), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* hdr = lv_label_create(s_scr);
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hdr, lv_color_hex(kMuted), 0);
  lv_label_set_text(hdr, "SOUNDS");
  lv_obj_set_pos(hdr, 16, 10);

  s_status = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_status, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_status, lv_color_hex(kMuted), 0);
  lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_status, 230);
  lv_label_set_text(s_status, "");
  lv_obj_set_pos(s_status, 110, 12);

  uiwButton(s_scr, 352, 6, 112, 30, LV_SYMBOL_LEFT "  BACK", kRing, kText, backCb);

  // Slot chips — which event the list below assigns to — and a TEST
  // button that plays the active slot's current sound.
  s_slotBtn[kSndBoot] = uiwButton(s_scr, 16, 44, 140, 36, "BOOT", kTile, kMuted,
                                  slotCb, (void*)(uintptr_t)kSndBoot);
  s_slotBtn[kSndCharge] = uiwButton(s_scr, 164, 44, 140, 36, "CHARGE", kTile,
                                    kMuted, slotCb, (void*)(uintptr_t)kSndCharge);
  uiwButton(s_scr, 352, 44, 112, 36, LV_SYMBOL_PLAY "  TEST", kTeal, kBg, testCb);

  s_list = lv_list_create(s_scr);
  lv_obj_set_size(s_list, 448, 188);  // volume row lives below
  lv_obj_set_pos(s_list, 16, 88);
  lv_obj_set_style_bg_color(s_list, lv_color_hex(kTile), 0);
  lv_obj_set_style_border_width(s_list, 0, 0);
  lv_obj_set_style_radius(s_list, 8, 0);
  lv_obj_set_style_pad_row(s_list, 2, 0);

  // Volume row: live while dragging (even mid-clip), saved on release —
  // the brightness slider's pattern. TEST sits right above for tuning.
  lv_obj_t* volTitle = lv_label_create(s_scr);
  lv_obj_set_style_text_font(volTitle, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(volTitle, lv_color_hex(kMuted), 0);
  lv_label_set_text(volTitle, "VOL");
  lv_obj_set_pos(volTitle, 16, 292);

  s_volSlider = lv_slider_create(s_scr);
  lv_slider_set_range(s_volSlider, 5, 100);
  lv_slider_set_value(s_volSlider, soundGetVol(), LV_ANIM_OFF);
  lv_obj_set_size(s_volSlider, 296, 14);
  lv_obj_set_pos(s_volSlider, 64, 290);
  lv_obj_set_ext_click_area(s_volSlider, 12);  // finger-sized touch target
  lv_obj_set_style_bg_color(s_volSlider, lv_color_hex(kRing), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_volSlider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_volSlider, lv_color_hex(kTeal), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_volSlider, lv_color_hex(kText), LV_PART_KNOB);
  lv_obj_set_style_pad_all(s_volSlider, 4, LV_PART_KNOB);
  lv_obj_add_event_cb(s_volSlider, volCb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_volSlider, volCb, LV_EVENT_RELEASED, nullptr);

  s_volLbl = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_volLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_volLbl, lv_color_hex(kText), 0);
  lv_label_set_text_fmt(s_volLbl, "%d%%", soundGetVol());
  lv_obj_set_pos(s_volLbl, 374, 292);
}

void uiSoundsOpen() {
  s_prevScr = lv_scr_act();
  const int n = soundListFiles(s_files, kMaxFiles);
  if (n >= 0) s_nFiles = n;  // -1 = player holds the card; keep the old list
  styleSlotBtns();
  setStatus();
  rebuildList();
  lv_slider_set_value(s_volSlider, soundGetVol(), LV_ANIM_OFF);
  lv_label_set_text_fmt(s_volLbl, "%d%%", soundGetVol());
  lv_scr_load(s_scr);
}
