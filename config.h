#pragma once

// Non-secret configuration, with this installation's values as committed
// defaults. Every define is #ifndef-guarded, so secrets.h (gitignored,
// included before this file everywhere) may override any of them — but only
// true secrets (Wi-Fi credentials) HAVE to live there.
//
// Unit ids are Victron device instances (the number in brackets in the VRM
// device list). Serial 'V' sweeps for the MultiPlus; see SETUP.md.

// Victron GX addressing: mDNS name first, pinned address as fallback.
// The setup screen's GX ADDRESS field (NVS) overrides at runtime and skips
// the fallback for custom targets.
#ifndef GX_MDNS_HOST
#define GX_MDNS_HOST "venus"
#endif
#ifndef GX_FALLBACK_IP
#define GX_FALLBACK_IP "10.20.30.239"
#endif

// Orion XS alternator charger (com.victronenergy.alternator).
#ifndef GX_ALT_UNIT
#define GX_ALT_UNIT 239
#endif

// SmartSolar MPPT (com.victronenergy.solarcharger) — discover with the
// serial 'V' sweep; VE.Can devices reach Modbus through the static
// unit-id mapping, so the unit is NOT the VRM instance number. 0 = no
// solar on/off control (row shows muted). 1 on this system (found by
// sweep; the VE.Can MPPT answers there).
#ifndef GX_SOLAR_UNIT
#define GX_SOLAR_UNIT 1
#endif

// MultiPlus (com.victronenergy.vebus) — discover with the serial 'V' sweep.
#ifndef GX_VEBUS_UNIT
#define GX_VEBUS_UNIT 229
#endif

// Temperature sensors (any com.victronenergy.temperature service). Labels
// align by position; list length is free including {} = none (polling,
// display, and telemetry resize to match; labels must match length; the
// footer line fits about three).
#ifndef GX_TEMP_UNITS
#define GX_TEMP_UNITS {22, 23, 25}
#endif
#ifndef GX_TEMP_LABELS
#define GX_TEMP_LABELS {"OUT", "FRIDGE", "IN"}
#endif

// Tank senders (any com.victronenergy.tank service). Same rules, {} ok.
#ifndef GX_TANK_UNITS
#define GX_TANK_UNITS {23, 24, 25}
#endif
#ifndef GX_TANK_LABELS
#define GX_TANK_LABELS {"BLANTON", "ELIJAH", "PAPPY"}
#endif

// Temperature display unit: 1 = Fahrenheit, 0 = Celsius. First-boot
// default — the setup screen's UNITS button overrides via NVS.
#ifndef TEMPS_IN_F
#define TEMPS_IN_F 1
#endif

// Display title (upper left). "" = auto-pull the GX system name (needs a
// Venus release newer than 3.75); set a string to pin it.
#ifndef UI_TITLE
#define UI_TITLE ""
#endif

// Wall clock (top-right of the power screen): SNTP over the same Wi-Fi.
// POSIX TZ string — default US Eastern with DST rules; change it when the
// truck relocates half a continent (no auto-timezone on purpose).
#ifndef CLOCK_TZ
#define CLOCK_TZ "EST5EDT,M3.2.0,M11.1.0"
#endif
#ifndef CLOCK_NTP1
#define CLOCK_NTP1 "pool.ntp.org"
#endif
#ifndef CLOCK_NTP2
#define CLOCK_NTP2 "time.nist.gov"
#endif

// Battery tile title (upper-left tile) — installation-flavored, so it is
// overridable per build like UI_TITLE ("HOUSE" for the truck, e.g.
// "NG BATT" for the camper).
#ifndef UI_BATT_LABEL
#define UI_BATT_LABEL "HOUSE"
#endif

// 24 h history storage backends (history.cpp): FFat snapshots ride the
// partition scheme's 9 MB FAT area; the TF card gets a daily CSV. SD pins
// are the TF slot's own HSPI bus, from ELECROW lesson-04 (V1.2-V1.4).
#define HIST_ENABLE_STORAGE 1
#ifndef HIST_SD_SCK
#define HIST_SD_SCK 5
#endif
#ifndef HIST_SD_MISO
#define HIST_SD_MISO 4
#endif
#ifndef HIST_SD_MOSI
#define HIST_SD_MOSI 6
#endif
#ifndef HIST_SD_CS
#define HIST_SD_CS 7
#endif
