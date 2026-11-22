#include "cache.h"
#include "settings.h"
#include <Preferences.h>
#include <nvs.h>
#include <type_traits>

static_assert(std::is_trivially_copyable<Forecast>::value, "Forecast is saved as raw bytes");
static_assert(std::is_trivially_copyable<WxData>::value, "WxData is saved as raw bytes");

static const char* NS = "cache";
static const uint32_t LAYOUT = 1;  // bump when a saved struct changes meaning without changing size

// Size and layout version together: a firmware with different structs reads the old copy as missing.
static uint32_t tag(size_t size) { return (LAYOUT << 24) | (uint32_t)size; }

// The weather setup includes the API key; only a hash of it is stored here.
static uint32_t hashKey(const String& s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}

void cacheSaveChart(const Forecast& f, const String& chartKey, const String& etag, time_t checkedUtc) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  bool ok = p.putBytes("fc", &f, sizeof f) == sizeof f;
  p.putUInt("fcTag", ok ? tag(sizeof f) : 0);
  p.putString("fcKey", chartKey);
  p.putString("fcEtag", etag);
  p.putULong64("fcChk", (uint64_t)checkedUtc);
  p.end();
  logf("[cache] chart %s\n", ok ? "saved" : "NOT saved (flash full?)");
}

void cacheSaveChartChecked(time_t checkedUtc) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putULong64("fcChk", (uint64_t)checkedUtc);
  p.end();
}

bool cacheLoadChart(Forecast& f, const String& chartKey, String& etag, time_t& checkedUtc) {
  Preferences p;
  if (!p.begin(NS, true)) return false;  // nothing saved yet
  bool ok = p.getUInt("fcTag", 0) == tag(sizeof f) && p.getString("fcKey", "") == chartKey && chartKey.length()
         && p.getBytesLength("fc") == sizeof f;
  if (ok) {
    Forecast tmp;
    ok = p.getBytes("fc", &tmp, sizeof tmp) == sizeof tmp && tmp.valid && tmp.n > 0 && tmp.n <= MAX_COLS;
    if (ok) { f = tmp; etag = p.getString("fcEtag", ""); checkedUtc = (time_t)p.getULong64("fcChk", 0); }
  }
  p.end();
  return ok;
}

void cacheSaveWeather(const Weather& w, const String& setupKey, const time_t okUtc[3]) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  const WxData& d = w;
  bool ok = p.putBytes("wx", &d, sizeof d) == sizeof d;
  p.putUInt("wxTag", ok ? tag(sizeof d) : 0);
  p.putUInt("wxKey", hashKey(setupKey));
  p.putString("wxSrc", w.source);
  p.putString("wxFT", w.forecastTitle);
  p.putString("wxFX", w.forecastText);
  uint64_t t[3] = {(uint64_t)okUtc[0], (uint64_t)okUtc[1], (uint64_t)okUtc[2]};
  p.putBytes("wxOk", t, sizeof t);
  p.end();
  if (!ok) logf("[cache] weather NOT saved (flash full?)\n");
}

bool cacheLoadWeather(Weather& w, const String& setupKey, time_t okUtc[3]) {
  Preferences p;
  if (!p.begin(NS, true)) return false;
  uint64_t t[3] = {0, 0, 0};
  bool ok = p.getUInt("wxTag", 0) == tag(sizeof(WxData)) && p.getUInt("wxKey", 0) == hashKey(setupKey)
         && p.getBytesLength("wx") == sizeof(WxData) && p.getBytes("wxOk", t, sizeof t) == sizeof t;
  if (ok) {
    Weather tmp;
    WxData& d = tmp;
    ok = p.getBytes("wx", &d, sizeof d) == sizeof d;
    if (ok) {
      tmp.source = p.getString("wxSrc", "");
      tmp.forecastTitle = p.getString("wxFT", "");
      tmp.forecastText = p.getString("wxFX", "");
      w = tmp;
      for (int i = 0; i < 3; i++) okUtc[i] = (time_t)t[i];
    }
  }
  p.end();
  return ok;
}

void cacheLogUsage() {
  nvs_stats_t s;
  if (nvs_get_stats(nullptr, &s) == ESP_OK)
    Serial.printf("[cache] NVS %u of %u entries used, %u free\n", (unsigned)s.used_entries, (unsigned)s.total_entries, (unsigned)s.free_entries);
}
