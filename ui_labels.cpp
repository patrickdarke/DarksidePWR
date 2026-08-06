#include "ui_labels.h"

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "gx_settings.h"
#include "ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"

// Layout (480x320): header row (LABELS + CANCEL/SAVE), then a scrollable
// column of caption+field rows sharing the bottom region with the keyboard
// (the column shrinks while the keyboard is up and the tapped field is
// scrolled into view). SAVE applies only fields that differ from their
// open-time prefill; clearing a field reverts that label to its
// config.h/secrets.h default (shown as the placeholder).
namespace {

constexpr int kRows = kGxLblFixedCount + GxData::kNumTemps + GxData::kNumTanks;
constexpr int kRowH = 44;
constexpr int kListY = 40;
constexpr int kListHFull = 272;  // to the screen bottom
constexpr int kListHKb = 128;    // keyboard (y 168..320) showing

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_prevScr = nullptr;
lv_obj_t* s_cont = nullptr;
lv_obj_t* s_kb = nullptr;
lv_obj_t* s_ta[kRows] = {nullptr};
char s_prefill[kRows][24];

void showKeyboard(lv_obj_t* ta) {
  lv_keyboard_set_textarea(s_kb, ta);
  lv_obj_set_height(s_cont, kListHKb);
  lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

void hideKeyboard() {
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_height(s_cont, kListHFull);
}

void taCb(lv_event_t* e) { showKeyboard(lv_event_get_target(e)); }

void kbCb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) hideKeyboard();
}

void saveCb(lv_event_t*) {
  hideKeyboard();
  bool changed = false;
  for (int i = 0; i < kRows; i++) {
    const char* txt = lv_textarea_get_text(s_ta[i]);
    if (strcmp(txt, s_prefill[i]) != 0) {
      gxSetLabel(i, txt);
      changed = true;
    }
  }
  if (changed) uiLabelsApply();
  lv_scr_load(s_prevScr);
}

void cancelCb(lv_event_t*) {
  hideKeyboard();
  Serial.println("[setup] labels cancelled, no changes");
  lv_scr_load(s_prevScr);
}

}  // namespace

void uiLabelsBuild() {
  s_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(kBg), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  // Only the field column scrolls; the screen itself stays put.
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* hdr = lv_label_create(s_scr);
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hdr, lv_color_hex(kMuted), 0);
  lv_label_set_text(hdr, "LABELS");
  lv_obj_set_pos(hdr, 16, 10);

  lv_obj_t* hint = lv_label_create(s_scr);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(kMuted), 0);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
  lv_obj_set_width(hint, 108);
  lv_label_set_text(hint, "empty = default");
  lv_obj_set_pos(hint, 112, 12);

  uiwButton(s_scr, 232, 6, 108, 30, LV_SYMBOL_CLOSE "  CANCEL", kRing, kText, cancelCb);
  uiwButton(s_scr, 348, 6, 116, 30, "SAVE", kTeal, kBg, saveCb);

  s_cont = lv_obj_create(s_scr);
  lv_obj_remove_style_all(s_cont);
  lv_obj_set_pos(s_cont, 16, kListY);
  lv_obj_set_size(s_cont, 448, kListHFull);
  lv_obj_set_scroll_dir(s_cont, LV_DIR_VER);

  for (int i = 0; i < kRows; i++) {
    const int y = i * kRowH;

    char cap[16];
    gxLabelCaption(i, cap, sizeof cap);
    lv_obj_t* lbl = lv_label_create(s_cont);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kMuted), 0);
    lv_label_set_text(lbl, cap);
    lv_obj_set_pos(lbl, 0, y + 11);

    lv_obj_t* ta = lv_textarea_create(s_cont);
    lv_textarea_set_one_line(ta, true);
    const char* def = gxLabelDefault(i);
    lv_textarea_set_placeholder_text(ta, def[0] ? def : "AUTO (GX NAME)");
    lv_textarea_set_max_length(ta, i == kGxLblTitle ? 20 : 12);
    lv_obj_set_size(ta, 320, 36);
    lv_obj_set_pos(ta, 128, y);
    lv_obj_set_style_bg_color(ta, lv_color_hex(kTile), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kText), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(kRing), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kMuted), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(ta, taCb, LV_EVENT_CLICKED, nullptr);
    s_ta[i] = ta;
  }

  // Created last so it stacks above the field column when shown; docked
  // with align, never set_pos (lv_keyboard self-aligns BOTTOM_MID).
  s_kb = lv_keyboard_create(s_scr);
  lv_obj_set_size(s_kb, 480, 152);
  lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(s_kb, lv_color_hex(kBg), 0);
  lv_obj_set_style_bg_color(s_kb, lv_color_hex(kTile), LV_PART_ITEMS);
  lv_obj_set_style_text_color(s_kb, lv_color_hex(kText), LV_PART_ITEMS);
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_kb, kbCb, LV_EVENT_ALL, nullptr);
}

void uiLabelsOpen() {
  s_prevScr = lv_scr_act();
  for (int i = 0; i < kRows; i++) {
    snprintf(s_prefill[i], sizeof s_prefill[i], "%s", gxLabel(i));
    lv_textarea_set_text(s_ta[i], s_prefill[i]);
  }
  hideKeyboard();
  lv_obj_scroll_to_y(s_cont, 0, LV_ANIM_OFF);
  lv_scr_load(s_scr);
}
