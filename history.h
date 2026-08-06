#pragma once
#include "gx_poller.h"

// 24 h of per-minute history for the tile values + SOC, recorded from the
// same samples the UI renders. RAM ring in PSRAM; when HIST_ENABLE_STORAGE
// is defined (config.h), a 15-min FFat snapshot makes it survive reboots
// and an SD card (if present) gets a daily CSV, one line per minute.
// Storage is strictly optional — no card and no FAT partition just means
// RAM-only history; the UI never blocks on any of it.

enum HistSeries : int {
  kHistV = 0,   // battery volts        (tile 0)
  kHistA,       // battery amps         (tile 1)
  kHistInW,     // charge input watts   (tile 2)
  kHistAcW,     // AC loads watts       (tile 3)
  kHistSoc,     // state of charge %    (recorded + CSV; no tile tap yet)
  kHistCount
};

constexpr int kHistMinutes = 24 * 60;

void histInit();                   // alloc ring, start the storage task
void histRecord(const GxData& d);  // fold one sample (UI task; cheap, no IO)
// Copy one series into out[kHistMinutes], oldest..newest, NAN = gap.
// Returns the epoch minute of the newest slot (0 = nothing recorded yet).
uint32_t histGet(int series, float* out);
void histStatus();                 // serial 'H' one-line status

// SD card sharing: this module owns the card (mount + minute retry), but
// the sound player (sound.cpp) streams MP3s from it too. Every SD.* access
// — here and there — sits between histSdTake/histSdGive: FatFS volume
// state is not task-safe on its own. Boards without storage: never-ready
// stubs.
bool histSdReady();                  // card mounted right now?
bool histSdTake(uint32_t maxWaitMs); // false = timed out (0 = try-lock)
void histSdGive();
