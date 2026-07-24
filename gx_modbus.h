#pragma once
#include <stdint.h>

#include "secrets.h"

// Temperature unit: 1 = °F, 0 = °C (the screen shows a bare ° either way;
// the telemetry line carries the letter). Older secrets.h files without the
// define keep the original Fahrenheit behavior.
#ifndef TEMPS_IN_F
#define TEMPS_IN_F 1
#endif

// Sensor rosters derive from the GX_*_UNITS lists in secrets.h — add or
// remove a unit there and polling, storage, and telemetry all resize to
// match (ui.cpp static_asserts the label lists against these counts).
constexpr uint8_t kGxTempUnits[] = GX_TEMP_UNITS;
constexpr uint8_t kGxTankUnits[] = GX_TANK_UNITS;

// Live values read from the Victron Ekrano GX over Modbus TCP (port 502,
// no auth — enable "Modbus TCP Server" under Settings -> Integrations).
// System aggregates live on unit 100 (com.victronenergy.system), verified
// against the running Darkside.Overland system 2026-07-24:
//   840 battery volts x10 · 841 battery amps x10 signed · 842 battery watts
//   843 SOC percent · 844 battery state (0 idle, 1 charging, 2 discharging)
//   850 PV power W · 817 AC consumption L1 W · 860 DC system W
// The Orion XS DC-DC charger is its own unit (com.victronenergy.alternator,
// GX_ALT_UNIT in secrets.h — found by unit-id sweep, 239 on this system):
//   4100 output volts x100 · 4101 output amps x10 signed (power = V*A; the
//   service has no power register). Absent/asleep alternator reads as 0.
struct GxData {
  bool valid = false;        // last poll round succeeded end-to-end
  uint32_t lastOkMs = 0;     // millis() of the last good poll
  float battV = 0;
  float battA = 0;
  int battW = 0;
  int soc = 0;
  int battState = 0;
  int pvW = 0;
  int acW = 0;
  int dcW = 0;
  int altW = 0;              // Orion XS output (0 when engine off/absent)
  int inW = 0;               // total charge input = pvW + altW

  // Temperature sensors via the GX (com.victronenergy.temperature — Ruuvis
  // here, but any temp service works; unit id = device instance on current
  // Venus; reg 3304 = degC x100). Order follows GX_TEMP_UNITS/GX_TEMP_LABELS.
  static constexpr int kNumTemps = sizeof(kGxTempUnits) / sizeof(kGxTempUnits[0]);
  float temp[kNumTemps] = {0};       // °F or °C per TEMPS_IN_F
  bool tempOk[kNumTemps] = {false};  // per-sensor, non-fatal (Ruuvis nap)

  // Tank levels (com.victronenergy.tank — Mopekas here, any tank service
  // works; reg 3004 = level% x10). A tank whose sensor died falls off D-Bus
  // entirely (unit stops answering) — its slot reads not-ok and displays
  // '--'. Order follows GX_TANK_UNITS.
  static constexpr int kNumTanks = sizeof(kGxTankUnits) / sizeof(kGxTankUnits[0]);
  float tankPct[kNumTanks] = {0};
  bool tankOk[kNumTanks] = {false};

  // GX system name (the VRM installation name): settings service reg 5700,
  // string[8] = 16 chars max (longer names arrive truncated). Read once per
  // connection, non-fatally; keeps its last value across reconnects.
  char sysName[17] = {0};
  bool sysNameOk = false;
};

// Runtime settings (setup screen; NVS-backed, secrets.h supplies the
// first-boot defaults). Safe to call from the UI task — target changes are
// handed to the poller task rather than touching its socket directly.
bool gxTempsInF();                   // current display unit
void gxSetTempsInF(bool fahrenheit); // apply now + persist
const char* gxGetTarget();           // current GX target (mDNS host or IP)
void gxSetTarget(const char* addr);  // "" reverts to the secrets.h defaults

// Spawn the poller task (core 0). Call once from setup(); the task waits
// for Wi-Fi on its own and polls at ~1 Hz. ALL Modbus/mDNS/socket blocking
// happens on that task, so a slow or dead GX link can never stall the LVGL
// loop. Poll semantics (per round, on a kept-alive connection): unit-100
// system reads are fatal — socket drops, 3 s backoff; sensor reads are
// per-device non-fatal — an absent device reads not-ok, a timeout/framing
// fault skips the remaining sensors and cycles the socket without backoff
// while the round still succeeds with the system values.
void gxStart();

// Copy the newest sample into `out`. Returns a sequence number that bumps
// once per successful poll round — equal values mean nothing new. If
// lastPollMs is given it receives the duration of the most recent poll
// attempt (successful or not).
uint32_t gxSnapshot(GxData& out, uint32_t* lastPollMs = nullptr);
