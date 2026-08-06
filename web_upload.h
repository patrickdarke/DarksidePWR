#pragma once

// Tiny HTTP manager for the /sounds folder — upload MP3s from any browser
// on the LAN (phone included) instead of pulling the SD card. Serves:
//   GET  /        file list + upload form + current boot/charge picks
//   POST /upload  multipart MP3 -> /sounds/<name> (sanitized, overwrite ok)
//   GET  /del?f=  remove one file (assignments degrade down the fallback
//                 chain if the picked file disappears)
// Runs on its own core-0 task so a slow upload never touches the LVGL
// loop; every SD access sits inside histSdTake/histSdGive like the player.
// No auth — this is a truck-LAN convenience, same trust level as the GX's
// own Modbus port. Sound picking stays on the panel (gear -> SOUNDS).
void webUploadStart();  // idempotent; call once Wi-Fi is up
bool webUploadUp();
