#include "gx_poller.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "gx_settings.h"
#include "modbus_transport.h"

namespace {

IPAddress s_gxIp;              // resolved once; re-resolved after failures
uint32_t s_nextAttemptMs = 0;  // backoff gate after a failure
bool s_haveSysName = false;    // system name fetched on this connection
bool s_altModeSupported = true;  // probe reg 4119 once per connection

// Poller task <-> UI handoff.
SemaphoreHandle_t s_mux = nullptr;
GxData s_shared;               // newest sample (copied under s_mux)
uint32_t s_seq = 0;            // bumps once per successful round
uint32_t s_pollDurMs = 0;      // duration of the latest attempt

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

void lock() { if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY); }
void unlock() { if (s_mux) xSemaphoreGive(s_mux); }

bool resolveGx() {
  // A literal IP target needs no lookup. Otherwise treat the target as an
  // mDNS host (".local" optional — it doesn't go through DNS), and fall
  // back to the pinned address only for the stock config target; a typed
  // custom host that fails to resolve should fail visibly, not silently
  // poll some unrelated IP.
  String target;
  bool overridden = false;
  gxTargetCopy(target, overridden);
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
  mbStop();
  s_nextAttemptMs = millis() + 3000;
}

inline int s16(uint16_t v) { return (v > 32767) ? (int)v - 65536 : (int)v; }

// Control read-backs stay STICKY through transport faults: on a rough
// link, a mid-round fault otherwise blanks every control row for that
// round and the CONTROL page flickers dead (its guards swallow taps on
// muted rows). A fresh read or a clean exception always wins; a faulted
// or skipped read keeps the row Ok while the previous read-back (still
// in the task-persistent GxData) is under kCtlStickyMs old. Poller-task
// state only — no locking.
constexpr uint32_t kCtlStickyMs = 15000;
struct { uint32_t mp = 0, shore = 0, relay = 0, dvcc = 0, solar = 0, alt = 0; } s_ctlFresh;

bool ctlRow(RegResult r, uint32_t& okMs) {
  if (r == kRegOk) { okMs = millis(); return true; }
  if (r == kRegException) { okMs = 0; return false; }  // honest absence
  return okMs != 0 && millis() - okMs < kCtlStickyMs;
}

