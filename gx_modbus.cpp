#include "gx_modbus.h"

#include <WiFi.h>
#include <ESPmDNS.h>

#include "secrets.h"

namespace {

WiFiClient s_sock;
IPAddress s_gxIp;            // resolved once; re-resolved after failures
uint16_t s_tid = 1;
uint32_t s_nextAttemptMs = 0;  // backoff gate after a failure

bool resolveGx() {
  // .local names don't go through DNS — query mDNS, fall back to the pinned
  // address (the GX keeps 10.20.30.239 via DHCP reservation).
  IPAddress ip = MDNS.queryHost(GX_MDNS_HOST, 2000);
  if (ip == IPAddress()) {
    if (!s_gxIp.fromString(GX_FALLBACK_IP)) return false;
    Serial.printf("[gx] %s.local not resolved, using fallback %s\n",
                  GX_MDNS_HOST, GX_FALLBACK_IP);
  } else {
    s_gxIp = ip;
    Serial.printf("[gx] %s.local -> %s\n", GX_MDNS_HOST, s_gxIp.toString().c_str());
  }
  return true;
}

void drop() {
  s_sock.stop();
  s_nextAttemptMs = millis() + 3000;
}

// Read exactly n bytes with a deadline; false on timeout/close.
bool readExact(uint8_t* dst, int n, uint32_t deadline) {
  int got = 0;
  while (got < n) {
    if ((int32_t)(deadline - millis()) <= 0) return false;
    int r = s_sock.read(dst + got, n - got);
    if (r > 0) got += r;
    else delay(5);
  }
  return true;
}

// Modbus FC3 read of `count` holding registers into regs[]. Parses the MBAP
// length first so a Modbus EXCEPTION (e.g. a unit that went away) is consumed
// cleanly and reported as false without desyncing or stalling the socket —
// framing/timeout problems also return false (caller drops the connection).
bool readRegs(uint8_t unit, uint16_t addr, uint16_t count, uint16_t* regs) {
  uint8_t req[12];
  uint16_t tid = s_tid++;
  req[0] = tid >> 8; req[1] = tid & 0xFF;
  req[2] = 0; req[3] = 0;            // protocol id
  req[4] = 0; req[5] = 6;            // length
  req[6] = unit;
  req[7] = 3;                        // FC3 read holding registers
  req[8] = addr >> 8; req[9] = addr & 0xFF;
  req[10] = count >> 8; req[11] = count & 0xFF;
  if (s_sock.write(req, sizeof(req)) != sizeof(req)) return false;

  const uint32_t deadline = millis() + 500;
  uint8_t hdr[8];
  if (!readExact(hdr, sizeof(hdr), deadline)) return false;
  const int bodyLen = ((hdr[4] << 8) | hdr[5]) - 2;  // after unit id + fc
  uint8_t body[1 + 2 * 16];
  if (bodyLen < 1 || bodyLen > (int)sizeof(body)) return false;
  if (!readExact(body, bodyLen, deadline)) return false;
  if (hdr[0] != (tid >> 8) || hdr[1] != (tid & 0xFF)) return false;
  if (hdr[7] != 3) return false;                     // exception response
  if (body[0] != count * 2 || bodyLen != 1 + count * 2) return false;
  for (int i = 0; i < count; i++)
    regs[i] = ((uint16_t)body[1 + 2 * i] << 8) | body[2 + 2 * i];
  return true;
}

inline int s16(uint16_t v) { return (v > 32767) ? (int)v - 65536 : (int)v; }

}  // namespace

bool gxPoll(GxData& out) {
  if (WiFi.status() != WL_CONNECTED) { drop(); out.valid = false; return false; }
  if (!s_sock.connected()) {
    if ((int32_t)(millis() - s_nextAttemptMs) < 0) { out.valid = false; return false; }
    if (s_gxIp == IPAddress() && !resolveGx()) { drop(); out.valid = false; return false; }
    if (!s_sock.connect(s_gxIp, 502, 1500)) {
      Serial.printf("[gx] connect %s:502 failed\n", s_gxIp.toString().c_str());
      s_gxIp = IPAddress();  // re-resolve next time (GX may have moved)
      drop(); out.valid = false; return false;
    }
    s_sock.setNoDelay(true);
    Serial.printf("[gx] connected %s:502\n", s_gxIp.toString().c_str());
  }

  uint16_t batt[5], pv[1], ac[1], dc[1];
  if (!readRegs(100, 840, 5, batt) || !readRegs(100, 850, 1, pv) ||
      !readRegs(100, 817, 1, ac) || !readRegs(100, 860, 1, dc)) {
    Serial.println("[gx] read failed, reconnecting next poll");
    drop(); out.valid = false; return false;
  }

  // Orion XS output — NON-FATAL: engine-off/asleep or a re-numbered unit
  // just shows 0 W rather than failing the whole poll.
  uint16_t alt[2];
  if (readRegs(GX_ALT_UNIT, 4100, 2, alt)) {
    const float altV = alt[0] / 100.0f;
    const float altA = s16(alt[1]) / 10.0f;
    out.altW = (int)lroundf(altV * altA);
  } else {
    out.altW = 0;
  }

  // Ruuvi temperatures — each NON-FATAL (tags sleep, batteries die).
  static const uint8_t kTempUnits[GxData::kNumTemps] = GX_TEMP_UNITS;
  for (int i = 0; i < GxData::kNumTemps; i++) {
    uint16_t t[1];
    out.tempOk[i] = readRegs(kTempUnits[i], 3304, 1, t);
    if (out.tempOk[i]) out.tempF[i] = s16(t[0]) / 100.0f * 9.0f / 5.0f + 32.0f;
  }

  out.battV = batt[0] / 10.0f;
  out.battA = s16(batt[1]) / 10.0f;
  out.battW = s16(batt[2]);
  out.soc = batt[3];
  out.battState = batt[4];
  out.pvW = s16(pv[0]);
  out.acW = s16(ac[0]);
  out.dcW = s16(dc[0]);
  out.inW = out.pvW + out.altW;
  out.valid = true;
  out.lastOkMs = millis();
  return true;
}
