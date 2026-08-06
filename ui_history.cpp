#include "ui_history.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "gx_settings.h"
#include "history.h"
#include "ui_theme.h"
#include "ui_widgets.h"

// Layout (480x320): header row (series name + CLOSE), chart, a time row
// (window start .. now), and a MIN/MAX/NOW stats line. 1440 minute buckets
// draw as 288 five-minute points; gaps stay gaps (LV_CHART_POINT_NONE).
namespace {

constexpr int kPoints = 288;
constexpr int kBucket = kHistMinutes / kPoints;  // 5 minutes per drawn point

struct SeriesMeta {
  const char* unit;
  uint32_t color;
  float scale;     // float -> chart int
  int decimals;    // label formatting
  float minSpan;   // minimum y-span so flat lines don't zoom to noise
};
const SeriesMeta kMeta[kHistCount] = {
    {"V", kText, 100.0f, 2, 0.4f},
    {"A", kAmber, 10.0f, 1, 4.0f},
    {"W", kGreen, 1.0f, 0, 100.0f},
    {"W", kBlue, 1.0f, 0, 100.0f},
    {"%", kTeal, 1.0f, 0, 10.0f},
};

// Series names come from the label slots so on-device renames show here
// too (read at refill time, not baked in). SOC has no main-screen label.
const char* seriesName(int s) {
  switch (s) {
    case kHistV:   return gxLabel(kGxLblBatt);
    case kHistA:   return gxLabel(kGxLblCurrent);
    case kHistInW: return gxLabel(kGxLblPowerIn);
    case kHistAcW: return gxLabel(kGxLblAcLoads);
  }
  return "SOC";
}

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_prevScr = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_chart = nullptr;
lv_chart_series_t* s_ser = nullptr;
lv_obj_t* s_tStart = nullptr;
lv_obj_t* s_tEnd = nullptr;
lv_obj_t* s_stats = nullptr;
lv_obj_t* s_noData = nullptr;
lv_timer_t* s_refresh = nullptr;
int s_series = 0;
lv_coord_t s_pts[kPoints];

void fmtVal(char* out, size_t cap, float v, int series) {
  snprintf(out, cap, "%.*f", kMeta[series].decimals, v);
}

void refill() {
  static float mins[kHistMinutes];  // static: too big for the UI task stack
  const uint32_t nowMin = histGet(s_series, mins);
  const SeriesMeta& m = kMeta[s_series];

  float lo = NAN, hi = NAN, nowVal = NAN;
  for (int p = 0; p < kPoints; p++) {
    float sum = 0;
    int cnt = 0;
    for (int b = 0; b < kBucket; b++) {
      const float v = mins[p * kBucket + b];
      if (!isnan(v)) { sum += v; cnt++; }
    }
    if (cnt) {
      const float avg = sum / cnt;
      s_pts[p] = (lv_coord_t)lroundf(avg * m.scale);
      if (isnan(lo) || avg < lo) lo = avg;
      if (isnan(hi) || avg > hi) hi = avg;
    } else {
      s_pts[p] = LV_CHART_POINT_NONE;
    }
  }
  // Newest non-gap minute = NOW readout.
  for (int i = kHistMinutes - 1; i >= 0; i--)
    if (!isnan(mins[i])) { nowVal = mins[i]; break; }

  char buf[96];
  snprintf(buf, sizeof buf, "%s  #8c96aa 24H %s#", seriesName(s_series), m.unit);
  lv_label_set_text(s_title, buf);
  lv_obj_set_style_line_color(s_chart, lv_color_hex(m.color), LV_PART_ITEMS);

  if (isnan(lo)) {
    lv_obj_clear_flag(s_noData, LV_OBJ_FLAG_HIDDEN);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_label_set_text(s_stats, "");
    lv_label_set_text(s_tStart, "");
    lv_label_set_text(s_tEnd, "");
  } else {
    lv_obj_add_flag(s_noData, LV_OBJ_FLAG_HIDDEN);
    // Pad the range so the trace never kisses the frame; enforce a minimum
    // span so a flat overnight line doesn't become full-scale noise.
    float span = hi - lo;
    if (span < m.minSpan) {
      const float pad = (m.minSpan - span) / 2;
      lo -= pad;
      hi += pad;
      span = m.minSpan;
    }
    lo -= span * 0.08f;
    hi += span * 0.08f;
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y,
                       (lv_coord_t)floorf(lo * m.scale),
                       (lv_coord_t)ceilf(hi * m.scale));

    char a[16], b[16], c[16];
    fmtVal(a, sizeof a, lo + span * 0.08f, s_series);   // undo pad for display
    fmtVal(b, sizeof b, hi - span * 0.08f, s_series);
    if (!isnan(nowVal)) fmtVal(c, sizeof c, nowVal, s_series);
    else snprintf(c, sizeof c, "--");
    snprintf(buf, sizeof buf, "#8c96aa MIN# %s   #8c96aa MAX# %s   #8c96aa NOW# #e8f0fa %s#",
             a, b, c);
    lv_label_set_text(s_stats, buf);

    const time_t t0 = (time_t)(nowMin - (kHistMinutes - 1)) * 60;
    const time_t t1 = (time_t)nowMin * 60;
    struct tm tv;
    localtime_r(&t0, &tv);
    snprintf(buf, sizeof buf, "%02d:%02d", tv.tm_hour, tv.tm_min);
    lv_label_set_text(s_tStart, buf);
    localtime_r(&t1, &tv);
    snprintf(buf, sizeof buf, "%02d:%02d", tv.tm_hour, tv.tm_min);
    lv_label_set_text(s_tEnd, buf);
  }
  lv_chart_refresh(s_chart);
}

