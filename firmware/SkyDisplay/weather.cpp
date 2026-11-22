#include "weather.h"
#include "settings.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Big responses (24 h history is ~200 KB) are parsed straight from the stream through a
// filter, with the parsed document in PSRAM.
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return ps_malloc(n); }
  void deallocate(void* p) override { free(p); }
  void* reallocate(void* p, size_t n) override { return ps_realloc(p, n); }
};
static PsramAllocator psAlloc;

bool weatherUsesEcowitt() { return cfg.ecowittHost.length() > 0; }
bool weatherConfigured() { return weatherUsesEcowitt() || (cfg.wuStation.length() > 0 && cfg.wuKey.length() > 0); }
bool weatherHasForecast() { return cfg.wuKey.length() > 0; }

// ---------- 24 h trace kept on the device: hourly averages in a ring of 24 slots ----------
static int slotFor(Weather& w, uint32_t hour) {
  int s = hour % WX_TRACE;
  if (w.slotHour[s] != hour) {  // a new hour: start the slot afresh
    w.slotHour[s] = hour; w.slotT[s] = w.slotD[s] = w.slotP[s] = 0; w.slotNT[s] = w.slotND[s] = w.slotNP[s] = 0;
  }
  return s;
}
static void addSample(Weather& w, time_t t, float temp, float dew, float pres) {
  if (t < 1700000000) return;
  int s = slotFor(w, (uint32_t)(t / 3600));
  if (!isnan(temp)) { w.slotT[s] += temp; w.slotNT[s]++; }
  if (!isnan(dew))  { w.slotD[s] += dew;  w.slotND[s]++; }
  if (!isnan(pres)) { w.slotP[s] += pres; w.slotNP[s]++; }
}
static float slotAvg(const Weather& w, uint32_t hour, const float* sum, const uint16_t* n) {
  int s = hour % WX_TRACE;
  return (w.slotHour[s] == hour && n[s]) ? sum[s] / n[s] : NAN;
}
// Rebuild traceT/traceD (oldest first) and the 1 h / 3 h changes from the slots.
static void rebuildTrace(Weather& w, time_t now) {
  uint32_t h = (uint32_t)(now / 3600);
  bool any = false;
  for (int i = 0; i < WX_TRACE; i++) {
    uint32_t hr = h - (WX_TRACE - 1) + i;
    w.traceT[i] = slotAvg(w, hr, w.slotT, w.slotNT);
    w.traceD[i] = slotAvg(w, hr, w.slotD, w.slotND);
    if (!isnan(w.traceT[i])) any = true;
  }
  float t1 = slotAvg(w, h - 1, w.slotT, w.slotNT), p3 = slotAvg(w, h - 3, w.slotP, w.slotNP);
  w.change1h = (!isnan(t1) && !isnan(w.temp)) ? w.temp - t1 : NAN;
  w.pressure3h = (!isnan(p3) && !isnan(w.pressure)) ? w.pressure - p3 : NAN;
  w.haveHistory = any;
}

// ---------- EcoWitt gateway: GET http://<gateway>/get_livedata_info ----------
// Values are strings with units that follow the gateway's display settings ("18.1" + unit "F", "0.00 mph",
// "30.22 inHg"), so each is converted to metric from the unit it arrives with.
static bool numUnit(JsonVariantConst item, const char* key, float& v, String& unit) {
  String raw = item[key] | "";
  raw.trim();
  if (!raw.length() || raw.startsWith("--")) return false;
  int i = 0;
  while (i < (int)raw.length() && (isdigit((unsigned char)raw[i]) || raw[i] == '.' || raw[i] == '-' || raw[i] == '+')) i++;
  if (!i) return false;
  v = raw.substring(0, i).toFloat();
  unit = raw.substring(i); unit.trim();
  if (!unit.length()) unit = item["unit"] | "";
  unit.replace("\xC2\xB0", ""); unit.toLowerCase();
  return true;
}
static float toC(float v, const String& u) { return (u == "f" || u.endsWith("f")) ? (v - 32) * 5 / 9 : v; }
static float toKmh(float v, const String& u) {
  if (u == "mph") return v * 1.609344f;
  if (u == "m/s") return v * 3.6f;
  if (u.startsWith("knot") || u == "kn") return v * 1.852f;
  if (u == "ft/s") return v * 1.09728f;
  return v;  // km/h
}
static float toHpa(float v, const String& u) {
  if (u == "inhg") return v * 33.8639f;
  if (u == "kpa") return v * 10;
  if (u == "mmhg") return v * 1.33322f;
  return v;  // hPa / mbar
}
static float toMm(float v, const String& u) { return u.startsWith("in") ? v * 25.4f : v; }

