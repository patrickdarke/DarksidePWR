#include "gx_modbus.h"

#include <Preferences.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "secrets.h"

// All socket/mDNS/Modbus state below is owned by the poller task once
// gxStart() runs. The UI task touches it only through gxSnapshot()/the
// setters, which go through s_mux or task-posted flags.
namespace {

WiFiClient s_sock;
IPAddress s_gxIp;            // resolved once; re-resolved after failures
uint16_t s_tid = 1;
uint32_t s_nextAttemptMs = 0;  // backoff gate after a failure
bool s_haveSysName = false;    // system name fetched on this connection
uint8_t s_lastExc = 0;         // code from the most recent Modbus exception

// Runtime settings; secrets.h is the default. s_host is guarded by s_mux
// (String assignment isn't atomic); the temps flag is a plain aligned bool.
String s_host;                  // GX target: mDNS host or literal IP
bool s_hostOverridden = false;  // true when the target came from NVS
volatile bool s_tempsF = (TEMPS_IN_F != 0);
bool s_prefsLoaded = false;

// Poller task <-> UI handoff.
SemaphoreHandle_t s_mux = nullptr;
GxData s_shared;                  // newest sample (copied under s_mux)
uint32_t s_seq = 0;               // bumps once per successful round
uint32_t s_pollDurMs = 0;         // duration of the latest attempt
char s_pendingTarget[40] = "";    // target change posted by the UI
volatile bool s_targetDirty = false;
char s_targetShown[40] = "";      // stable buffer returned by gxGetTarget

// Control writes queued by the UI, executed by the poller task. Commands
// expire: a tap made during an outage must not fire minutes later when the
// GX reappears (imagine a stale MULTI OFF executing after you walked away).
struct WriteCmd {
  uint8_t unit;
  uint16_t reg;
  uint16_t val;
  uint32_t queuedMs;
};
constexpr uint32_t kWriteTtlMs = 10000;
QueueHandle_t s_writeQ = nullptr;
volatile bool s_sweepReq = false;
bool s_altModeSupported = true;   // probe once per connection (reg 4119)

void lock() { if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY); }
void unlock() { if (s_mux) xSemaphoreGive(s_mux); }

void loadPrefs() {
  if (s_prefsLoaded) return;
  s_prefsLoaded = true;
  s_host = GX_MDNS_HOST;
  Preferences p;
  if (p.begin("darkside", /*readOnly=*/true)) {
    if (p.isKey("gx.addr")) {
      s_host = p.getString("gx.addr", s_host);
      s_hostOverridden = true;
    }
    if (p.isKey("tempF")) s_tempsF = p.getUChar("tempF", s_tempsF ? 1 : 0) != 0;
    p.end();
  }
}

