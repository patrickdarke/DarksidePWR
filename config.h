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

// MultiPlus (com.victronenergy.vebus) — discover with the serial 'V' sweep.
#ifndef GX_VEBUS_UNIT
#define GX_VEBUS_UNIT 229
#endif

// Temperature sensors (any com.victronenergy.temperature service). Labels
// align by position; list length is free (polling/display resize to match,
// labels must match length; the footer line fits about three).
#ifndef GX_TEMP_UNITS
#define GX_TEMP_UNITS {22, 23, 25}
#endif
#ifndef GX_TEMP_LABELS
#define GX_TEMP_LABELS {"OUT", "FRIDGE", "IN"}
#endif

// Tank senders (any com.victronenergy.tank service). Same rules.
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
