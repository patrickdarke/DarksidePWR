#include "modbus_transport.h"

#include <WiFi.h>

namespace {

WiFiClient s_sock;
uint16_t s_tid = 1;
uint8_t s_lastExc = 0;

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

}  // namespace

bool mbConnect(const IPAddress& ip) {
  if (!s_sock.connect(ip, 502, 1500)) return false;
  s_sock.setNoDelay(true);
  return true;
}

bool mbConnected() { return s_sock.connected(); }

void mbStop() { s_sock.stop(); }

RegResult mbRead(uint8_t unit, uint16_t addr, uint16_t count, uint16_t* regs) {
  uint8_t req[12];
  const uint16_t tid = s_tid++;
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
  if (hdr[7] != 3) {                 // exception response
    s_lastExc = body[0];
    return kRegException;
  }
  if (body[0] != count * 2 || bodyLen != 1 + count * 2) return kRegFault;
  for (int i = 0; i < count; i++)
    regs[i] = ((uint16_t)body[1 + 2 * i] << 8) | body[2 + 2 * i];
  return kRegOk;
}

RegResult mbWrite(uint8_t unit, uint16_t reg, uint16_t val) {
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
    return kRegException;
  }
  if (bodyLen != 4) return kRegFault;
  return kRegOk;
}

uint8_t mbLastException() { return s_lastExc; }