static bool fetchEcowitt(Weather& w, String host) {
  if (WiFi.status() != WL_CONNECTED) { w.error = "no Wi-Fi"; return false; }
  host.trim();
  if (host.indexOf("://") < 0) host = "http://" + host;
  while (host.endsWith("/")) host.remove(host.length() - 1);
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(4000);
  if (!http.begin(client, host + "/get_livedata_info")) { w.error = "EcoWitt: bad address"; return false; }
  int code = http.GET();
  if (code != 200) { http.end(); w.error = code > 0 ? "EcoWitt: HTTP " + String(code) : String("EcoWitt: no answer"); return false; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) { w.error = "EcoWitt: unexpected response"; return false; }
  JsonArrayConst common = doc["common_list"];
  if (common.isNull()) { w.error = "EcoWitt: no live data"; return false; }
  float temp = NAN, feels = NAN, dew = NAN, wind = NAN, gust = NAN, pres = NAN, rain = NAN; int hum = -1, dir = -1;
  for (JsonVariantConst it : common) {
    String id = it["id"] | ""; id.toLowerCase();
    float v; String u;
    if (!numUnit(it, "val", v, u)) continue;
    if (id == "0x02") temp = toC(v, u);
    else if (id == "3") feels = toC(v, u);
    else if (id == "0x03") dew = toC(v, u);
    else if (id == "0x07") hum = (int)lroundf(v);
    else if (id == "0x0b") wind = toKmh(v, u);
    else if (id == "0x0c") gust = toKmh(v, u);
    else if (id == "0x0a") dir = ((int)lroundf(v)) % 360;
  }
  for (const char* list : {"rain", "piezoRain"}) {  // tipping bucket first, then a piezo gauge (WS90)
    if (!isnan(rain)) break;
    for (JsonVariantConst it : doc[list].as<JsonArrayConst>()) {
      String id = it["id"] | ""; id.toLowerCase();
      float v; String u;
      if (id == "0x10" && numUnit(it, "val", v, u)) { rain = toMm(v, u); break; }  // rain today
    }
  }
  { float v; String u; JsonVariantConst wh = doc["wh25"][0]; if (numUnit(wh, "rel", v, u)) pres = toHpa(v, u); }
  if (isnan(temp)) { w.error = "EcoWitt: no outdoor temperature"; return false; }
  w.temp = temp; w.dew = dew; w.humidity = hum; w.wind = wind; w.gust = gust; w.windDir = dir;
  w.pressure = pres; w.rainToday = rain;
  w.feels = isnan(feels) ? temp : feels;
  w.obsUtc = time(nullptr);
  w.haveCurrent = true;
  w.source = "EcoWitt";
  return true;
}

// A Stream that collects whatever is written to it in a growing PSRAM buffer.
struct PsramSink : Stream {
  uint8_t* buf = nullptr; size_t len = 0, cap = 0;
  ~PsramSink() { free(buf); }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* p, size_t n) override {
    if (len + n + 1 > cap) {
      size_t nc = max(cap * 2, len + n + 1 + 16384);
      uint8_t* nb = (uint8_t*)ps_realloc(buf, nc); if (!nb) return 0;
      buf = nb; cap = nc;
    }
    memcpy(buf + len, p, n); len += n; buf[len] = 0; return n;
  }
  int available() override { return 0; } int read() override { return -1; } int peek() override { return -1; } void flush() override {}
};

