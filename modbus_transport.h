#pragma once
#include <IPAddress.h>
#include <stdint.h>

// Modbus TCP framing over one kept-alive socket. Owns the WiFiClient, the
// transaction ids, and the last exception code — nothing about Victron,
// polling cadence, or which registers mean what (that's gx_poller).
// Single-task use only: the poller task is the sole caller once running.

// Result of one register operation. kRegException = the unit replied with a
// Modbus exception (absent/napping device) — the frame was consumed cleanly
// and the socket stays usable. kRegFault = write/timeout/framing/TID
// trouble — reply bytes may still be in flight, so the caller must close
// the connection before the next operation.
enum RegResult { kRegOk, kRegException, kRegFault };

bool mbConnect(const IPAddress& ip);  // 502, 1.5 s timeout, TCP_NODELAY
bool mbConnected();
void mbStop();

// FC3 read of `count` holding registers into regs[]. Parses the MBAP length
// before the body so an exception response is consumed without desyncing or
// stalling the socket.
RegResult mbRead(uint8_t unit, uint16_t addr, uint16_t count, uint16_t* regs);

// FC6 single-register write, same framing/tri-state rules (a good response
// echoes addr+value).
RegResult mbWrite(uint8_t unit, uint16_t reg, uint16_t val);

uint8_t mbLastException();  // code from the most recent kRegException
