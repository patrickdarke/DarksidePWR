#include "web_upload.h"

#include <Arduino.h>
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "gx_settings.h"  // gxLabel — page title matches the panel header
#include "history.h"      // SD mutex + mount state
#include "sound.h"

namespace {

WebServer s_server(80);
volatile bool s_up = false;

// Upload state for the multipart callbacks.
File s_upFile;
bool s_upLocked = false;
char s_upName[36] = "";
char s_lastMsg[64] = "";

// /sounds/<name> from an untrusted client name: basename only, .mp3 only,
// short, whitelisted characters — no traversal, no AppleDouble junk.
bool sanitizeName(const String& raw, char* out, size_t cap) {
  const int slash = raw.lastIndexOf('/');
  String base = (slash >= 0) ? raw.substring(slash + 1) : raw;
  const int bs = base.lastIndexOf('\\');
  if (bs >= 0) base = base.substring(bs + 1);
  if (base.length() < 5 || base.length() > 31) return false;
  if (base.startsWith("._")) return false;
  String low = base;
  low.toLowerCase();
  if (!low.endsWith(".mp3")) return false;
  for (size_t i = 0; i < base.length(); i++) {
    const char c = base[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == ' '))
      return false;
  }
  snprintf(out, cap, "%s", base.c_str());
  return true;
}

void sendRoot() {
  char files[20][32];
  const int n = soundListFiles(files, 20);

  String p;
  p.reserve(3072);
  p += F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>sounds</title><body style='background:#0b0f14;color:#e8f0fa;"
         "font-family:-apple-system,system-ui,sans-serif;max-width:480px;"
         "margin:24px auto;padding:0 16px'>");
  p += "<h2 style='color:#8c96aa;font-weight:600'>";
  const char* title = gxLabel(kGxLblTitle);
  p += (title[0] ? title : "PWR MONITOR");
  p += " &mdash; SOUNDS</h2>";
  if (s_lastMsg[0]) {
    p += "<p style='color:#2dd4bf'>";
    p += s_lastMsg;
    p += "</p>";
    s_lastMsg[0] = '\0';
  }
  p += "<p style='color:#8c96aa'>boot: <b style='color:#e8f0fa'>";
  p += soundGet(kSndBoot);
  p += "</b> &nbsp; charge: <b style='color:#e8f0fa'>";
  p += soundGet(kSndCharge);
  p += "</b><br><small>assign sounds on the panel: gear &rarr; SOUNDS</small></p>";

  if (!histSdReady()) {
    p += F("<p style='color:#ff5050'>no SD card mounted &mdash; uploads need one</p>");
  } else if (n < 0) {
    p += F("<p>card busy (a sound is playing) &mdash; refresh in a moment</p>");
  } else {
    p += "<h3 style='color:#8c96aa'>/sounds</h3><ul>";
    for (int i = 0; i < n; i++) {
      p += "<li style='margin:6px 0'>";
      p += files[i];
      p += " <a style='color:#ff5050;text-decoration:none' href='/del?f=";
      p += files[i];
      p += "' onclick='return confirm(\"delete ";
      p += files[i];
      p += "?\")'>[x]</a></li>";
    }
    if (n == 0) p += "<li style='color:#8c96aa'>(empty)</li>";
    p += "</ul>";
  }

  p += F("<form method=POST action=/upload enctype=multipart/form-data "
         "style='margin-top:16px'>"
         "<input type=file name=f accept=.mp3 required style='color:#8c96aa'> "
         "<input type=submit value=UPLOAD style='background:#2dd4bf;color:#0b0f14;"
         "border:0;padding:8px 18px;border-radius:6px;font-weight:700'></form>"
         "<p style='color:#8c96aa'><small>.mp3 only, any bitrate; names up to 31 "
         "chars. New files appear in the panel's SOUNDS list on next open."
         "</small></p></body>");
  s_server.send(200, "text/html", p);
}

void handleUpload() {
  HTTPUpload& up = s_server.upload();
  if (up.status == UPLOAD_FILE_START) {
    s_upName[0] = '\0';
    if (!histSdReady()) return;
    if (!sanitizeName(up.filename, s_upName, sizeof s_upName)) {
      s_upName[0] = '\0';
      return;
    }
    if (!histSdTake(5000)) {  // player mid-clip at worst — brief wait
      s_upName[0] = '\0';
      return;
    }
    s_upLocked = true;
    char path[48];
    snprintf(path, sizeof path, "/sounds/%s", s_upName);
    s_upFile = SD.open(path, FILE_WRITE);  // truncates: replacing is fine
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_upFile) s_upFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_upFile) {
      s_upFile.close();
      snprintf(s_lastMsg, sizeof s_lastMsg, "uploaded %s (%u bytes)", s_upName,
               (unsigned)up.totalSize);
      Serial.printf("[web] %s\n", s_lastMsg);
    }
    if (s_upLocked) {
      histSdGive();
      s_upLocked = false;
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (s_upFile) {
      s_upFile.close();
      char path[48];
      snprintf(path, sizeof path, "/sounds/%s", s_upName);
      SD.remove(path);  // no half-written clips in the picker
      Serial.printf("[web] upload aborted, %s removed\n", s_upName);
    }
    if (s_upLocked) {
      histSdGive();
      s_upLocked = false;
    }
  }
}

void handleUploadDone() {
  if (!s_upName[0] && !s_lastMsg[0])
    snprintf(s_lastMsg, sizeof s_lastMsg, "upload rejected (.mp3 only, card required)");
  s_server.sendHeader("Location", "/");
  s_server.send(303);
}

void handleDelete() {
  char name[36];
  if (s_server.hasArg("f") && sanitizeName(s_server.arg("f"), name, sizeof name) &&
      histSdReady() && histSdTake(2000)) {
    char path[48];
    snprintf(path, sizeof path, "/sounds/%s", name);
    const bool ok = SD.remove(path);
    histSdGive();
    snprintf(s_lastMsg, sizeof s_lastMsg, "%s %s", name, ok ? "deleted" : "not deleted");
    Serial.printf("[web] %s\n", s_lastMsg);
  }
  s_server.sendHeader("Location", "/");
  s_server.send(303);
}

void webTask(void*) {
  for (;;) {
    s_server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(3));
  }
}

}  // namespace

void webUploadStart() {
  if (s_up) return;
  s_server.on("/", HTTP_GET, sendRoot);
  s_server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  s_server.on("/del", HTTP_GET, handleDelete);
  s_server.onNotFound([] { s_server.send(404, "text/plain", "not here"); });
  s_server.begin();
  xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 0);
  s_up = true;
  Serial.printf("[web] sound manager on http://%s/\n",
                WiFi.localIP().toString().c_str());
}

bool webUploadUp() { return s_up; }
