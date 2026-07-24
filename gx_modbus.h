#pragma once
#include <stdint.h>

// Live values read from the Victron Ekrano GX over Modbus TCP (port 502,
// no auth — enable "Modbus TCP Server" under Settings -> Integrations).
// All registers live on unit 100 (com.victronenergy.system) and were
// verified against the running Darkside.Overland system 2026-07-24:
//   840 battery volts x10 · 841 battery amps x10 signed · 842 battery watts
//   843 SOC percent · 844 battery state (0 idle, 1 charging, 2 discharging)
//   850 PV power W · 817 AC consumption L1 W · 860 DC system W
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
};

// One polling round (4 short reads on a kept-alive connection). Returns true
// and updates `out` on success; on any failure closes the socket so the next
// call reconnects (venus.local via mDNS first, then the static fallback).
bool gxPoll(GxData& out);
