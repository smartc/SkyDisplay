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

bool weatherConfigured() { return cfg.wuStation.length() > 0 && cfg.wuKey.length() > 0; }

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
  if (WiFi.status() != WL_CONNECTED) { err = "no Wi-Fi"; return false; }
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(20000);  // milliseconds (Stream::setTimeout)
  HTTPClient http;
  http.setUserAgent("SkyDisplay/" FIRMWARE_VERSION);
  http.setTimeout(20000);
  const char* keys[] = {"Content-Encoding", "Transfer-Encoding", "Content-Length"};
  http.collectHeaders(keys, 3);
  if (!http.begin(client, url)) { err = String(what) + ": begin failed"; return false; }
  http.addHeader("Accept-Encoding", "identity");
  int code = http.GET();
  if (code == 204) { err = String(what) + ": no data (station offline?)"; http.end(); return false; }
  if (code == 401) { err = "weather: API key rejected"; http.end(); return false; }
  if (code == 404) { err = "weather: station not found"; http.end(); return false; }
  if (code != 200) { err = String(what) + ": HTTP " + String(code); http.end(); return false; }
  String enc = http.header("Content-Encoding"), te = http.header("Transfer-Encoding"), cl = http.header("Content-Length");
  PsramSink body;
  int wrote = http.writeToStream(&body);
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

  time_t now = time(nullptr);
  float sumT[WX_TRACE] = {0}, sumD[WX_TRACE] = {0}; int nT[WX_TRACE] = {0}, nD[WX_TRACE] = {0};
  float t1h = NAN, p3h = NAN; long best1h = LONG_MAX, best3h = LONG_MAX;
  for (JsonVariantConst ob : obs) {
    time_t ep = ob["epoch"] | 0; if (!ep) continue;
    long age = (long)(now - ep);
    if (age < 0 || age >= 24L * 3600) continue;
    int slot = WX_TRACE - 1 - (int)(age / 3600);  // hourly buckets, newest last
    float t = f(ob["metric"]["tempAvg"]), d = f(ob["metric"]["dewptAvg"]), p = f(ob["metric"]["pressureMax"]);
    if (!isnan(t)) { sumT[slot] += t; nT[slot]++; }
    if (!isnan(d)) { sumD[slot] += d; nD[slot]++; }
    if (!isnan(t) && labs(age - 3600) < best1h) { best1h = labs(age - 3600); t1h = t; }
    if (!isnan(p) && labs(age - 3 * 3600) < best3h) { best3h = labs(age - 3 * 3600); p3h = p; }
  }
  for (int i = 0; i < WX_TRACE; i++) {
    w.traceT[i] = nT[i] ? sumT[i] / nT[i] : NAN;
    w.traceD[i] = nD[i] ? sumD[i] / nD[i] : NAN;
  }
  // Hourly rows are averages, so allow up to 40 / 50 minutes either side of the target time.
  w.change1h = (best1h < 40 * 60 && !isnan(w.temp)) ? w.temp - t1h : NAN;
  w.pressure3h = (best3h < 50 * 60 && !isnan(w.pressure)) ? w.pressure - p3h : NAN;
  w.haveHistory = true;
  logf("[wx] history: %u observations, 1 h change %.1f, 3 h pressure %.1f\n", (unsigned)obs.size(), w.change1h, w.pressure3h);
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

bool weatherFetch(WxPart part, Weather& w, const String& station, const String& key, double lat, double lon) {
  if (!station.length() || !key.length()) { w.error = "not set up"; return false; }
  gStation = station; gKey = key;
  bool ok = part == WX_CURRENT ? fetchCurrent(w) : part == WX_HISTORY ? fetchHistory(w) : fetchForecast(w, lat, lon);
  if (ok) w.error = "";
  else logf("[wx] %s\n", w.error.c_str());
  return ok;
}