bool pollOnce(GxData& out) {
  if (WiFi.status() != WL_CONNECTED) { drop(); out.valid = false; return false; }
  if (!mbConnected()) {
    if ((int32_t)(millis() - s_nextAttemptMs) < 0) { out.valid = false; return false; }
    if (s_gxIp == IPAddress() && !resolveGx()) { drop(); out.valid = false; return false; }
    if (!mbConnect(s_gxIp)) {
      Serial.printf("[gx] connect %s:502 failed\n", s_gxIp.toString().c_str());
      s_gxIp = IPAddress();  // re-resolve next time (GX may have moved)
      drop(); out.valid = false; return false;
    }
    s_haveSysName = false;      // refresh the system name on each new connection
    s_altModeSupported = true;  // re-probe reg 4119 on each new connection
    Serial.printf("[gx] connected %s:502\n", s_gxIp.toString().c_str());
  }

  uint16_t batt[5], pv[1], ac[1], dc[1];
  if (mbRead(100, 840, 5, batt) != kRegOk || mbRead(100, 850, 1, pv) != kRegOk ||
      mbRead(100, 817, 1, ac) != kRegOk || mbRead(100, 860, 1, dc) != kRegOk) {
    Serial.println("[gx] read failed, reconnecting next poll");
    drop(); out.valid = false; return false;
  }

  // Sensor reads are per-device NON-FATAL: an absent/asleep device answers
  // with a Modbus exception (kRegException) and just reads not-ok
  // (0 W/'--'). A kRegFault is different — the stream is suspect, later
  // replies would mis-pair with later requests and every further read
  // would stall toward its 1500 ms deadline — so stop reading, leave the
  // rest not-ok, and cycle the socket below. The system values above are
  // good; the poll succeeds.
  bool sockOk = true;

  // GX system name — once per connection, non-fatal. dbus_modbustcp packs
  // strings two ASCII chars per register, high byte first.
  if (!s_haveSysName) {
    uint16_t nm[8];
    const RegResult rn = mbRead(100, 5700, 8, nm);
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
                    " — title falls back to UI_TITLE/default\n", mbLastException());
    }
  }

  // Orion XS output (engine off/asleep or re-numbered unit -> 0 W).
  uint16_t alt[2];
  out.altW = 0;
  RegResult r = sockOk ? mbRead(GX_ALT_UNIT, 4100, 2, alt) : kRegFault;
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
    r = sockOk ? mbRead(kGxTempUnits[i], 3304, 1, t) : kRegFault;
    if (r == kRegFault) sockOk = false;
    out.tempOk[i] = (r == kRegOk);
    if (out.tempOk[i]) {
      const float degC = s16(t[0]) / 100.0f;
      out.temp[i] = gxTempsInF() ? degC * 9.0f / 5.0f + 32.0f : degC;
    }
  }

  // Controls state for the CONTROL page — same per-read non-fatal rules.
  uint16_t cv[2];
  r = sockOk ? mbRead(GX_VEBUS_UNIT, 33, 1, cv) : kRegFault;
  if (r == kRegFault) sockOk = false;
  out.mpModeOk = ctlRow(r, s_ctlFresh.mp);
  if (r == kRegOk) out.mpMode = cv[0];

  r = sockOk ? mbRead(GX_VEBUS_UNIT, 22, 1, cv) : kRegFault;
  if (r == kRegFault) sockOk = false;
  out.shoreLimOk = ctlRow(r, s_ctlFresh.shore);
  if (r == kRegOk) out.shoreLimA = s16(cv[0]) / 10.0f;

  r = sockOk ? mbRead(100, 806, 2, cv) : kRegFault;
  if (r == kRegFault) sockOk = false;
  out.relayOk = ctlRow(r, s_ctlFresh.relay);
  if (r == kRegOk) {
    out.relayClosed[0] = (cv[0] == 1);
    out.relayClosed[1] = (cv[1] == 1);
  }

  r = sockOk ? mbRead(100, 2705, 1, cv) : kRegFault;
  if (r == kRegFault) sockOk = false;
  out.dvccOk = ctlRow(r, s_ctlFresh.dvcc);
  if (r == kRegOk) out.dvccLimA = s16(cv[0]);

  // SmartSolar /Mode — a unit of 0 means "not configured" (compile-time
  // constant, the branch folds away). An ext-control MPPT may accept the
  // read but bounce writes; read-back truth shows that on the page.
  if (GX_SOLAR_UNIT != 0) {
    r = sockOk ? mbRead(GX_SOLAR_UNIT, 774, 1, cv) : kRegFault;
    if (r == kRegFault) sockOk = false;
    out.solarModeOk = ctlRow(r, s_ctlFresh.solar);
    if (r == kRegOk) out.solarMode = cv[0];
  }

  // Orion /Mode (4119) only exists on newer GX firmware — one clean
  // exception per connection marks it unsupported until reconnect.
  if (s_altModeSupported && sockOk) {
    r = mbRead(GX_ALT_UNIT, 4119, 1, cv);
    if (r == kRegFault) {
      sockOk = false;
      out.altModeOk = ctlRow(r, s_ctlFresh.alt);
    } else if (r == kRegException) {
      s_altModeSupported = false;
      out.altModeOk = false;
      s_ctlFresh.alt = 0;
      Serial.println("[ctl] alternator /Mode reg 4119 unavailable on this GX firmware");
    } else {
      out.altModeOk = ctlRow(r, s_ctlFresh.alt);
      out.altMode = cv[0];
    }
  } else if (s_altModeSupported) {
    // Reads skipped by an earlier fault this round — sticky like every
    // other row (this case previously kept the stale flag forever).
    out.altModeOk = ctlRow(kRegFault, s_ctlFresh.alt);
  } else {
    out.altModeOk = false;
  }
  out.altModeSupported = s_altModeSupported;

  // Tank levels — same treatment.
  for (int i = 0; i < GxData::kNumTanks; i++) {
    uint16_t t[1];
    r = sockOk ? mbRead(kGxTankUnits[i], 3004, 1, t) : kRegFault;
    if (r == kRegFault) sockOk = false;
    out.tankOk[i] = (r == kRegOk);
    if (out.tankOk[i]) out.tankPct[i] = t[0] / 10.0f;
  }

  if (!sockOk) {
    // No backoff: the GX just answered the system reads, so the box is up —
    // a fresh connection next poll beats 3 s of amber dot.
    Serial.println("[gx] sensor read fault, cycling connection");
    mbStop();
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

void runSweep() {
  Serial.println("[sweep] units 1-247: solar /Mode (774); 200-247 also: vebus /Mode (33)...");
  uint16_t v[1];
  for (int u = 1; u <= 247; u++) {
    RegResult r = mbRead((uint8_t)u, 774, 1, v);
    if (r == kRegOk && (v[0] == 1 || v[0] == 4))
      Serial.printf("[sweep] unit %d: solar /Mode=%u  <-- solarcharger candidate\n", u, v[0]);
    else if (r == kRegFault) {
      Serial.println("[sweep] aborted: socket fault");
      mbStop();
      s_nextAttemptMs = millis();
      return;
    }
    if (u >= 200) {
      r = mbRead((uint8_t)u, 33, 1, v);
      if (r == kRegOk && v[0] >= 1 && v[0] <= 4)
        Serial.printf("[sweep] unit %d: /Mode=%u  <-- vebus candidate\n", u, v[0]);
      else if (r == kRegFault) {
        Serial.println("[sweep] aborted: socket fault");
        mbStop();
        s_nextAttemptMs = millis();
        return;
      }
    }
  }
  Serial.println("[sweep] done");
}

void netTask(void*) {
  GxData local;  // persists across rounds: sysName and last values carry over
  for (;;) {
    if (gxTargetConsumePending()) {
      s_gxIp = IPAddress();   // re-resolve against the new target
      mbStop();               // fresh connection, no backoff
      s_nextAttemptMs = millis();
    }

    if (s_sweepReq && mbConnected()) {
      s_sweepReq = false;
      runSweep();
    }

    // UI-queued control writes — on the socket-owner task, then the poll
    // below re-reads state so the page shows read-back truth fast.
    if (s_writeQ && mbConnected()) {
      WriteCmd c;
      while (xQueueReceive(s_writeQ, &c, 0) == pdTRUE) {
        if (millis() - c.queuedMs > kWriteTtlMs) {
          Serial.printf("[ctl] dropped stale write u%u r%u (queued %lums ago)\n",
                        c.unit, c.reg, (unsigned long)(millis() - c.queuedMs));
          continue;
        }
        const RegResult wr = mbWrite(c.unit, c.reg, c.val);
        Serial.printf("[ctl] write u%u r%u = %d -> %s\n", c.unit, c.reg,
                      (int)(int16_t)c.val,
                      wr == kRegOk ? "ok"
                      : (wr == kRegException ? "exception" : "fault"));
        if (wr == kRegFault) {
          mbStop();
          s_nextAttemptMs = millis();
          break;
        }
      }
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

    // ~1 Hz measured from poll start, in short slices so queued control
    // writes never wait a full second for the task to wake.
    uint32_t sleepMs = (dt >= 950) ? 50 : (1000 - dt);
    while (sleepMs > 0) {
      const uint32_t slice = (sleepMs > 100) ? 100 : sleepMs;
      vTaskDelay(pdMS_TO_TICKS(slice));
      sleepMs -= slice;
      if ((s_writeQ && uxQueueMessagesWaiting(s_writeQ) > 0) || s_sweepReq) break;
    }
  }
}

}  // namespace

void gxStart() {
  gxSettingsLoad();  // once, before any concurrent access
  s_mux = xSemaphoreCreateMutex();
  s_writeQ = xQueueCreate(8, sizeof(WriteCmd));
  xTaskCreatePinnedToCore(netTask, "gxpoll", 8192, nullptr, 1, nullptr, 0);
}

uint32_t gxSnapshot(GxData& out, uint32_t* lastPollMs) {
  lock();
  out = s_shared;
  const uint32_t seq = s_seq;
  if (lastPollMs) *lastPollMs = s_pollDurMs;
  unlock();
  return seq;
}

bool gxWrite(uint8_t unit, uint16_t reg, uint16_t value) {
  if (!s_writeQ) return false;
  const WriteCmd c = {unit, reg, value, millis()};
  return xQueueSend(s_writeQ, &c, 0) == pdTRUE;
}

void gxRequestSweep() { s_sweepReq = true; }
