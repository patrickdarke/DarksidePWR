#pragma once

// Non-secret configuration for the SECOND install (ELECROW CrowPanel Advance
// 5.0", 800x480) — a new rig, not the truck. Every define is #ifndef-guarded
// so secrets.h may override any of them, same rule as config_35.h.
//
// Device roster (owner-provided, unit ids NOT yet known — this is a new,
// uncommissioned install): one MultiPlus, two SmartSolar MPPT chargers, one
// Orion DC-DC (alternator) charger, one LPG tank sender via Bluetooth, one
// SmartShunt + two Bluetooth smart batteries (read via the unit-100 system
// aggregate — see CLAUDE.md, no separate per-battery registers needed). No
// Ruuvi temperature sensors on this install.
//
// TODO before first real use: every unit id below is a PLACEHOLDER (0).
// Discover the real ones with the serial 'V' sweep once the GX is reachable
// (see SETUP.md) and fill them in here or override from secrets.h.

// Victron GX addressing: mDNS name first, pinned address as fallback.
#ifndef GX_MDNS_HOST
#define GX_MDNS_HOST "venus"
#endif
#ifndef GX_FALLBACK_IP
#define GX_FALLBACK_IP "0.0.0.0"
#endif

// Orion DC-DC alternator charger (com.victronenergy.alternator). Placeholder
// — discover with the serial 'V' sweep (sweeps 200-247 for vebus; the Orion
// itself is usually easiest to read off its VRM bracket number).
#ifndef GX_ALT_UNIT
#define GX_ALT_UNIT 0
#endif

// Two SmartSolar MPPT chargers (com.victronenergy.solarcharger). Placeholder
// unit ids — the serial 'V' sweep already reports EVERY unit that answers a
// plausible solar /Mode, so both should show up in one sweep. List length is
// free including {} = none (polling, control-page rows, and telemetry
// resize to match; labels must match length).
#ifndef GX_SOLAR_UNITS
#define GX_SOLAR_UNITS {0, 0}
#endif
#ifndef GX_SOLAR_LABELS
#define GX_SOLAR_LABELS {"MPPT 1", "MPPT 2"}
#endif

// MultiPlus (com.victronenergy.vebus) — discover with the serial 'V' sweep.
#ifndef GX_VEBUS_UNIT
#define GX_VEBUS_UNIT 0
#endif

// No Ruuvi temperature sensors on this install — empty list is legal (the
// footer temps line just doesn't appear; GxData arrays pad to >=1 internally
// so {} stays valid C++).
#ifndef GX_TEMP_UNITS
#define GX_TEMP_UNITS {}
#endif
#ifndef GX_TEMP_LABELS
#define GX_TEMP_LABELS {}
#endif

// One LPG tank sender via Bluetooth (any com.victronenergy.tank service).
#ifndef GX_TANK_UNITS
#define GX_TANK_UNITS {0}
#endif
#ifndef GX_TANK_LABELS
#define GX_TANK_LABELS {"LPG"}
#endif

// Temperature display unit: irrelevant with no temp sensors configured, but
// keep the define so gx_settings' NVS default logic has something to load.
#ifndef TEMPS_IN_F
#define TEMPS_IN_F 1
#endif

// Display title (upper left). "" = auto-pull the GX system name (needs a
// Venus release newer than 3.75, same caveat as the 3.5" install).
#ifndef UI_TITLE
#define UI_TITLE ""
#endif
