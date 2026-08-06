#pragma once

// SOUNDS sub-screen (reached from the setup screen): assign a sound to
// the BOOT and CHARGE events from the built-ins (silent / chirp / baked-in
// voice) and any MP3s in /sounds/ on the SD card. Tapping a row assigns it
// to the active slot, saves to NVS, and plays it as a preview — no SAVE
// step. Build once after uiSetupBuild; open pushes onto the active screen.
void uiSoundsBuild();
void uiSoundsOpen();
