#include "night_mode.h"

#if defined(BOARD_CROWPANEL_50)
#include <Arduino.h>
#include <Preferences.h>
#include <time.h>

#include "backlight.h"

// Wall-mounted-in-a-bedroom feature: blank the backlight on a schedule,
// wake on touch for 30s, re-blank — but only while still inside the sleep
// window (if the scheduled wake time arrives mid-interaction, just stay
// on). See CLAUDE.md for why this is NTP-timed rather than using the
// onboard RTC.
namespace {

constexpr uint32_t kWakeMs = 30000;
// Sanity floor for "has the clock synced yet" — an epoch before this is
// still the ESP32's post-boot default, not a real NTP-derived time.
// (~2023-11-14, comfortably before this project existed; any real sync
// will land far past it.)
constexpr time_t kMinValidEpoch = 1700000000;

bool s_enabled = false;
int s_sleepMin = 22 * 60;  // default 22:00 — inert until enabled from setup
int s_wakeMin = 7 * 60;    // default 07:00
bool s_blanked = false;
uint32_t s_wakeUntilMs = 0;

int clampMin(int m) {
  m %= 1440;
  if (m < 0) m += 1440;
  return m;
}

bool inSleepWindow(int nowMin) {
  if (s_sleepMin == s_wakeMin) return false;  // degenerate config: never sleep
  if (s_sleepMin < s_wakeMin) return nowMin >= s_sleepMin && nowMin < s_wakeMin;
  return nowMin >= s_sleepMin || nowMin < s_wakeMin;  // wraps midnight
}

void ensureAwake() {
  if (s_blanked) {
    backlightWake();
    s_blanked = false;
  }
  s_wakeUntilMs = 0;
}

void loadPrefs() {
  Preferences p;
  if (p.begin("darkside", /*readOnly=*/true)) {
    if (p.isKey("night.en")) s_enabled = p.getUChar("night.en", 0) != 0;
    if (p.isKey("night.sm")) s_sleepMin = clampMin(p.getInt("night.sm", s_sleepMin));
    if (p.isKey("night.wm")) s_wakeMin = clampMin(p.getInt("night.wm", s_wakeMin));
    p.end();
  }
}

}  // namespace

void nightModeInit() {
  loadPrefs();
  // Non-blocking: the SNTP client resolves/syncs in the background and
  // keeps retrying on its own once Wi-Fi comes up.
  configTzTime("MST7", "pool.ntp.org", "time.nist.gov");
}

bool nightModeTick(bool touchedNow) {
  if (!s_enabled) {
    ensureAwake();
    return false;
  }

  const time_t t = time(nullptr);
  if (t < kMinValidEpoch) {
    ensureAwake();  // clock not synced yet — fail safe to always-on
    return false;
  }

  struct tm lt;
  localtime_r(&t, &lt);
  const int nowMin = lt.tm_hour * 60 + lt.tm_min;

  if (!inSleepWindow(nowMin)) {
    ensureAwake();
    return false;
  }

  if (touchedNow) s_wakeUntilMs = millis() + kWakeMs;

  const bool awakeWindow = s_wakeUntilMs != 0 && (int32_t)(millis() - s_wakeUntilMs) < 0;
  if (awakeWindow && s_blanked) {
    backlightWake();
    s_blanked = false;
  } else if (!awakeWindow && !s_blanked) {
    backlightBlank();
    s_blanked = true;
  }
  return s_blanked;
}

void nightModeGetSchedule(bool& enabled, int& sleepMin, int& wakeMin) {
  enabled = s_enabled;
  sleepMin = s_sleepMin;
  wakeMin = s_wakeMin;
}

void nightModeSetSchedule(bool enabled, int sleepMin, int wakeMin) {
  s_enabled = enabled;
  s_sleepMin = clampMin(sleepMin);
  s_wakeMin = clampMin(wakeMin);
  Preferences p;
  p.begin("darkside", false);
  p.putUChar("night.en", enabled ? 1 : 0);
  p.putInt("night.sm", s_sleepMin);
  p.putInt("night.wm", s_wakeMin);
  p.end();
  if (!enabled) ensureAwake();
  Serial.printf("[setup] night mode %s, sleep %02d:%02d wake %02d:%02d\n",
                enabled ? "on" : "off", s_sleepMin / 60, s_sleepMin % 60,
                s_wakeMin / 60, s_wakeMin % 60);
}

#else

void nightModeInit() {}
bool nightModeTick(bool) { return false; }
void nightModeGetSchedule(bool& enabled, int& sleepMin, int& wakeMin) {
  enabled = false;
  sleepMin = 0;
  wakeMin = 0;
}
void nightModeSetSchedule(bool, int, int) {}

#endif
