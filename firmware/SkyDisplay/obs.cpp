#include "obs.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <AsyncUDP.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------- HTTP + JSON ----------
struct PsAlloc : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return ps_malloc(n); }
  void deallocate(void* p) override { free(p); }
  void* reallocate(void* p, size_t n) override { return ps_realloc(p, n); }
};
static PsAlloc psAlloc;

// Collects a response body in PSRAM (NINA's sequence JSON can be large).
struct Body : Stream {
  uint8_t* buf = nullptr; size_t len = 0, cap = 0;
  ~Body() { free(buf); }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* p, size_t n) override {
    if (len + n + 1 > cap) {
      size_t nc = max(cap * 2, len + n + 1 + 4096);
      uint8_t* nb = (uint8_t*)ps_realloc(buf, nc); if (!nb) return 0;
      buf = nb; cap = nc;
    }
    memcpy(buf + len, p, n); len += n; buf[len] = 0; return n;
  }
  int available() override { return 0; } int read() override { return -1; } int peek() override { return -1; } void flush() override {}
};

// GET url and parse JSON through filter. Returns the HTTP status (or a negative HTTPClient error).
static int getJson(const String& url, JsonDocument& filter, JsonDocument& doc, int timeoutMs = 3000) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  bool tls = url.startsWith("https://");
  WiFiClient plain; WiFiClientSecure secure;
  if (tls) secure.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(timeoutMs);
  http.setUserAgent("SkyDisplay/" FIRMWARE_VERSION);
  if (!(tls ? http.begin(secure, url) : http.begin(plain, url))) return -1;
  int code = http.GET();
  if (code <= 0) { http.end(); return code; }
  Body body;
  int wrote = http.writeToStream(&body);
  http.end();
  if (wrote < 0 || !body.len) return code == 200 ? -2 : code;
  if (deserializeJson(doc, (const char*)body.buf, body.len, DeserializationOption::Filter(filter))) return code == 200 ? -3 : code;
  return code;
}

// "host[:port][/device]" or "https://host[:port][/device]" -> base URL and device number.
static bool parseAlpaca(String s, String& base, int& dev) {
  s.trim(); if (!s.length()) return false;
  String scheme = "http://";
  int p = s.indexOf("://");
  if (p >= 0) { scheme = s.substring(0, p + 3); s = s.substring(p + 3); }
  dev = 0;
  int slash = s.indexOf('/');
  if (slash >= 0) { dev = s.substring(slash + 1).toInt(); s = s.substring(0, slash); }
  if (s.indexOf(':') < 0) s += scheme == "https://" ? ":443" : ":11111";
  base = scheme + s;
  return true;
}

static uint32_t txn = 1;
// One Alpaca property. Returns true when the device answered; ok false when it returned an Alpaca error.
static bool alpaca(const String& base, const char* type, int dev, const char* prop, JsonDocument& doc, bool& ok, String* err = nullptr, const String& extra = "") {
  JsonDocument filter;
  filter["Value"] = true; filter["ErrorNumber"] = true; filter["ErrorMessage"] = true;
  filter["Comments"]["UnsafeMonitors"][0]["Monitor"] = true;   // Llama Watch: why it's unsafe
  filter["Comments"]["UnsafeMonitors"][0]["Reason"] = true;
  String url = base + "/api/v1/" + type + "/" + String(dev) + "/" + prop + "?ClientID=7&ClientTransactionID=" + String(txn++) + extra;
  int code = getJson(url, filter, doc, 4000);
  if (code != 200 && code != 400 && code != 500) return false;  // no answer
  ok = (doc["ErrorNumber"] | 0) == 0 && !doc["Value"].isNull();
  if (!ok && err) *err = doc["ErrorMessage"] | "";
  return true;
}

// ---------- UDP: park sensors and safety sensors ----------
static const uint16_t PARK_UDP_PORT = 23435;
static const uint32_t UDP_STALE_MS = 20000;  // sensors broadcast every 2 s
struct UdpSensor { String id, name; bool safe = false, isSafety = false; uint32_t lastMs = 0; };
static UdpSensor udpSensors[OBS_MAX_PARK + 4];
static int nUdp = 0;
static portMUX_TYPE udpMux = portMUX_INITIALIZER_UNLOCKED;
static AsyncUDP udp;
static bool udpStarted = false;

