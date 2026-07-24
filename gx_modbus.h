#pragma once
#include <stdint.h>

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

  // Ruuvi temperature sensors via the GX (com.victronenergy.temperature,
  // unit id = device instance on current Venus; reg 3304 = degC x100).
  // Order follows GX_TEMP_UNITS/GX_TEMP_LABELS in secrets.h.
  static constexpr int kNumTemps = 3;
  float tempF[kNumTemps] = {0};
  bool tempOk[kNumTemps] = {false};  // per-sensor, non-fatal (Ruuvis nap)

  // Mopeka LPG tanks (com.victronenergy.tank, reg 3004 = level% x10). A tank
  // whose sensor died falls off D-Bus entirely (unit stops answering) — its
  // slot reads not-ok and displays '--'. Order follows GX_TANK_UNITS.
  static constexpr int kNumTanks = 3;
  float tankPct[kNumTanks] = {0};
  bool tankOk[kNumTanks] = {false};
};

// One polling round (4 short reads on a kept-alive connection). Returns true
// and updates `out` on success; on any failure closes the socket so the next
// call reconnects (venus.local via mDNS first, then the static fallback).
bool gxPoll(GxData& out);
