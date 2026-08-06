#pragma once

// Sound player + per-event sound choices. Two slots — BOOT and CHARGE —
// each holding one of: "!off", "!chirp", "!voice" (the baked-in
// tools/make_voice.sh clip, when compiled), or the bare name of an MP3 in
// /sounds/ on the SD card (decoded on the fly with the vendored minimp3,
// played on the NS4168 I2S speaker). Choices persist in NVS ("darkside":
// snd.boot / snd.chg) and are edited on the SOUNDS setup sub-screen.
//
// Playback is always async on a short-lived task; a missing card or file
// degrades down the chain SD file -> baked-in voice -> chirp, so the
// speaker path can never error into silence unless "!off" was chosen.

constexpr int kSndBoot = 0;
constexpr int kSndCharge = 1;

void soundInit();          // load NVS choices; call once from setup()
void soundPlayBoot();      // play the BOOT slot (waits briefly for SD mount)
void soundPlayCharge();    // play the CHARGE slot (serial 'B' + full alert)
void soundPreview(const char* choice);      // play any choice string now
const char* soundGet(int slot);             // current choice string
void soundSet(int slot, const char* choice);  // persist + apply immediately
bool soundHasVoice();      // baked-in voice.h present in this build?
bool soundBusy();          // something playing right now?

// Playback volume, 5..100 % (NVS "snd.vol", default 85 — the level the
// fleet shipped at). soundSetVol applies immediately (the player reads it
// per chunk, so dragging changes a playing clip live); soundSaveVol also
// persists — the brightness slider's live-drag/save-on-release pattern.
int soundGetVol();
void soundSetVol(int pct);
void soundSaveVol(int pct);
// Fill names[][] with the MP3 basenames in /sounds (sorted, "._" junk
// skipped). Returns the count; 0 when no card. Never blocks the UI for
// more than ~150 ms — while a sound is playing it reports -1 (caller
// keeps its previous list).
int soundListFiles(char names[][32], int maxNames);
