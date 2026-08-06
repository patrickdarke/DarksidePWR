#include "sound.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <Preferences.h>
#include <SD.h>
#include <math.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "beeper.h"   // piezo chirp = the bottom of the fallback chain
#include "history.h"  // SD mount state + the shared SD mutex

// MP3 decode: vendored minimp3 (lib/minimp3, public domain/CC0 — safe in
// this MIT repo; the GPL Arduino audio libs are not). Implementation is
// compiled exactly once, here.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

// Baked-in fallback voice (tools/make_voice.sh) — optional and gitignored,
// like secrets.h; adopter builds without it just lose the "!voice" choice.
#if __has_include("voice.h")
#include "voice.h"
#define SOUND_HAS_VOICE 1
#endif

namespace {

// NS4168 speaker pins, hardware-verified in the af978da chime era (vendor
// V1.2-1.4 audio lesson). I2S is begun/ended around each playback; CTRL 21
// stays LOW throughout (beeperInit parks it in the audio-enabled state).
constexpr int kPinBclk = 13;
constexpr int kPinLrc = 11;
constexpr int kPinDout = 12;
// Playback volume lives in s_volPct (SOUNDS screen slider, NVS-backed);
// the play loops read it per chunk, so a drag reshapes a playing clip.
constexpr int kVolDefault = 85;  // the level the fleet shipped at
constexpr int kVolMin = 5;       // a forgotten 0 would read as dead speaker
volatile int s_volPct = kVolDefault;
// Fixed high-pass on SD playback: the bare cone can't reproduce below
// ~150 Hz — that band only wastes excursion and distorts everything
// above it. SD MP3s play as-authored, so the player supplies the cut
// (the baked voice.h already gets it from make_voice.sh's mastering).
constexpr float kHpHz = 150.0f;
constexpr char kSoundsDir[] = "/sounds";

const char* kSlotKey[2] = {"snd.boot", "snd.chg"};
const char* kSlotName[2] = {"boot", "charge"};

#ifdef SOUND_HAS_VOICE
constexpr char kDefCharge[] = "!voice";
#else
constexpr char kDefCharge[] = "!chirp";
#endif

char s_choice[2][40] = {"!off", ""};  // charge default filled in soundInit
volatile bool s_busy = false;

// Play parameters for the in-flight task. One playback at a time (s_busy),
// so plain statics are safe.
char s_playPath[64];
bool s_playIsVoice = false;
bool s_playWaitSd = false;

// Decoder + IO buffers, static so the 4 KB task stack stays small. ~32 KB
// of DRAM total; only touched while a sound plays.
mp3dec_t s_dec;
uint8_t s_in[16384];
int16_t s_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
int16_t s_out[MINIMP3_MAX_SAMPLES_PER_FRAME];

void writeSilence(I2SClass& i2s, int hz, int ms) {
  memset(s_out, 0, sizeof s_out);
  int frames = hz * ms / 1000;
  while (frames > 0) {
    const int n = frames > 256 ? 256 : frames;
    i2s.write((uint8_t*)s_out, n * 2 * sizeof(int16_t));
    frames -= n;
  }
}

#ifdef SOUND_HAS_VOICE
bool playVoice(I2SClass& i2s) {
  if (!i2s.begin(I2S_MODE_STD, kVoiceRate, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO)) {
    Serial.println("[snd] i2s begin failed");
    return false;
  }
  const int frames = (int)(kVoicePcmLen / 2);
  writeSilence(i2s, kVoiceRate, 60);
  int done = 0;
  while (done < frames) {
    const int gain = (s_volPct * 256) / 100;  // per chunk: live volume
    const int n = (frames - done) > 256 ? 256 : (frames - done);
    for (int i = 0; i < n; i++) {
      // Byte-assembled: the xxd array has no int16 alignment guarantee.
      const uint8_t* p = &kVoicePcm[2 * (done + i)];
      int16_t s = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
      s = (int16_t)((s * gain) / 256);
      s_out[2 * i] = s;
      s_out[2 * i + 1] = s;
    }
    i2s.write((uint8_t*)s_out, n * 2 * sizeof(int16_t));
    done += n;
  }
  writeSilence(i2s, kVoiceRate, 60);
  i2s.end();
  return true;
}
#endif

// Stream one MP3 from the card. Holds the SD mutex for the whole playback
// (a few seconds) — the history CSV queue rides that out. Returns true if
// any audio actually played.
bool playMp3(I2SClass& i2s, const char* path) {
  if (s_playWaitSd) {  // boot race: the storage task mounts the card async
    for (int i = 0; i < 50 && !histSdReady(); i++) vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!histSdReady() || !histSdTake(30000)) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) {
    histSdGive();
    Serial.printf("[snd] open failed: %s\n", path);
    return false;
  }

