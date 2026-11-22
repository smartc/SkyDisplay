#include "po.h"
#include "settings.h"
#include "config.h"
#include "board.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
// Messages are kept in a file on the board's data partition: FAT on the WT32 (its "ffat" partition),
// LittleFS on the Indicator (its "spiffs" partition). Written only when messages arrive or are acknowledged.
#if defined(BOARD_INDICATOR)
  #include <LittleFS.h>
  #define PO_FS LittleFS
#else
  #include <FFat.h>
  #define PO_FS FFat
#endif

PoInbox poInbox;

static const char* API = "https://api.pushover.net/1/";
static const char* UA = "SkyDisplay/" FIRMWARE_VERSION " (ESP32-S3; unofficial Open Client)";
static const char* FILE_PATH = "/notify.bin";
static const uint32_t FILE_MAGIC = 0x314E4F50;  // "PON1"
static const uint32_t WS_QUIET_MS = 180000;
// While an emergency waits for acknowledgement, download this often: Pushover sends no signal when it's acknowledged
// on another device (e.g. the phone), but a download then returns it with acked 1, which stops the alarm here.
static const uint32_t EMERGENCY_POLL_MS = 15000;     // no frame (Pushover sends '#' keep-alives) for this long: reconnect

static WebSocketsClient ws;
static volatile bool syncReq = false, restartReq = true, stopped = false;
static uint64_t ackId = 0;  // message to acknowledge (set from the main loop on a tap; under the data lock)
static bool fsMounted = false;

static void setState(PoState s, const String& err) {
  DataGuard g;
  poInbox.state = s; poInbox.error = err;
}