// GET url (whole body, chunked transfer handled by HTTPClient) and parse JSON through filter.
// The URL contains the API key, so it is never logged.
static bool getJson(const String& url, JsonDocument& filter, JsonDocument& doc, String& err, const char* what) {
  HttpsGuard tls;
  if (WiFi.status() != WL_CONNECTED) { err = "no Wi-Fi"; return false; }
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(20000);  // milliseconds (Stream::setTimeout)
  HTTPClient http;
  http.setUserAgent("SkyDisplay/" FIRMWARE_VERSION);
  http.setTimeout(20000);
  const char* keys[] = {"Content-Encoding", "Transfer-Encoding", "Content-Length"};
  http.collectHeaders(keys, 3);
  // HTTP/1.0: no chunked transfer, so the body can be read straight off the connection below.
  http.useHTTP10(true);
  if (!http.begin(client, url)) { err = String(what) + ": begin failed"; return false; }
  http.addHeader("Accept-Encoding", "identity");
  int code = http.GET();
  if (code == 204) { err = String(what) + ": no data (station offline?)"; http.end(); return false; }
  if (code == 401) { err = "weather: API key rejected"; http.end(); return false; }
  if (code == 404) { err = "weather: station not found"; http.end(); return false; }
  if (code != 200) { err = String(what) + ": HTTP " + String(code); http.end(); return false; }
  String enc = http.header("Content-Encoding"), te = http.header("Transfer-Encoding"), cl = http.header("Content-Length");
  PsramSink body;
  // Read the body with our own loop rather than HTTPClient::writeToStream: when the server stalls mid-response,
  // that one spins in a read loop without ever sleeping, starving core 0's idle task until the task watchdog
  // restarts the board (a slow Weather Underground forecast caused a restart loop). Here an empty read sleeps,
  // and 20 s without data ends the download.
  int expected = http.getSize();  // -1 when the server doesn't say; then read until it closes
  WiFiClient* in = http.getStreamPtr();
  uint8_t chunk[1024];
  uint32_t lastData = millis();
  int wrote = 0;
  while (in && (expected < 0 || (int)body.len < expected)) {
    int avail = in->available();
    if (avail > 0) {
      int r = in->read(chunk, min(avail, (int)sizeof chunk));
      if (r > 0) { body.write(chunk, r); wrote += r; lastData = millis(); continue; }
    }
    if (!in->connected()) break;
    if (millis() - lastData > 20000) { wrote = -1; break; }  // stalled
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (expected > 0 && (int)body.len < expected) wrote = -1;  // cut short
  http.end();
  if (wrote < 0 || !body.len) {
    err = String(what) + ": download failed (" + String(wrote) + ")";
    logf("[wx] %s: wrote %d, len %u, encoding '%s', transfer '%s', length '%s'\n", what, wrote, (unsigned)body.len, enc.c_str(), te.c_str(), cl.c_str());
    return false;
  }
  DeserializationError e = deserializeJson(doc, (const char*)body.buf, body.len, DeserializationOption::Filter(filter));
  if (e) {
    err = String(what) + ": " + e.c_str();
    char head[49]; size_t n = min(body.len, sizeof head - 1);
    for (size_t i = 0; i < n; i++) head[i] = isprint(body.buf[i]) ? body.buf[i] : '.';
    head[n] = 0;
    logf("[wx] %s: %u bytes, encoding '%s', transfer '%s', length '%s', starts '%s'\n",
                  what, (unsigned)body.len, enc.c_str(), te.c_str(), cl.c_str(), head);
    return false;
  }
  logf("[wx] %s: %u bytes\n", what, (unsigned)body.len);
  return true;
}

static String base() { return "https://api.weather.com"; }
static String gStation, gKey;  // set per call by weatherFetch (network task only)
static String keyParam() { return "&apiKey=" + gKey; }
static float f(JsonVariantConst v) { return v.isNull() ? NAN : v.as<float>(); }

static bool fetchCurrent(Weather& w) {
  JsonDocument filter;
  JsonObject o = filter["observations"][0].to<JsonObject>();
  o["epoch"] = o["humidity"] = o["winddir"] = true;
  JsonObject m = o["metric"].to<JsonObject>();
  m["temp"] = m["heatIndex"] = m["windChill"] = m["dewpt"] = m["windSpeed"] = m["windGust"] = m["pressure"] = m["precipTotal"] = true;
  JsonDocument doc(&psAlloc);
  String url = base() + "/v2/pws/observations/current?stationId=" + gStation + "&format=json&units=m&numericPrecision=decimal" + keyParam();
  if (!getJson(url, filter, doc, w.error, "current")) return false;
  JsonVariantConst ob = doc["observations"][0];
  if (ob.isNull()) { w.error = "current: empty response"; return false; }
  JsonVariantConst mt = ob["metric"];
  w.obsUtc = ob["epoch"] | 0;
  w.temp = f(mt["temp"]); w.dew = f(mt["dewpt"]);
  // With numericPrecision=decimal these come back as decimals (e.g. 86.0), so read them as floats.
  float hum = f(ob["humidity"]), dir = f(ob["winddir"]);
  w.humidity = isnan(hum) ? -1 : (int)lroundf(hum);
  w.windDir = isnan(dir) ? -1 : ((int)lroundf(dir) % 360);
  w.wind = f(mt["windSpeed"]); w.gust = f(mt["windGust"]);
  w.pressure = f(mt["pressure"]); w.rainToday = f(mt["precipTotal"]);
  // Feels like: wind chill when cold, heat index when hot, else the temperature.
  float wc = f(mt["windChill"]), hi = f(mt["heatIndex"]);
  w.feels = (!isnan(wc) && wc < w.temp) ? wc : (!isnan(hi) && hi > w.temp) ? hi : w.temp;
  w.haveCurrent = true;
  w.source = gStation;
  logf("[wx] current: %.1f C, dew %.1f, %d%%, wind %.0f gust %.0f, %.1f hPa\n", w.temp, w.dew, w.humidity, w.wind, w.gust, w.pressure);
  return true;
}

static bool fetchHistory(Weather& w) {
  JsonDocument filter;
  JsonObject o = filter["observations"][0].to<JsonObject>();
  o["epoch"] = true;
  JsonObject m = o["metric"].to<JsonObject>();
  m["tempAvg"] = m["dewptAvg"] = m["pressureMax"] = true;
  JsonDocument doc(&psAlloc);
  // hourly/7day rather than all/1day: all/1day only goes back to local midnight, so the trace was half empty
  // in the evening. Hourly data over 7 days (~170 rows) covers a full 24 hours at any time of day.
  String url = base() + "/v2/pws/observations/hourly/7day?stationId=" + gStation + "&format=json&units=m&numericPrecision=decimal" + keyParam();
  if (!getJson(url, filter, doc, w.error, "history")) return false;
  JsonArrayConst obs = doc["observations"];
  if (obs.isNull() || obs.size() == 0) { w.error = "history: empty response"; return false; }

  // Seed the on-device trace with the hourly rows of the last 24 hours (only hours the device hasn't
  // filled itself; the current hour is left to the live readings).
  time_t now = time(nullptr);
  uint32_t thisHour = (uint32_t)(now / 3600);
  int seeded = 0;
  for (JsonVariantConst ob : obs) {
    time_t ep = ob["epoch"] | 0; if (!ep) continue;
    long age = (long)(now - ep);
    if (age < 0 || age >= 24L * 3600) continue;
    uint32_t hr = (uint32_t)(ep / 3600);
    if (hr >= thisHour) continue;
    int s = hr % WX_TRACE;
    if (w.slotHour[s] == hr && w.slotNT[s]) continue;
    addSample(w, ep, f(ob["metric"]["tempAvg"]), f(ob["metric"]["dewptAvg"]), f(ob["metric"]["pressureMax"]));
    seeded++;
  }
  rebuildTrace(w, now);
  logf("[wx] history: seeded %d hours of the trace\n", seeded);
  return true;
}

static bool fetchForecast(Weather& w, double lat, double lon) {
  if (lat == 0 && lon == 0) { w.error = "forecast: location unknown"; return false; }
  JsonDocument filter;
  filter["dayOfWeek"] = filter["calendarDayTemperatureMax"] = filter["temperatureMin"] = true;
  JsonObject dp = filter["daypart"][0].to<JsonObject>();
  dp["cloudCover"] = dp["precipChance"] = dp["iconCode"] = dp["temperature"] = dp["narrative"] = true;
  JsonDocument doc(&psAlloc);
  char geo[48]; snprintf(geo, sizeof geo, "%.4f,%.4f", lat, lon);
  String url = base() + "/v3/wx/forecast/daily/5day?geocode=" + geo + "&format=json&units=m&language=en-US" + keyParam();
  if (!getJson(url, filter, doc, w.error, "forecast")) return false;
  JsonArrayConst dow = doc["dayOfWeek"];
  JsonVariantConst part = doc["daypart"][0];
  if (dow.isNull() || part.isNull()) { w.error = "forecast: unexpected response"; return false; }
  // Day k's night is daypart index 2k+1 (the arrays alternate day, night, starting today).
  for (int k = 0; k < WX_DAYS; k++) {
    WxDay& d = w.days[k];
    if (k == 0) strcpy(d.name, "TONIGHT");
    else { String n = dow[k] | ""; n = n.substring(0, 3); n.toUpperCase(); strlcpy(d.name, n.c_str(), sizeof d.name); }
    d.hi = k == 0 ? NAN : f(doc["calendarDayTemperatureMax"][k]);
    d.lo = f(doc["temperatureMin"][k]);
    int ni = 2 * k + 1;
    float cc = f(part["cloudCover"][ni]), pp = f(part["precipChance"][ni]), ic = f(part["iconCode"][ni]);
    d.cloud = isnan(cc) ? -1 : (int)lroundf(cc);
    d.pop = isnan(pp) ? -1 : (int)lroundf(pp);
    d.icon = isnan(ic) ? -1 : (int)lroundf(ic);
    d.valid = true;
  }
  // Today's daytime part (index 0). WU drops it (null) once the day is mostly over, from mid-afternoon.
  WxDay& t = w.today;
  t = WxDay();
  strcpy(t.name, "TODAY");
  float th = f(part["temperature"][0]), tc = f(part["cloudCover"][0]), tp = f(part["precipChance"][0]), ti = f(part["iconCode"][0]);
  t.valid = !isnan(th) || !isnan(ti);
  if (t.valid) {
    t.hi = th;
    t.cloud = isnan(tc) ? -1 : (int)lroundf(tc);
    t.pop = isnan(tp) ? -1 : (int)lroundf(tp);
    t.icon = isnan(ti) ? -1 : (int)lroundf(ti);
  }
  String dayText = part["narrative"][0] | "", nightText = part["narrative"][1] | "";
  if (t.valid && dayText.length()) { w.forecastTitle = "TODAY"; w.forecastText = dayText; }
  else { w.forecastTitle = "TONIGHT"; w.forecastText = nightText; }
  w.cloudTonight = w.days[0].cloud;
  w.haveForecast = true;
  logf("[wx] forecast: tonight cloud %d%%, low %.0f\n", w.cloudTonight, w.days[0].lo);
  return true;
}

bool weatherFetch(WxPart part, Weather& w, const WxSetup& s) {
  gStation = s.station; gKey = s.key;
  bool ok;
  if (part == WX_CURRENT) {
    if (s.ecowitt.length()) ok = fetchEcowitt(w, s.ecowitt);
    else if (s.station.length() && s.key.length()) ok = fetchCurrent(w);
    else { w.error = "not set up"; return false; }
    if (ok) { addSample(w, w.obsUtc, w.temp, w.dew, w.pressure); rebuildTrace(w, w.obsUtc); }
  } else if (!s.key.length() || (part == WX_HISTORY && !s.station.length())) { return false; }  // nothing to do
  else ok = part == WX_HISTORY ? fetchHistory(w) : fetchForecast(w, s.lat, s.lon);
  if (part != WX_CURRENT) {  // WU forecast / history: their own error, so EcoWitt successes don't hide it
    if (ok) w.wuError = "";
    else { w.wuError = w.error; w.error = ""; logf("[wx] %s\n", w.wuError.c_str()); return false; }
    return true;
  }
  if (ok) w.error = "";
  else logf("[wx] %s\n", w.error.c_str());
  return ok;
}