static void onUdp(AsyncUDPPacket& pk) {
  JsonDocument doc;
  if (deserializeJson(doc, (const char*)pk.data(), pk.length())) return;
  String id = doc["serialNumber"] | "";
  if (!id.length()) return;
  String type = doc["deviceType"] | "";
  bool isSafety = type == "SafetySensor";
  bool safe = isSafety ? (doc["isSafe"] | false) : (doc["isSafeToMove"] | false);
  String name = doc["name"] | "";
  portENTER_CRITICAL(&udpMux);
  int k = 0;
  while (k < nUdp && udpSensors[k].id != id) k++;
  if (k == nUdp && nUdp < (int)(sizeof udpSensors / sizeof udpSensors[0])) nUdp++;
  if (k < nUdp) {
    UdpSensor& s = udpSensors[k];
    s.id = id; if (name.length()) s.name = name;
    s.safe = safe; s.isSafety = isSafety; s.lastMs = millis();
  }
  portEXIT_CRITICAL(&udpMux);
}

void obsBegin() {
  if (udpStarted) return;
  if (udp.listen(PARK_UDP_PORT)) { udp.onPacket(onUdp); udpStarted = true; Serial.printf("[obs] listening for park/safety sensors on UDP %u\n", PARK_UDP_PORT); }
  else Serial.println("[obs] UDP listen FAILED");
}

static int udpSnapshot(UdpSensor* out, int maxN) {
  portENTER_CRITICAL(&udpMux);
  int n = min(nUdp, maxN);
  for (int i = 0; i < n; i++) out[i] = udpSensors[i];
  portEXIT_CRITICAL(&udpMux);
  return n;
}

// ---------- helpers ----------
// "Error - Timeout: No data received for 259125 seconds (timeout: 300s)" -> "no data for 3 days"
static String shortReason(String monitor, String reason) {
  int paren = monitor.indexOf(" ("); if (paren > 0) monitor = monitor.substring(0, paren);
  if (reason.startsWith("Error - ")) reason = reason.substring(8);
  int p = reason.indexOf("No data received for ");
  if (p >= 0) {
    long secs = reason.substring(p + 21).toInt();
    String age = secs >= 2 * 86400 ? String(secs / 86400) + " days" : secs >= 7200 ? String(secs / 3600) + " h"
               : secs >= 120 ? String(secs / 60) + " min" : String(secs) + " s";
    reason = "no data for " + age;
  }
  int tp = reason.indexOf(" (timeout"); if (tp > 0) reason = reason.substring(0, tp);
  return monitor.length() ? monitor + ": " + reason : reason;
}

// When a finished or lost run stops being shown: the first holdHour (local) after it ended.
static time_t holdUntil(time_t ended, int holdHour) {
  struct tm t; localtime_r(&ended, &t);
  t.tm_hour = holdHour; t.tm_min = 0; t.tm_sec = 0;
  time_t h = mktime(&t);
  if (h <= ended) h += 24 * 3600;
  return h;
}