  mp3dec_init(&s_dec);
  int have = 0, hz = 0;
  bool eof = false, started = false, any = false;
  // 150 Hz Butterworth high-pass biquad (RBJ), coefficients set once the
  // first frame reveals the file's sample rate; state is per-playback.
  float hb0 = 1, hb1 = 0, hb2 = 0, ha1 = 0, ha2 = 0;
  float hx1 = 0, hx2 = 0, hy1 = 0, hy2 = 0;
  for (;;) {
    if (!eof && have < (int)sizeof(s_in) / 2) {
      const int r = f.read(s_in + have, sizeof(s_in) - have);
      if (r <= 0) eof = true;
      else have += r;
    }
    if (have <= 0) break;
    mp3dec_frame_info_t info;
    const int samples = mp3dec_decode_frame(&s_dec, s_in, have, s_pcm, &info);
    if (info.frame_bytes <= 0) {
      if (eof) break;  // trailing junk only
      if (have >= (int)sizeof(s_in) - 64) {
        // A full buffer with no sync anywhere (oversized ID3 art, junk):
        // discard the front and move on, or this scan would spin the CPU
        // at 100% until the task watchdog reboots the panel.
        memmove(s_in, s_in + have - 2048, 2048);
        have = 2048;
      }
      continue;        // need more data
    }
    if (samples > 0) {
      if (!started) {
        hz = info.hz;
        if (!i2s.begin(I2S_MODE_STD, hz, I2S_DATA_BIT_WIDTH_16BIT,
                       I2S_SLOT_MODE_STEREO)) {
          Serial.println("[snd] i2s begin failed");
          break;
        }
        const float w0 = 2.0f * (float)M_PI * kHpHz / (float)hz;
        const float alpha = sinf(w0) / (2.0f * 0.70710678f);  // Q = 1/sqrt(2)
        const float c = cosf(w0);
        const float a0 = 1.0f + alpha;
        hb0 = (1.0f + c) / (2.0f * a0);
        hb1 = -(1.0f + c) / a0;
        hb2 = hb0;
        ha1 = (-2.0f * c) / a0;
        ha2 = (1.0f - alpha) / a0;
        started = true;
        writeSilence(i2s, hz, 60);
      }
      // Mono NS4168: duplicate mono sources, downmix stereo ones so no
      // content lives only in the slot the amp ignores; high-pass, then
      // gain, then clamp (the filter can overshoot a full-scale input).
      const int gain = (s_volPct * 256) / 100;  // per frame: live volume
      for (int i = 0; i < samples; i++) {
        const int m = (info.channels == 2)
                          ? ((int)s_pcm[2 * i] + (int)s_pcm[2 * i + 1]) / 2
                          : s_pcm[i];
        const float x = (float)m;
        const float y = hb0 * x + hb1 * hx1 + hb2 * hx2 - ha1 * hy1 - ha2 * hy2;
        hx2 = hx1;
        hx1 = x;
        hy2 = hy1;
        hy1 = y;
        int s = ((int)y * gain) / 256;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        s_out[2 * i] = (int16_t)s;
        s_out[2 * i + 1] = (int16_t)s;
      }
      i2s.write((uint8_t*)s_out, samples * 2 * sizeof(int16_t));
      any = true;
    }
    have -= info.frame_bytes;
    memmove(s_in, s_in + info.frame_bytes, have);
  }
  if (started) {
    writeSilence(i2s, hz, 60);
    i2s.end();
  }
  f.close();
  histSdGive();
  if (!any) Serial.printf("[snd] no decodable audio in %s\n", path);
  return any;
}

// Crash guard: armed in NVS just before an SD playback, disarmed right
// after it returns. If the panel dies mid-decode (panic/brownout), the
// flag survives the reboot and soundInit reverts any SD-file slot to its
// default — a poisonous file gets at most ONE crash, never a boot loop.
// (Cost a real boot-brick 2026-08-06: sysop.mp3 as the boot sound +
// the stack bug below = panic on every boot.)
void sdGuard(bool armed) {
  Preferences p;
  p.begin("darkside", false);
  p.putUChar("snd.guard", armed ? 1 : 0);
  p.end();
}

void playTask(void*) {
  I2SClass i2s;
  i2s.setPins(kPinBclk, kPinLrc, kPinDout, -1, -1);
  bool ok = false;
  if (s_playIsVoice) {
#ifdef SOUND_HAS_VOICE
    ok = playVoice(i2s);
#endif
  } else {
    sdGuard(true);
    ok = playMp3(i2s, s_playPath);
    sdGuard(false);
#ifdef SOUND_HAS_VOICE
    if (!ok) ok = playVoice(i2s);  // card/file gone -> baked-in voice
#endif
  }
  s_busy = false;       // release before the chirp — it has its own guard
  if (!ok) beeperChirp();  // bottom of the chain (piezo needs no I2S)
  vTaskDelete(nullptr);
}

