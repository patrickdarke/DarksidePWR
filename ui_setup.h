#pragma once

// On-device Wi-Fi setup screen: async network scanner/picker plus SSID and
// password textareas with an on-screen keyboard (first real use of the
// GT911 touch). SAVE persists to NVS (namespace "darkside", keys
// "wifi.ssid"/"wifi.pass") and re-joins immediately; at boot NVS wins over
// the secrets.h fallback (see DarksidePWR.ino).
void uiSetupBuild();  // create the screen once, after lv_init
void uiSetupOpen();   // switch to it and kick off a scan