static String nightLabel(time_t t) {  // "Fri night" for the evening the run started (before noon counts as the previous night)
  t -= 12 * 3600;
  struct tm tmv; localtime_r(&t, &tmv);
  static const char* D[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return String(D[tmv.tm_wday]) + " night";
}

// ---------- NINA ----------
static void pollRig(ObsRig& r, const String& host, int holdHour, time_t now) {
  String h = host; h.trim();
  if (h.indexOf("://") < 0) h = "http://" + h;
  if (h.indexOf(':', 7) < 0) h += ":1888";
  String api = h + "/v2/api";

  JsonDocument f, d;
  f["Success"] = true; f["Response"] = true;
  bool online = getJson(api + "/version", f, d, 2500) == 200 && (d["Success"] | false);
  if (online) {
    r.online = true; r.lastSeen = now; r.error = "";
    JsonDocument fm; JsonObject m = fm["Response"].to<JsonObject>();
    m["Connected"] = m["AtPark"] = m["TrackingEnabled"] = true;
    JsonDocument dm;
    if (getJson(api + "/equipment/mount/info", fm, dm) == 200) {
      r.parked = dm["Response"]["Connected"] | false ? (dm["Response"]["AtPark"] | false) : false;
      r.tracking = dm["Response"]["TrackingEnabled"] | false;
    }
    JsonDocument fc; JsonObject c = fc["Response"].to<JsonObject>();
    c["IsExposing"] = c["CameraState"] = true;
    JsonDocument dc;
    if (getJson(api + "/equipment/camera/info", fc, dc) == 200) {
      String st = dc["Response"]["CameraState"] | "";
      r.exposing = (dc["Response"]["IsExposing"] | false) || st == "Exposing" || st == "Reading" || st == "Download";
    }
    JsonDocument fg; JsonObject g = fg["Response"].to<JsonObject>();
    g["Connected"] = g["State"] = true; g["RMSError"]["Total"]["Arcseconds"] = true;
    JsonDocument dg;
    if (getJson(api + "/equipment/guider/info", fg, dg) == 200) {
      bool gc = dg["Response"]["Connected"] | false;
      r.guider = gc ? String(dg["Response"]["State"] | "") : String("");
      JsonVariantConst a = dg["Response"]["RMSError"]["Total"]["Arcseconds"];
      r.rms = (gc && r.guider == "Guiding" && !a.isNull()) ? a.as<float>() : NAN;
    }
    // Sequence: running if any root container (Start / Imaging / End) is RUNNING.
    JsonDocument fs; fs["Response"][0]["Status"] = true;
    JsonDocument ds;
    int sc = getJson(api + "/sequence/json", fs, ds, 6000);
    bool running = false, failed = false;
    if (sc == 200) for (JsonVariantConst it : ds["Response"].as<JsonArrayConst>()) {
      String st = it["Status"] | ""; st.toUpperCase();
      if (st == "RUNNING") running = true;
      if (st == "FAILED") failed = true;
    }
    r.seqRunning = running;
    // Light frames: count, and the newest one when the count changes.
    JsonDocument fn; fn["Response"] = true;
    JsonDocument dn;
    if (getJson(api + "/image-history?count=true&imageType=LIGHT", fn, dn) == 200) {
      int n = dn["Response"] | 0;
      if (n != r.frames && n > 0) {
        JsonDocument fi; JsonObject im = fi["Response"][0].to<JsonObject>();
        im["TargetName"] = im["Filter"] = im["ExposureTime"] = im["HFR"] = true;
        JsonDocument di;
        if (getJson(api + "/image-history?index=" + String(n - 1) + "&imageType=LIGHT", fi, di) == 200) {
          JsonVariantConst resp = di["Response"];
          JsonVariantConst v = resp.is<JsonArrayConst>() ? resp[0] : resp;
          r.target = v["TargetName"] | ""; r.filter = v["Filter"] | "";
          r.exposure = v["ExposureTime"].isNull() ? NAN : v["ExposureTime"].as<float>();
          r.hfr = v["HFR"].isNull() ? NAN : v["HFR"].as<float>();
        }
        if (n > r.frames) r.lastImage = now;
      }
      r.frames = n;
    }
    if (running) {
      r.session = true; r.endedAt = 0;
      r.state = (r.exposing || r.guider == "Guiding") ? RIG_IMAGING : RIG_WAITING;
    } else if (r.session) {
      if (!r.endedAt) r.endedAt = now;
      r.state = RIG_FINISHED;
    } else r.state = failed ? RIG_ERROR : RIG_IDLE;
    if (failed && !running) r.error = "Sequence failed";
  } else {
    r.online = false;
    if (r.state == RIG_IMAGING || r.state == RIG_WAITING) { r.state = RIG_LOST; r.endedAt = now; }
    else if (r.state == RIG_FINISHED) { /* keep */ }
    else if (r.state != RIG_LOST) r.state = RIG_INACTIVE;
  }
  // Finished or lost runs are shown until the hold hour, then the rig goes back to inactive (or idle).
  if ((r.state == RIG_FINISHED || r.state == RIG_LOST) && r.endedAt && now >= holdUntil(r.endedAt, holdHour)) {
    if (r.target.length()) r.lastRun = "Last run: " + nightLabel(r.endedAt) + ", " + r.target + ", " + String(r.frames) + " frames";
    r.session = false; r.endedAt = 0;
    r.state = r.online ? RIG_IDLE : RIG_INACTIVE;
  }
}

// ---------- one full poll ----------
void obsPoll(Observatory& o, const ObsConfig& c) {
  time_t now = time(nullptr);
  String base; int dev;
  JsonDocument d; bool ok;

  // Roof
  o.roof.configured = parseAlpaca(c.roof, base, dev);
  if (o.roof.configured) {
    String err;
    if (alpaca(base, "dome", dev, "shutterstatus", d, ok, &err)) {
      o.roof.online = true;
      int s = ok ? (d["Value"] | -1) : SHUTTER_ERROR;
      if (o.roof.shutter >= 0 && s != o.roof.shutter) o.roof.since = now;
      o.roof.shutter = s;
      o.roof.error = "";
      if (s == SHUTTER_ERROR) {  // the reason is usually in the next call's error message
        String e2; JsonDocument d2; bool ok2;
        if (!err.length() && alpaca(base, "dome", dev, "slewing", d2, ok2, &e2) && !ok2) err = e2;
        int dot = err.indexOf('.'); if (dot > 0) err = err.substring(0, dot);  // first sentence
        o.roof.error = err.length() ? err : String("Roof reports an error");
      }
      if (!o.roof.name.length()) { JsonDocument dn; bool okn; if (alpaca(base, "dome", dev, "name", dn, okn) && okn) o.roof.name = dn["Value"] | ""; }
    } else o.roof.online = false;
  }

  // Safety verdict
  o.safety.configured = parseAlpaca(c.safety, base, dev);
  if (o.safety.configured) {
    if (alpaca(base, "safetymonitor", dev, "issafe", d, ok)) {
      o.safety.online = ok;
      o.safety.safe = ok && (d["Value"] | false);
      o.safety.reason = "";
      JsonArrayConst um = d["Comments"]["UnsafeMonitors"];
      if (!o.safety.safe && !um.isNull() && um.size())
        o.safety.reason = shortReason(um[0]["Monitor"] | "", um[0]["Reason"] | "") + (um.size() > 1 ? " (+" + String(um.size() - 1) + ")" : "");
    } else { o.safety.online = false; o.safety.safe = false; o.safety.reason = "No answer from the safety monitor"; }
  }

  // Rain: an Alpaca SafetyMonitor, or a UDP safety broadcast
  String rc = c.rain; rc.trim();
  o.rain.configured = rc.length() > 0;
  UdpSensor us[OBS_MAX_PARK + 4]; int nu = udpSnapshot(us, OBS_MAX_PARK + 4);
  if (rc.startsWith("udp")) {
    String want = rc.length() > 4 ? rc.substring(4) : ""; want.trim(); want.toLowerCase();
    o.rain.online = false;
    for (int i = 0; i < nu; i++) {
      if (!us[i].isSafety) continue;
      String n = us[i].name; n.toLowerCase(); String id = us[i].id; id.toLowerCase();
      if (want.length() && n.indexOf(want) < 0 && id.indexOf(want) < 0) continue;
      if (millis() - us[i].lastMs > UDP_STALE_MS) continue;
      o.rain.online = true; o.rain.wet = !us[i].safe; break;
    }
  } else if (o.rain.configured && parseAlpaca(rc, base, dev)) {
    o.rain.online = alpaca(base, "safetymonitor", dev, "issafe", d, ok) && ok;
    o.rain.wet = o.rain.online && !(d["Value"] | true);
  }

  // Sky conditions
  o.sky.configured = parseAlpaca(c.sky, base, dev);
  if (o.sky.configured) {
    auto get = [&](const char* prop) -> float { JsonDocument dd; bool okk; return (alpaca(base, "observingconditions", dev, prop, dd, okk) && okk) ? dd["Value"].as<float>() : NAN; };
    o.sky.cloud = get("cloudcover");
    o.sky.online = !isnan(o.sky.cloud);
    if (o.sky.online) {
      o.sky.sqm = get("skyquality"); o.sky.lux = get("skybrightness");
      o.sky.skyT = get("skytemperature"); o.sky.ambient = get("temperature");
    } else o.sky.sqm = o.sky.lux = o.sky.skyT = o.sky.ambient = NAN;
  }

  // Park sensors. Each rig can name the park sensor on its pier: one heard on the UDP broadcast (by name or serial),
  // or an Alpaca Switch address (anything with a dot, colon or "://"). A sensor tied to a rig is shown in that rig's
  // row; the rest are listed on their own, sorted by name.
  int pier[OBS_MAX_RIGS];
  bool used[OBS_MAX_PARK + 4] = {};
  for (int i = 0; i < OBS_MAX_RIGS; i++) {
    pier[i] = PIER_NONE;
    String a = c.rigPark[i]; a.trim();
    if (!a.length() || !c.rigHost[i].length()) continue;
    pier[i] = PIER_OFFLINE;  // until it's heard from
    bool address = a.indexOf("://") >= 0 || ((a.indexOf('.') >= 0 || a.indexOf(':') >= 0) && a.indexOf(' ') < 0);
    if (address) {
      int sw = 0, hash = a.indexOf('#');
      if (hash >= 0) { sw = a.substring(hash + 1).toInt(); a = a.substring(0, hash); }
      JsonDocument dp; bool okp;
      if (parseAlpaca(a, base, dev) && alpaca(base, "switch", dev, "getswitch", dp, okp, nullptr, "&Id=" + String(sw)) && okp)
        pier[i] = (dp["Value"] | false) ? PIER_PARKED : PIER_UNPARKED;  // switch on = parked
      continue;
    }
    String want = a; want.toLowerCase();
    int best = -1;
    for (int k = 0; k < nu; k++) {
      if (us[k].isSafety) continue;
      String n = us[k].name, id = us[k].id; n.toLowerCase(); id.toLowerCase();
      if (n == want || id == want) { best = k; break; }          // exact name or serial
      if (best < 0 && n.indexOf(want) >= 0) best = k;             // else the first name containing it
    }
    if (best < 0) continue;
    used[best] = true;
    if (millis() - us[best].lastMs <= UDP_STALE_MS) pier[i] = us[best].safe ? PIER_PARKED : PIER_UNPARKED;
  }
  o.nPark = 0;
  for (int k = 0; k < nu && o.nPark < OBS_MAX_PARK; k++) {
    if (us[k].isSafety || used[k]) continue;
    ObsPark& p = o.park[o.nPark++];
    p.id = us[k].id; p.name = us[k].name.length() ? us[k].name : us[k].id;
    p.parked = us[k].safe; p.online = millis() - us[k].lastMs <= UDP_STALE_MS;
  }
  for (int i = 1; i < o.nPark; i++) for (int j = i; j > 0 && o.park[j].name < o.park[j - 1].name; j--) std::swap(o.park[j], o.park[j - 1]);

  // Rigs (keep each rig's remembered state when the list is unchanged)
  int n = 0;
  for (int i = 0; i < OBS_MAX_RIGS; i++) {
    String host = c.rigHost[i]; host.trim();
    if (!host.length()) continue;
    ObsRig& r = o.rigs[n];
    String name = c.rigName[i].length() ? c.rigName[i] : String("Rig ") + String(i + 1);
    if (r.name != name) { r = ObsRig(); r.name = name; }
    r.configured = true;
    r.pier = pier[i];
    pollRig(r, host, c.holdHour, now);
    n++;
  }
  for (int i = n; i < OBS_MAX_RIGS; i++) o.rigs[i] = ObsRig();
  o.nRigs = n;
  o.polled = now;
}

int obsHeardParkSensors(String* names, int maxN) {
  UdpSensor us[OBS_MAX_PARK + 4]; int nu = udpSnapshot(us, OBS_MAX_PARK + 4), n = 0;
  for (int k = 0; k < nu && n < maxN; k++) if (!us[k].isSafety) names[n++] = us[k].name.length() ? us[k].name : us[k].id;
  return n;
}

// ---------- Alpaca discovery (settings page) ----------
String obsDiscoverJson() {
  WiFiUDP u;
  JsonDocument out; JsonArray arr = out.to<JsonArray>();
  if (!u.begin(0)) return "[]";
  IPAddress bc = WiFi.broadcastIP();
  for (IPAddress dst : {bc, IPAddress(255, 255, 255, 255)}) {
    u.beginPacket(dst, 32227); u.print("alpacadiscovery1"); u.endPacket();
  }
  struct Hit { IPAddress ip; int port; } hits[16]; int nh = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 1500) {
    int sz = u.parsePacket();
    if (sz <= 0) { delay(10); continue; }
    char buf[128]; int len = u.read(buf, sizeof buf - 1); buf[max(0, len)] = 0;
    JsonDocument d; if (deserializeJson(d, buf)) continue;
    int port = d["AlpacaPort"] | 0; if (!port) continue;
    IPAddress ip = u.remoteIP();
    bool dup = false; for (int i = 0; i < nh; i++) if (hits[i].ip == ip && hits[i].port == port) dup = true;
    if (!dup && nh < 16) hits[nh++] = {ip, port};
  }
  u.stop();
  for (int i = 0; i < nh; i++) {
    String base = "http://" + hits[i].ip.toString() + ":" + String(hits[i].port);
    JsonDocument f; f["Value"][0]["DeviceName"] = true; f["Value"][0]["DeviceType"] = true; f["Value"][0]["DeviceNumber"] = true;
    JsonDocument d;
    if (getJson(base + "/management/v1/configureddevices?ClientID=7&ClientTransactionID=1", f, d, 2500) != 200) continue;
    for (JsonVariantConst v : d["Value"].as<JsonArrayConst>()) {
      JsonObject o = arr.add<JsonObject>();
      String addr = hits[i].ip.toString() + (hits[i].port == 11111 ? String("") : ":" + String(hits[i].port));
      int num = v["DeviceNumber"] | 0;
      if (num) addr += "/" + String(num);
      o["addr"] = addr; o["type"] = v["DeviceType"] | ""; o["name"] = v["DeviceName"] | "";
    }
  }
  String s; serializeJson(out, s);
  return s;
}