// Resolve a choice string and start playback. waitSd only matters for SD
// files picked as the boot sound (card mounts async at boot).
void playChoice(const char* choice, bool waitSd) {
  if (!choice || !choice[0] || strcmp(choice, "!off") == 0) return;
  if (strcmp(choice, "!chirp") == 0) {
    beeperChirp();
    return;
  }
  if (s_busy) return;
  s_busy = true;
  if (strcmp(choice, "!voice") == 0) {
#ifdef SOUND_HAS_VOICE
    s_playIsVoice = true;
#else
    s_busy = false;
    beeperChirp();
    return;
#endif
  } else {
    s_playIsVoice = false;
    snprintf(s_playPath, sizeof s_playPath, "%s/%s", kSoundsDir, choice);
  }
  s_playWaitSd = waitSd;
  // 24 KB stack: minimp3 keeps its decode scratch (~16 KB of float
  // synthesis buffers) on the CALLER'S STACK — 4 KB here tripped the
  // stack canary on the first real MP3 (panic in task "sound",
  // 2026-08-06). The task is short-lived, so the heap cost is transient.
  xTaskCreatePinnedToCore(playTask, "sound", 24576, nullptr, 1, nullptr, 0);
}

}  // namespace

void soundInit() {
  snprintf(s_choice[kSndCharge], sizeof s_choice[0], "%s", kDefCharge);
  Preferences p;
  if (p.begin("darkside", /*readOnly=*/false)) {
    int v = p.getUChar("snd.vol", kVolDefault);
    if (v < kVolMin) v = kVolMin;
    if (v > 100) v = 100;
    s_volPct = v;
    const bool crashed = p.getUChar("snd.guard", 0) != 0;
    if (crashed) p.putUChar("snd.guard", 0);
    for (int i = 0; i < 2; i++) {
      if (p.isKey(kSlotKey[i])) {
        const String v = p.getString(kSlotKey[i], "");
        if (v.length()) snprintf(s_choice[i], sizeof s_choice[0], "%s", v.c_str());
      }
      // Last SD playback never finished (panel crashed/browned out with
      // the guard armed): any slot pointing at an SD file goes back to
      // its default so the panel always boots stable. The file stays on
      // the card — re-pick it on the SOUNDS screen to try again.
      if (crashed && s_choice[i][0] != '!' && s_choice[i][0] != '\0') {
        Serial.printf("[snd] previous SD playback crashed — %s '%s' reverted\n",
                      kSlotName[i], s_choice[i]);
        snprintf(s_choice[i], sizeof s_choice[0], "%s",
                 i == kSndBoot ? "!off" : kDefCharge);
        p.putString(kSlotKey[i], s_choice[i]);
      }
    }
    p.end();
  }
  Serial.printf("[snd] boot='%s' charge='%s'\n", s_choice[kSndBoot],
                s_choice[kSndCharge]);
}

void soundPlayBoot() { playChoice(s_choice[kSndBoot], /*waitSd=*/true); }
void soundPlayCharge() { playChoice(s_choice[kSndCharge], /*waitSd=*/false); }
void soundPreview(const char* choice) { playChoice(choice, /*waitSd=*/false); }

const char* soundGet(int slot) { return s_choice[slot & 1]; }

void soundSet(int slot, const char* choice) {
  slot &= 1;
  snprintf(s_choice[slot], sizeof s_choice[0], "%s", choice);
  Preferences p;
  p.begin("darkside", false);
  p.putString(kSlotKey[slot], choice);
  p.end();
  Serial.printf("[snd] %s sound = '%s'\n", kSlotName[slot], choice);
}

bool soundHasVoice() {
#ifdef SOUND_HAS_VOICE
  return true;
#else
  return false;
#endif
}

bool soundBusy() { return s_busy; }

int soundGetVol() { return s_volPct; }

void soundSetVol(int pct) {
  if (pct < kVolMin) pct = kVolMin;
  if (pct > 100) pct = 100;
  s_volPct = pct;
}

void soundSaveVol(int pct) {
  soundSetVol(pct);
  Preferences p;
  p.begin("darkside", false);
  p.putUChar("snd.vol", (uint8_t)s_volPct);
  p.end();
  Serial.printf("[snd] volume %d%% saved\n", (int)s_volPct);
}

int soundListFiles(char names[][32], int maxNames) {
  if (!histSdReady()) return 0;
  if (!histSdTake(150)) return -1;  // player mid-clip: keep the old list
  int n = 0;
  File dir = SD.open(kSoundsDir);
  if (dir && dir.isDirectory()) {
    File f;
    while (n < maxNames && (f = dir.openNextFile())) {
      if (!f.isDirectory()) {
        const char* full = f.name();
        const char* base = strrchr(full, '/');
        base = base ? base + 1 : full;
        const size_t len = strlen(base);
        // .mp3 only; skip macOS "._" AppleDouble droppings.
        if (len > 4 && len < 32 && strncmp(base, "._", 2) != 0 &&
            strcasecmp(base + len - 4, ".mp3") == 0) {
          snprintf(names[n], 32, "%s", base);
          n++;
        }
      }
      f.close();
    }
    dir.close();
  }
  histSdGive();
  // Insertion sort — the list is tiny and this keeps the menu stable.
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && strcasecmp(names[j - 1], names[j]) > 0; j--) {
      char tmp[32];
      memcpy(tmp, names[j - 1], 32);
      memcpy(names[j - 1], names[j], 32);
      memcpy(names[j], tmp, 32);
    }
  return n;
}