bool resolveGx() {
  // A literal IP target needs no lookup. Otherwise treat the target as an
  // mDNS host (".local" optional — it doesn't go through DNS), and fall back
  // to the pinned address only for the stock secrets.h target; a typed
  // custom host that fails to resolve should fail visibly, not silently
  // poll some unrelated IP.
  lock();
  const String target = s_host;
  const bool overridden = s_hostOverridden;
  unlock();
  if (s_gxIp.fromString(target.c_str())) return true;
  String host = target;
  if (host.endsWith(".local")) host.remove(host.length() - 6);
  IPAddress ip = MDNS.queryHost(host.c_str(), 2000);
  if (ip != IPAddress()) {
    s_gxIp = ip;
    Serial.printf("[gx] %s.local -> %s\n", host.c_str(), s_gxIp.toString().c_str());
    return true;
  }
  if (!overridden && s_gxIp.fromString(GX_FALLBACK_IP)) {
    Serial.printf("[gx] %s.local not resolved, using fallback %s\n",
                  host.c_str(), GX_FALLBACK_IP);
    return true;
  }
  Serial.printf("[gx] %s not resolved\n", target.c_str());
  return false;
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

// Result of one register read. kRegNoAnswer = the unit replied with a Modbus
// exception (absent/napping device) — the frame was consumed cleanly and the
// socket stays usable. kRegFault = write/timeout/framing/TID trouble — reply
// bytes may still be in flight, so the caller must close the connection
// before issuing another read.
enum RegResult { kRegOk, kRegNoAnswer, kRegFault };

// Modbus FC3 read of `count` holding registers into regs[]. Parses the MBAP
// length before the body so an exception response is consumed without
// desyncing or stalling the socket.
RegResult readRegs(uint8_t unit, uint16_t addr, uint16_t count, uint16_t* regs) {
  uint8_t req[12];
  uint16_t tid = s_tid++;
  req[0] = tid >> 8; req[1] = tid & 0xFF;
  req[2] = 0; req[3] = 0;            // protocol id
  req[4] = 0; req[5] = 6;            // length
  req[6] = unit;
  req[7] = 3;                        // FC3 read holding registers
  req[8] = addr >> 8; req[9] = addr & 0xFF;
  req[10] = count >> 8; req[11] = count & 0xFF;
  if (s_sock.write(req, sizeof(req)) != sizeof(req)) return kRegFault;

  const uint32_t deadline = millis() + 500;
  uint8_t hdr[8];
  if (!readExact(hdr, sizeof(hdr), deadline)) return kRegFault;
  const int bodyLen = ((hdr[4] << 8) | hdr[5]) - 2;  // after unit id + fc
  uint8_t body[1 + 2 * 16];
  if (bodyLen < 1 || bodyLen > (int)sizeof(body)) return kRegFault;
  if (!readExact(body, bodyLen, deadline)) return kRegFault;
  if (hdr[0] != (tid >> 8) || hdr[1] != (tid & 0xFF)) return kRegFault;
  if (hdr[7] != 3) {                                 // exception response
    s_lastExc = body[0];
    return kRegNoAnswer;
  }
  if (body[0] != count * 2 || bodyLen != 1 + count * 2) return kRegFault;
  for (int i = 0; i < count; i++)
    regs[i] = ((uint16_t)body[1 + 2 * i] << 8) | body[2 + 2 * i];
  return kRegOk;
}

inline int s16(uint16_t v) { return (v > 32767) ? (int)v - 65536 : (int)v; }

// Modbus FC6 single-register write, same framing/tri-state rules as
// readRegs (a good response echoes addr+value; exceptions are consumed
// cleanly). Poller task only.
RegResult writeReg(uint8_t unit, uint16_t reg, uint16_t val) {
  uint8_t req[12];
  const uint16_t tid = s_tid++;
  req[0] = tid >> 8; req[1] = tid & 0xFF;
  req[2] = 0; req[3] = 0;
  req[4] = 0; req[5] = 6;
  req[6] = unit;
  req[7] = 6;                        // FC6 write single register
  req[8] = reg >> 8; req[9] = reg & 0xFF;
  req[10] = val >> 8; req[11] = val & 0xFF;
  if (s_sock.write(req, sizeof(req)) != sizeof(req)) return kRegFault;

  const uint32_t deadline = millis() + 500;
  uint8_t hdr[8];
  if (!readExact(hdr, sizeof(hdr), deadline)) return kRegFault;
  const int bodyLen = ((hdr[4] << 8) | hdr[5]) - 2;
  uint8_t body[8];
  if (bodyLen < 1 || bodyLen > (int)sizeof(body)) return kRegFault;
  if (!readExact(body, bodyLen, deadline)) return kRegFault;
  if (hdr[0] != (tid >> 8) || hdr[1] != (tid & 0xFF)) return kRegFault;
  if (hdr[7] != 6) {                 // exception response
    s_lastExc = body[0];
    return kRegNoAnswer;
  }
  if (bodyLen != 4) return kRegFault;
  return kRegOk;
}

}  // namespace

bool gxTempsInF() {
  loadPrefs();
  return s_tempsF;
}