// ---------- text: plain ASCII plus the few symbols the display fonts have ----------
// Strips HTML when the message is HTML, turns common Unicode punctuation into ASCII and drops the rest (emoji).
static void cleanText(const char* in, bool html, char* out, size_t sz) {
  size_t o = 0;
  auto put = [&](const char* s) { size_t l = strlen(s); if (o + l < sz) { memcpy(out + o, s, l); o += l; } };
  auto putc1 = [&](char c) { if (c == ' ' && (o == 0 || out[o - 1] == ' ' || out[o - 1] == '\n')) return; if (o + 1 < sz) out[o++] = c; };
  for (const uint8_t* p = (const uint8_t*)in; *p && o + 1 < sz; ) {
    uint8_t c = *p;
    if (html && c == '<') {  // tag: <br> and </p> become line breaks, others vanish
      const char* t = (const char*)p + 1;
      bool brk = !strncasecmp(t, "br", 2) || !strncasecmp(t, "/p", 2) || !strncasecmp(t, "/div", 4) || !strncasecmp(t, "li", 2);
      while (*p && *p != '>') p++;
      if (*p) p++;
      if (brk && o && out[o - 1] != '\n') { while (o && out[o - 1] == ' ') o--; putc1('\n'); }
      continue;
    }
    if (html && c == '&') {
      static const struct { const char* e; const char* r; } ENT[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
                                                                    {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "}};
      bool done = false;
      for (auto& e : ENT) if (!strncmp((const char*)p, e.e, strlen(e.e))) { put(e.r); p += strlen(e.e); done = true; break; }
      if (done) continue;
    }
    if (c < 0x80) {
      if (c == '\r') {}
      else if (c == '\n') { while (o && out[o - 1] == ' ') o--; if (o + 1 < sz) out[o++] = '\n'; }
      else if (c == '\t') putc1(' ');
      else if (c >= 0x20) putc1((char)c);
      p++; continue;
    }
    // UTF-8 sequence: decode the code point
    int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    uint32_t cp = len == 4 ? (c & 7) : len == 3 ? (c & 15) : (c & 31);
    bool bad = len == 1;
    for (int i = 1; i < len; i++) { if ((p[i] & 0xC0) != 0x80) { bad = true; len = i; break; } cp = (cp << 6) | (p[i] & 63); }
    p += len;
    if (bad) continue;
    switch (cp) {
      case 0x2018: case 0x2019: case 0x201B: case 0x2032: putc1('\''); break;
      case 0x201C: case 0x201D: case 0x2033: putc1('"'); break;
      case 0x2013: case 0x2014: case 0x2212: put("\xE2\x80\x93"); break;  // en dash (in the fonts)
      case 0x2026: put("..."); break;
      case 0x00A0: case 0x2009: case 0x202F: putc1(' '); break;
      case 0x00B0: put("\xC2\xB0"); break;
      case 0x00B7: case 0x2022: put("\xC2\xB7"); break;
      case 0x2264: put("\xE2\x89\xA4"); break;
      case 0x2265: put("\xE2\x89\xA5"); break;
      default: break;  // no glyph: leave it out
    }
  }
  while (o && (out[o - 1] == ' ' || out[o - 1] == '\n')) o--;
  out[o] = 0;
}

// ---------- storage ----------
static bool mountFs() {
  if (fsMounted) return true;
  fsMounted = PO_FS.begin(true);  // formats on first use (the partition is otherwise unused)
  if (!fsMounted) logf("[po] storage mount FAILED\n");
  return fsMounted;
}

static void saveInbox() {
  if (!mountFs()) return;
  // Copy under the lock, write outside it so the screen doesn't wait.
  int n; PoMsg* copy = (PoMsg*)ps_malloc(sizeof(PoMsg) * PO_MAX);
  if (!copy) return;
  { DataGuard g; n = poInbox.n; memcpy(copy, poInbox.msg, sizeof(PoMsg) * n); }
  File f = PO_FS.open(FILE_PATH, "w");
  if (f) {
    uint32_t hdr[3] = {FILE_MAGIC, (uint32_t)sizeof(PoMsg), (uint32_t)n};
    f.write((const uint8_t*)hdr, sizeof hdr);
    f.write((const uint8_t*)copy, sizeof(PoMsg) * n);
    f.close();
  }
  free(copy);
}

static void loadInbox() {
  if (!mountFs()) return;
  File f = PO_FS.open(FILE_PATH, "r");
  if (!f) return;
  uint32_t hdr[3] = {0, 0, 0};
  if (f.read((uint8_t*)hdr, sizeof hdr) == sizeof hdr && hdr[0] == FILE_MAGIC && hdr[1] == sizeof(PoMsg) && hdr[2] <= (uint32_t)PO_MAX) {
    PoMsg* tmp = (PoMsg*)ps_malloc(sizeof(PoMsg) * PO_MAX);
    if (tmp && f.read((uint8_t*)tmp, sizeof(PoMsg) * hdr[2]) == sizeof(PoMsg) * hdr[2]) {
      DataGuard g;
      memcpy(poInbox.msg, tmp, sizeof(PoMsg) * hdr[2]);
      poInbox.n = hdr[2];
      logf("[po] %d saved messages\n", poInbox.n);
    }
    free(tmp);
  }
  f.close();
}

// ---------- HTTPS ----------
static String urlEnc(const String& s) {
  String o; o.reserve(s.length() * 3);
  for (char c : s) {
    if (isalnum((uint8_t)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { char b[4]; snprintf(b, sizeof b, "%%%02X", (uint8_t)c); o += b; }
  }
  return o;
}

// One request; returns the HTTP code (or a negative HTTPClient error) and the body. Secrets are never logged.
// take = false for the settings page's link request: the web handler holds the data lock, and the HTTPS lock must
// never be taken while holding it.
static int request(const char* method, const String& path, const String& form, String& body, bool take = true) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  if (take) httpsLock();
  struct Release { bool on; ~Release() { if (on) httpsUnlock(); } } release{take};
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(15000);
  HTTPClient http;
  http.setUserAgent(UA);
  http.setTimeout(15000);
  if (!http.begin(client, String(API) + path)) return -1;
  int code;
  if (!strcmp(method, "POST")) { http.addHeader("Content-Type", "application/x-www-form-urlencoded"); code = http.POST(form); }
  else code = http.GET();
  body = code > 0 ? http.getString() : String();
  http.end();
  return code;
}

// Pushover's error text from a failed reply: the first entry of "errors" (an array, or an object of arrays).
static String apiError(const String& body, int code) {
  JsonDocument d;
  if (!deserializeJson(d, body)) {
    JsonVariantConst e = d["errors"];
    if (e.is<JsonArrayConst>() && e.size()) return e[0].as<String>();
    if (e.is<JsonObjectConst>()) for (JsonPairConst kv : e.as<JsonObjectConst>()) return String(kv.key().c_str()) + " " + (kv.value()[0] | "is not valid");
  }
  return code > 0 ? "HTTP " + String(code) : String("no answer");
}

// Download new messages, keep them, then delete them from Pushover (they're ours now).
// Returns false on a 4xx answer (secret no longer valid): those aren't retried.
static bool sync(const String& secret, const String& device) {
  String body;
  int code = request("GET", "messages.json?secret=" + urlEnc(secret) + "&device_id=" + urlEnc(device), "", body);
  if (code >= 400 && code < 500) { setState(PO_STOPPED, "Pushover: " + apiError(body, code) + ". Link the display again."); return false; }
  if (code != 200) { logf("[po] download failed (%d)\n", code); return true; }
  JsonDocument d;
  if (deserializeJson(d, body)) { logf("[po] download: bad JSON\n"); return true; }
  JsonArrayConst arr = d["messages"];
  if (arr.size()) logf("[po] download: %u message(s)\n", (unsigned)arr.size());
  uint64_t highest = 0; int added = 0; int8_t topPri = -2;
  bool ackedElsewhere = false;
  {
    DataGuard g;
    for (JsonVariantConst m : arr) {
      // Ids don't fit 32 bits; id_str is the exact value as text.
      const char* ids = m["id_str"] | "";
      uint64_t id = *ids ? strtoull(ids, nullptr, 10) : m["id"].as<uint64_t>();
      if (!id) continue;
      if (id > highest) highest = id;
      bool dup = false;
      const char* rc = m["receipt"] | "";
      int ackedFlag = m["acked"] | 0;
      logf("[po]   id %llu, priority %d, acked %d, receipt %.6s\n", (unsigned long long)id, (int)(m["priority"] | 0), ackedFlag, rc);
      // An emergency message comes again every retry period (same id) until acknowledged: keep the one we have,
      // no new row. Once it's acknowledged anywhere (e.g. on the phone), it comes once more with acked 1 and no
      // receipt; take that over, which also stops the alarm here.
      for (int i = 0; i < poInbox.n; i++) {
        bool same = poInbox.msg[i].id == id || (*rc && !strcmp(poInbox.msg[i].receipt, rc));
        if (!same) continue;
        dup = true;
        if (ackedFlag == 1 && poInbox.msg[i].acked != 1) { poInbox.msg[i].acked = 1; poInbox.changes++; ackedElsewhere = true; }
        break;
      }
      if (dup) continue;
      PoMsg x;
      x.id = id; x.date = m["date"] | 0; x.priority = m["priority"] | 0; x.acked = m["acked"] | 0;
      cleanText(m["app"] | "", false, x.app, sizeof x.app);
      cleanText(m["title"] | "", false, x.title, sizeof x.title);
      cleanText(m["message"] | "", (m["html"] | 0) == 1, x.text, sizeof x.text);
      strlcpy(x.receipt, m["receipt"] | "", sizeof x.receipt);
      // Newest first. The download lists oldest first, so each one goes to the front.
      memmove(&poInbox.msg[1], &poInbox.msg[0], sizeof(PoMsg) * min(poInbox.n, PO_MAX - 1));
      poInbox.msg[0] = x;
      if (poInbox.n < PO_MAX) poInbox.n++;
      added++;
      // Only fresh messages can wake the screen: a backlog (e.g. after notifications were turned off while away)
      // arrives quietly, just counted as unread.
      time_t now = time(nullptr);
      bool fresh = x.date && now - (time_t)x.date < 15 * 60;
      // One already acknowledged elsewhere (it can arrive that way, e.g. after a restart) never wakes or sounds.
      if (fresh && !(x.priority >= 2 && x.acked == 1) && x.priority > topPri) topPri = x.priority;
    }
    if (added) { poInbox.unread = min(poInbox.unread + added, PO_MAX); poInbox.arrivals++; poInbox.changes++; poInbox.newestPriority = topPri; }
  }
  if (ackedElsewhere) logf("[po] acknowledged elsewhere\n");
  if (added || ackedElsewhere) { if (added) logf("[po] %d new message(s)\n", added); saveInbox(); }
  if (highest) {
    char hs[24]; snprintf(hs, sizeof hs, "%llu", (unsigned long long)highest);
    code = request("POST", "devices/" + urlEnc(device) + "/update_highest_message.json", "secret=" + urlEnc(secret) + "&message=" + hs, body);
    if (code != 200) logf("[po] delete on server failed (%d); they'll be skipped next time\n", code);
  }
  return true;
}

static void acknowledge(const String& secret, uint64_t id) {
  String receipt;
  { DataGuard g; for (int i = 0; i < poInbox.n; i++) if (poInbox.msg[i].id == id) receipt = poInbox.msg[i].receipt; }
  if (!receipt.length()) return;
  String body;
  int code = request("POST", "receipts/" + urlEnc(receipt) + "/acknowledge.json", "secret=" + urlEnc(secret), body);
  logf("[po] acknowledge: %s\n", code == 200 ? "done" : apiError(body, code).c_str());
  // A 4xx usually means it was acknowledged elsewhere or has expired: done either way. Otherwise (no answer)
  // the button comes back so it can be tapped again.
  bool done = code == 200 || (code >= 400 && code < 500);
  { DataGuard g; for (int i = 0; i < poInbox.n; i++) if (poInbox.msg[i].id == id) poInbox.msg[i].acked = done ? 1 : 0; poInbox.changes++; }
  if (done) saveInbox();
}

// ---------- WebSocket ----------
static String wsLogin;
static volatile uint32_t lastFrameMs = 0;

static void onWs(WStype_t type, uint8_t* data, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      lastFrameMs = millis();
      ws.sendTXT(wsLogin);
      setState(PO_CONNECTED, "");
      syncReq = true;  // anything that arrived while we were away
      logf("[po] connected\n");
      break;
    case WStype_DISCONNECTED:
      if (!stopped) setState(PO_RETRYING, "");
      break;
    case WStype_TEXT: case WStype_BIN:
      lastFrameMs = millis();
      if (len && !(len == 1 && data[0] == '#')) logf("[po] frame '%.*s'\n", (int)min(len, (size_t)16), (const char*)data);  // diagnostics
      for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '!') syncReq = true;
        else if (c == 'R') { logf("[po] server asked to reconnect\n"); ws.disconnect(); }
        else if (c == 'E') { stopped = true; setState(PO_STOPPED, "Pushover refused the login. Link the display again."); }
        else if (c == 'A') { stopped = true; setState(PO_STOPPED, "Logged in from another session. Link the display again."); }
      }
      break;
    default: break;
  }
}

