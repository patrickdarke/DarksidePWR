#pragma once
#include <lvgl.h>

#include "ui_theme.h"

// Shared chip/button factory for every screen (power, setup, control):
// flat rounded chip, montserrat 14 centered label, pressed state dips to
// the screen background. Callers restyle after creation for one-offs
// (radius, ring background, etc.).
inline lv_obj_t* uiwButton(lv_obj_t* parent, int x, int y, int w, int h,
                           const char* txt, uint32_t bg, uint32_t fg,
                           lv_event_cb_t cb, void* user = nullptr) {
  lv_obj_t* b = lv_btn_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(kBg), LV_STATE_PRESSED);
  lv_obj_set_style_radius(b, 6, 0);
  lv_obj_set_size(b, w, h);
  lv_obj_set_pos(b, x, y);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  lv_obj_t* l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  return b;
}
