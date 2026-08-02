#pragma once

// Scheduled display blanking for a wall/bedroom-mounted install.
// BOARD_CROWPANEL_50 only — a no-op stub on the 3.5" board so callers
// never need #ifdef. NTP-timed: there's no onboard RTC driver (the 5"
// board's RTC chip/register map was never identified from vendor
// material, and this project doesn't guess unverified hardware facts —
// see CLAUDE.md). Fails safe: the schedule does nothing until the clock
// has synced at least once, and does nothing at all until enabled.

void nightModeInit();  // call once from setup(), alongside Wi-Fi bring-up

// Call once per touch-read tick. `touchedNow` = a fresh touch-down this
// cycle (edge, not held) — arms/extends the 30s wake window. Returns true
// if the screen is currently blanked, in which case the caller must NOT
// forward this touch to LVGL (it's consumed purely as a wake signal).
bool nightModeTick(bool touchedNow);

// Setup-screen support. sleepMin/wakeMin are minutes-since-midnight
// (0-1439), local time.
void nightModeGetSchedule(bool& enabled, int& sleepMin, int& wakeMin);
void nightModeSetSchedule(bool enabled, int sleepMin, int wakeMin);  // applies + persists