void refreshCb(lv_timer_t*) {
  if (lv_scr_act() == s_scr) refill();
}

void closeCb(lv_event_t*) {
  lv_timer_pause(s_refresh);
  lv_scr_load(s_prevScr);
}

}  // namespace

void uiHistBuild() {
  s_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(kBg), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

  s_title = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_title, lv_color_hex(kText), 0);
  lv_label_set_recolor(s_title, true);
  lv_label_set_text(s_title, "");
  lv_obj_set_pos(s_title, 16, 10);

  lv_obj_t* close = uiwButton(s_scr, 368, 6, 96, 30, LV_SYMBOL_CLOSE "  CLOSE",
                              kRing, kText, closeCb);
  (void)close;

  s_chart = lv_chart_create(s_scr);
  lv_obj_set_size(s_chart, 440, 204);
  lv_obj_set_pos(s_chart, 20, 48);
  lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(s_chart, kPoints);
  lv_chart_set_div_line_count(s_chart, 4, 6);
  lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_CIRCULAR);
  lv_obj_set_style_bg_color(s_chart, lv_color_hex(kTile), 0);
  lv_obj_set_style_border_width(s_chart, 0, 0);
  lv_obj_set_style_radius(s_chart, 8, 0);
  lv_obj_set_style_line_color(s_chart, lv_color_hex(kRing), LV_PART_MAIN);
  lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
  lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);  // no point dots
  lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);
  s_ser = lv_chart_add_series(s_chart, lv_color_hex(kTeal), LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_ext_y_array(s_chart, s_ser, s_pts);

  s_noData = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_noData, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_noData, lv_color_hex(kMuted), 0);
  lv_label_set_text(s_noData, "NO HISTORY YET — RECORDING");
  lv_obj_align(s_noData, LV_ALIGN_CENTER, 0, -20);
  lv_obj_add_flag(s_noData, LV_OBJ_FLAG_HIDDEN);

  s_tStart = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_tStart, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_tStart, lv_color_hex(kMuted), 0);
  lv_label_set_text(s_tStart, "");
  lv_obj_set_pos(s_tStart, 20, 258);

  s_tEnd = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_tEnd, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_tEnd, lv_color_hex(kMuted), 0);
  lv_obj_set_style_text_align(s_tEnd, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_width(s_tEnd, 60);
  lv_label_set_text(s_tEnd, "");
  lv_obj_set_pos(s_tEnd, 400, 258);

  s_stats = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_stats, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_stats, lv_color_hex(kMuted), 0);
  lv_label_set_recolor(s_stats, true);
  lv_label_set_text(s_stats, "");
  lv_obj_set_pos(s_stats, 20, 284);

  s_refresh = lv_timer_create(refreshCb, 60000, nullptr);
  lv_timer_pause(s_refresh);
}

void uiHistOpen(int series) {
  if (series < 0 || series >= kHistCount) return;
  s_series = series;
  s_prevScr = lv_scr_act();
  refill();
  lv_timer_resume(s_refresh);
  lv_scr_load(s_scr);
}
