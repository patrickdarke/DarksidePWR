#pragma once
#include <stdint.h>

#include "secrets.h"
#include "config.h"

// Victron GX polling: owns the register map knowledge, the poll cadence,
// and the core-0 task all Modbus I/O runs on (the LVGL loop never blocks on
// the network). Transport framing lives in modbus_transport; runtime
// settings in gx_settings.

// Sensor rosters derive from the GX_*_UNITS lists — add or remove a unit in
// config.h/secrets.h and polling, storage, and telemetry all resize to
// match (ui.cpp static_asserts the label lists against these counts).
constexpr uint8_t kGxTempUnits[] = GX_TEMP_UNITS;
constexpr uint8_t kGxTankUnits[] = GX_TANK_UNITS;

// Live values read from the GX. System aggregates on unit 100 (fatal on
// failure); everything else is per-device non-fatal — an absent device
// reads not-ok, a timeout/framing fault skips the remaining reads and
// cycles the socket without backoff while the round still succeeds with
// the system values. Consumers must gate on `valid`: an invalid sample may
// carry stale Ok flags from the previous round.
struct GxData {
  bool valid = false;        // last poll round succeeded end-to-end
  uint32_t lastOkMs = 0;     // millis() of the last good poll
  float battV = 0;
  float battA = 0;
  int battW = 0;
  int soc = 0;
  int battState = 0;         // 0 idle, 1 charging, 2 discharging
  int pvW = 0;
  int acW = 0;
  int dcW = 0;
  int altW = 0;              // Orion XS output (0 when engine off/absent)
  int inW = 0;               // total charge input = pvW + altW

  // Temperature sensors (reg 3304 = degC x100), order per GX_TEMP_UNITS.
  static constexpr int kNumTemps = sizeof(kGxTempUnits) / sizeof(kGxTempUnits[0]);
  float temp[kNumTemps] = {0};       // °F or °C per the units setting
  bool tempOk[kNumTemps] = {false};

  // Tank levels (reg 3004 = level% x10), order per GX_TANK_UNITS. A dead
  // sender falls off D-Bus entirely and reads not-ok ('--' on screen).
  static constexpr int kNumTanks = sizeof(kGxTankUnits) / sizeof(kGxTankUnits[0]);
  float tankPct[kNumTanks] = {0};
  bool tankOk[kNumTanks] = {false};

  // GX system name (VRM installation name): settings reg 5700, string[8] =
  // 16 chars max. Read once per connection; keeps its value across
  // reconnects. Absent on Venus <= 3.75.
  char sysName[17] = {0};
  bool sysNameOk = false;

  // Controls state for the CONTROL page (read-back truth).
  int mpMode = 0;          // vebus /Mode: 1 chg-only, 2 inv-only, 3 on, 4 off
  bool mpModeOk = false;
  float shoreLimA = 0;     // vebus /Ac/ActiveIn/CurrentLimit
  bool shoreLimOk = false;
  bool relayClosed[2] = {false, false};  // GX relays (806/807)
  bool relayOk = false;
  int dvccLimA = 0;        // DVCC max charge current, -1 = no limit
  bool dvccOk = false;
  int altMode = 0;         // Orion XS /Mode: 1 on, 4 off (newer firmware)
  bool altModeOk = false;  // this round's read answered
  bool altModeSupported = true;  // false only after a clean exception —
                                 // i.e. this GX firmware lacks reg 4119
};

// Spawn the poller task (core 0). Call once from setup(); the task waits
// for Wi-Fi on its own and polls at ~1 Hz.
void gxStart();

// Copy the newest sample into `out`. Returns a sequence number that bumps
// once per successful poll round — equal values mean nothing new. If
// lastPollMs is given it receives the duration of the most recent attempt.
uint32_t gxSnapshot(GxData& out, uint32_t* lastPollMs = nullptr);

// Queue a Modbus FC6 write for the poller task (false if the queue is
// full). Commands expire after 10 s so a tap made during an outage cannot
// fire minutes later; the task logs "[ctl] write ..." and re-polls
// immediately so the next snapshot carries read-back state.
bool gxWrite(uint8_t unit, uint16_t reg, uint16_t value);

// Ask the poller to sweep units 200-247 for a vebus /Mode answer and probe
// the control registers, printing results to serial (serial cmd 'V').
void gxRequestSweep();