void gxSetTempsInF(bool fahrenheit) {
  loadPrefs();
  s_tempsF = fahrenheit;
  Preferences p;
  p.begin("darkside", false);
  p.putUChar("tempF", fahrenheit ? 1 : 0);
  p.end();
  Serial.printf("[setup] temps unit %s saved\n", fahrenheit ? "F" : "C");
}

const char* gxGetTarget() {
  loadPrefs();
  lock();
  snprintf(s_targetShown, sizeof s_targetShown, "%s", s_host.c_str());
  unlock();
  return s_targetShown;
}

void gxSetTarget(const char* addr) {
  // NVS is written here (thread-safe); the socket/host swap is posted to
  // the poller task — never touch its connection from the UI task.
  Preferences p;
  p.begin("darkside", false);
  if (addr && addr[0]) p.putString("gx.addr", addr);
  else p.remove("gx.addr");
  p.end();
  lock();  // the poller task reads this buffer under the same mutex
  snprintf(s_pendingTarget, sizeof s_pendingTarget, "%s", addr ? addr : "");
  unlock();
  s_targetDirty = true;
}

static bool pollOnce(GxData& out) {
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
    s_haveSysName = false;      // refresh the system name on each new connection
    s_altModeSupported = true;  // re-probe reg 4119 on each new connection
    Serial.printf("[gx] connected %s:502\n", s_gxIp.toString().c_str());
  }

  uint16_t batt[5], pv[1], ac[1], dc[1];
  if (readRegs(100, 840, 5, batt) != kRegOk || readRegs(100, 850, 1, pv) != kRegOk ||
      readRegs(100, 817, 1, ac) != kRegOk || readRegs(100, 860, 1, dc) != kRegOk) {
    Serial.println("[gx] read failed, reconnecting next poll");
    drop(); out.valid = false; return false;
  }

  // Sensor reads are per-device NON-FATAL: an absent/asleep device answers
  // with a Modbus exception (kRegNoAnswer) and just reads not-ok (0 W/'--').
  // A kRegFault is different — the stream is suspect, later replies would
  // mis-pair with later requests and every further read would stall toward
  // its 500 ms deadline — so stop reading, leave the rest not-ok, and cycle
  // the socket below. The system values above are good; the poll succeeds.
  bool sockOk = true;

  // GX system name — once per connection, non-fatal. dbus_modbustcp packs
  // strings two ASCII chars per register, high byte first.
  if (!s_haveSysName) {
    uint16_t nm[8];
    const RegResult rn = readRegs(100, 5700, 8, nm);
    if (rn == kRegOk) {
      for (int i = 0; i < 8; i++) {
        out.sysName[2 * i] = (char)(nm[i] >> 8);
        out.sysName[2 * i + 1] = (char)(nm[i] & 0xFF);
      }
      out.sysName[16] = '\0';
      for (int i = 15; i >= 0 && (out.sysName[i] == '\0' || out.sysName[i] == ' '); i--)
        out.sysName[i] = '\0';
      out.sysNameOk = (out.sysName[0] != '\0');
      s_haveSysName = true;
      Serial.printf("[gx] system name: '%s'\n", out.sysName);
    } else if (rn == kRegFault) {
      sockOk = false;
    } else {
      s_haveSysName = true;  // GX without the register: stop asking
      Serial.printf("[gx] system name reg 5700 unavailable (modbus exception %u)"
                    " — title falls back to UI_TITLE/default\n", s_lastExc);
    }
  }

  // Orion XS output (engine off/asleep or re-numbered unit -> 0 W).
  uint16_t alt[2];
  out.altW = 0;
  RegResult r = sockOk ? readRegs(GX_ALT_UNIT, 4100, 2, alt) : kRegFault;
  if (r == kRegOk) {
    const float altV = alt[0] / 100.0f;
    const float altA = s16(alt[1]) / 10.0f;
    out.altW = (int)lroundf(altV * altA);
  } else if (r == kRegFault) {
    sockOk = false;
  }

  // Temperatures (tags sleep, batteries die).
  for (int i = 0; i < GxData::kNumTemps; i++) {
    uint16_t t[1];
    r = sockOk ? readRegs(kGxTempUnits[i], 3304, 1, t) : kRegFault;
    if (r == kRegFault) sockOk = false;
    out.tempOk[i] = (r == kRegOk);
    if (out.tempOk[i]) {
      const float degC = s16(t[0]) / 100.0f;
      out.temp[i] = gxTempsInF() ? degC * 9.0f / 5.0f + 32.0f : degC;
    }
  }

  // Controls state for the CONTROL page — same per-read non-fatal rules.
  uint16_t cv[2];
  r = sockOk ? readRegs(GX_VEBUS_UNIT, 33, 1, cv) : kRegFault;
  if (r == kRegFault) sockOk = false;
  out.mpModeOk = (r == kRegOk);
  if (out.mpModeOk) out.mpMode = cv[0];

  r = sockOk ? readRegs(GX_VEBUS_UNIT, 22, 1, cv) : kRegFault;
  if (r == kRegFault) sockOk = false;
  out.shoreLimOk = (r == kRegOk);
  if (out.shoreLimOk) out.shoreLimA = s16(cv[0]) / 10.0f;

  r = sockOk ? readRegs(100, 806, 2, cv) : kRegFault;
  if (r == kRegFault) sockOk = false;
  out.relayOk = (r == kRegOk);
  if (out.relayOk) {
    out.relayClosed[0] = (cv[0] == 1);
    out.relayClosed[1] = (cv[1] == 1);
  }

  r = sockOk ? readRegs(100, 2705, 1, cv) : kRegFault;
  if (r == kRegFault) sockOk = false;
  out.dvccOk = (r == kRegOk);
  if (out.dvccOk) out.dvccLimA = s16(cv[0]);

  // Orion /Mode (4119) only exists on newer GX firmware — one clean
  // exception per connection marks it unsupported until reconnect.
  if (s_altModeSupported && sockOk) {
    r = readRegs(GX_ALT_UNIT, 4119, 1, cv);
    if (r == kRegFault) {
      sockOk = false;
      out.altModeOk = false;
    } else if (r == kRegNoAnswer) {
      s_altModeSupported = false;
      out.altModeOk = false;
      Serial.println("[ctl] alternator /Mode reg 4119 unavailable on this GX firmware");
    } else {
      out.altModeOk = true;
      out.altMode = cv[0];
    }
  } else if (!s_altModeSupported) {
    out.altModeOk = false;
  }
  out.altModeSupported = s_altModeSupported;

  // Tank levels — same treatment.
  for (int i = 0; i < GxData::kNumTanks; i++) {
    uint16_t t[1];
    r = sockOk ? readRegs(kGxTankUnits[i], 3004, 1, t) : kRegFault;
    if (r == kRegFault) sockOk = false;
    out.tankOk[i] = (r == kRegOk);
    if (out.tankOk[i]) out.tankPct[i] = t[0] / 10.0f;
  }

  if (!sockOk) {
    // No backoff: the GX just answered the system reads, so the box is up —
    // a fresh connection next poll beats 3 s of amber dot.
    Serial.println("[gx] sensor read fault, cycling connection");
    s_sock.stop();
    s_nextAttemptMs = millis();
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

static void netTask(void*) {
  GxData local;  // persists across rounds: sysName and last values carry over
  for (;;) {
    if (s_targetDirty) {
      s_targetDirty = false;
      lock();
      if (s_pendingTarget[0]) {
        s_host = s_pendingTarget;
        s_hostOverridden = true;
      } else {
        s_host = GX_MDNS_HOST;
        s_hostOverridden = false;
      }
      const String applied = s_host;
      unlock();
      s_gxIp = IPAddress();   // re-resolve against the new target
      s_sock.stop();          // fresh connection, no backoff
      s_nextAttemptMs = millis();
      Serial.printf("[setup] gx target set to '%s'\n", applied.c_str());
    }

    // UI-queued control writes — executed here, on the socket owner, then
    // the round below re-reads state so the next snapshot is read-back
    // truth. A fault mid-write cycles the socket like any other fault.
    if (s_writeQ && s_sock.connected()) {
      WriteCmd c;
      while (xQueueReceive(s_writeQ, &c, 0) == pdTRUE) {
        if (millis() - c.queuedMs > kWriteTtlMs) {
          Serial.printf("[ctl] dropped stale write u%u r%u (queued %lums ago)\n",
                        c.unit, c.reg, (unsigned long)(millis() - c.queuedMs));
          continue;
        }
        const RegResult wr = writeReg(c.unit, c.reg, c.val);
        Serial.printf("[ctl] write u%u r%u = %d -> %s\n", c.unit, c.reg,
                      (int)(int16_t)c.val,
                      wr == kRegOk ? "ok"
                      : (wr == kRegNoAnswer ? "exception" : "fault"));
        if (wr == kRegFault) {
          s_sock.stop();
          s_nextAttemptMs = millis();
          break;
        }
      }
    }

    // Unit-id sweep for new installations (serial 'V').
    if (s_sweepReq && s_sock.connected()) {
      s_sweepReq = false;
      Serial.println("[sweep] units 200-247, vebus /Mode (reg 33)...");
      uint16_t v[1];
      for (int u = 200; u <= 247; u++) {
        const RegResult sr = readRegs((uint8_t)u, 33, 1, v);
        if (sr == kRegOk && v[0] >= 1 && v[0] <= 4)
          Serial.printf("[sweep] unit %d: /Mode=%u  <-- vebus candidate\n", u, v[0]);
        else if (sr == kRegFault) {
          Serial.println("[sweep] aborted: socket fault");
          s_sock.stop();
          s_nextAttemptMs = millis();
          break;
        }
      }
      Serial.println("[sweep] done");
    }

    const uint32_t t0 = millis();
    const bool ok = pollOnce(local);  // handles Wi-Fi-down itself
    const uint32_t dt = millis() - t0;

    lock();
    s_shared = local;
    s_pollDurMs = dt;
    if (ok) s_seq++;
    unlock();

    if (!ok && dt > 600)
      Serial.printf("[gx] slow failed poll %lums\n", (unsigned long)dt);

    // ~1 Hz measured from poll start, sliced so a queued control write
    // starts within ~100 ms instead of waiting out the full second.
    uint32_t sleepMs = (dt >= 950) ? 50 : (1000 - dt);
    while (sleepMs > 0) {
      const uint32_t slice = (sleepMs > 100) ? 100 : sleepMs;
      vTaskDelay(pdMS_TO_TICKS(slice));
      sleepMs -= slice;
      if ((s_writeQ && uxQueueMessagesWaiting(s_writeQ) > 0) || s_sweepReq ||
          s_targetDirty)
        break;
    }
  }
}

void gxStart() {
  loadPrefs();  // once, before any concurrent access
  s_mux = xSemaphoreCreateMutex();
  s_writeQ = xQueueCreate(8, sizeof(WriteCmd));
  xTaskCreatePinnedToCore(netTask, "gxpoll", 8192, nullptr, 1, nullptr, 0);
}

bool gxWrite(uint8_t unit, uint16_t reg, uint16_t value) {
  if (!s_writeQ) return false;
  const WriteCmd c = {unit, reg, value, millis()};
  return xQueueSend(s_writeQ, &c, 0) == pdTRUE;
}

void gxRequestSweep() { s_sweepReq = true; }

uint32_t gxSnapshot(GxData& out, uint32_t* lastPollMs) {
  lock();
  out = s_shared;
  const uint32_t seq = s_seq;
  if (lastPollMs) *lastPollMs = s_pollDurMs;
  unlock();
  return seq;
}