static void poTask(void*) {
  bool wsOn = false;
  bool loaded = false;
  uint32_t nextSyncRetry = 0, nextEmergencyPoll = 0;
  for (;;) {
    bool enabled, listen; String secret, device;
    { DataGuard g; enabled = cfg.poEnabled; listen = cfg.poListen; secret = cfg.poSecret; device = cfg.poDevice; }
    bool linked = secret.length() && device.length();
    if (restartReq) {
      restartReq = false; stopped = false;
      if (wsOn) { ws.disconnect(); wsOn = false; }
    }
    if (!enabled || !linked) {
      if (wsOn) { ws.disconnect(); wsOn = false; }
      setState(enabled ? PO_NOT_LINKED : PO_OFF, "");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!loaded) { loadInbox(); loaded = true; }
    if (!listen) {  // monitoring off: disconnected, messages wait on Pushover's servers
      if (wsOn) { ws.disconnect(); wsOn = false; }
      setState(PO_PAUSED, "");
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (stopped) { if (wsOn) { ws.disconnect(); wsOn = false; } vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    if (WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

    if (!wsOn) {
      setState(PO_CONNECTING, "");
      wsLogin = "login:" + device + ":" + secret + "\n";
      ws.onEvent(onWs);
      ws.setReconnectInterval(30000);  // after a drop, wait before reconnecting
      ws.beginSSL("client.pushover.net", 443, "/push", "", "");
      wsOn = true; lastFrameMs = millis();
    }
    ws.loop();
    if (wsOn && poInbox.state == PO_CONNECTED && millis() - lastFrameMs > WS_QUIET_MS) {
      logf("[po] no keep-alive for 3 min, reconnecting\n");
      ws.disconnect(); lastFrameMs = millis();
    }
    if ((int32_t)(millis() - nextEmergencyPoll) >= 0) {
      nextEmergencyPoll = millis() + EMERGENCY_POLL_MS;
      DataGuard g;
      for (int i = 0; i < poInbox.n; i++) if (poInbox.msg[i].priority >= 2 && poInbox.msg[i].acked == 0) { syncReq = true; break; }
    }
    if (syncReq && (int32_t)(millis() - nextSyncRetry) >= 0) {
      syncReq = false;
      if (!sync(secret, device)) { stopped = true; continue; }
      nextSyncRetry = millis() + 5000;  // Pushover: no sooner than 5 s between retries
    }
    uint64_t ack; { DataGuard g; ack = ackId; ackId = 0; }
    if (ack) acknowledge(secret, ack);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------- public ----------
void poBegin() {
  xTaskCreatePinnedToCore(poTask, "po", 12288, nullptr, 1, nullptr, 0);
}
void poSyncNow() { syncReq = true; }
void poRestart() { restartReq = true; }
void poAcknowledge(int index) {
  DataGuard g;
  if (index >= 0 && index < poInbox.n && poInbox.msg[index].priority >= 2 && poInbox.msg[index].acked == 0) {
    ackId = poInbox.msg[index].id;
    poInbox.msg[index].acked = 2;  // shown at once as "Acknowledging..."
    poInbox.changes++;
  }
}
void poDelete(int index) {
  {
    DataGuard g;
    if (index < 0 || index >= poInbox.n) return;
    memmove(&poInbox.msg[index], &poInbox.msg[index + 1], sizeof(PoMsg) * (poInbox.n - index - 1));
    poInbox.n--;
    poInbox.unread = min(poInbox.unread, poInbox.n);
    poInbox.changes++;
  }
  saveInbox();
}
void poClear() {
  { DataGuard g; poInbox.n = 0; poInbox.unread = 0; poInbox.changes++; }
  saveInbox();
}

String poLink(const String& email, const String& password, const String& twofa, const String& deviceName) {
  String body, form = "email=" + urlEnc(email) + "&password=" + urlEnc(password);
  if (twofa.length()) form += "&twofa=" + urlEnc(twofa);
  int code = request("POST", "users/login.json", form, body, false);
  if (code == 412) return "2fa";
  JsonDocument d;
  if (code != 200 || deserializeJson(d, body) || (d["status"] | 0) != 1) return "Login failed: " + apiError(body, code);
  String secret = d["secret"] | "";
  if (!secret.length()) return "Login failed: no session secret";
  code = request("POST", "devices.json", "secret=" + urlEnc(secret) + "&name=" + urlEnc(deviceName) + "&os=O", body, false);
  d.clear();
  if (code != 200 || deserializeJson(d, body) || (d["status"] | 0) != 1) return "Device registration failed: " + apiError(body, code);
  String id = d["id"] | "";
  if (!id.length()) return "Device registration failed: no device id";
  cfg.poSecret = secret; cfg.poDevice = id; cfg.poDeviceName = deviceName;
  settingsSave();
  poRestart();
  logf("[po] linked as device '%s'\n", deviceName.c_str());
  return "";
}

void poListenChanged() {
  cfg.poListenUntil = 0;
  time_t now = time(nullptr);
  if (!cfg.poListen || !cfg.poAuto || now < 1700000000) return;
  struct tm t; localtime_r(&now, &t);
  t.tm_hour = cfg.poAutoOff; t.tm_min = 0; t.tm_sec = 0; t.tm_isdst = -1;
  time_t at = mktime(&t);
  while (at < now + 3 * 3600) { t.tm_mday++; t.tm_isdst = -1; at = mktime(&t); }  // e.g. switched on at 10:30: noon tomorrow
  cfg.poListenUntil = (uint32_t)at;
}

void poUnlink() {
  cfg.poSecret = ""; cfg.poDevice = "";
  settingsSave();
  poRestart();
}

String poStateText() {
  switch (poInbox.state) {
    case PO_OFF: return "Off";
    case PO_NOT_LINKED: return "Not linked";
    case PO_PAUSED: return "Monitoring off";
    case PO_CONNECTING: return "Connecting";
    case PO_CONNECTED: return "Connected";
    case PO_RETRYING: return "Reconnecting";
    case PO_STOPPED: return poInbox.error.length() ? poInbox.error : String("Stopped");
  }
  return "";
}
