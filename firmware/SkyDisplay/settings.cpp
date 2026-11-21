#include "settings.h"
#include <Preferences.h>

Settings cfg;
static Preferences prefs;

static SemaphoreHandle_t lockHandle() {
  static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
  return m;
}
void dataLock() { xSemaphoreTakeRecursive(lockHandle(), portMAX_DELAY); }
void dataUnlock() { xSemaphoreGiveRecursive(lockHandle()); }

void logf(const char* fmt, ...) {
  char buf[256];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  DataGuard g;
  Serial.print(buf);
}

void settingsLoad() {
  Settings d;  // defaults
  prefs.begin("sky", true);
  cfg.chartUrl      = prefs.getString("chartUrl", d.chartUrl);
  cfg.pollMinutes   = prefs.getInt("poll", d.pollMinutes);
  cfg.lat           = prefs.getDouble("lat", d.lat);
  cfg.lon           = prefs.getDouble("lon", d.lon);
  cfg.tz            = prefs.getString("tz", d.tz);
  cfg.ntp1          = prefs.getString("ntp1", d.ntp1);
  cfg.ntp2          = prefs.getString("ntp2", d.ntp2);
  cfg.ntpHours      = prefs.getInt("ntpHours", d.ntpHours);
  cfg.brightness    = prefs.getInt("bright", d.brightness);
  cfg.idleMode      = prefs.getInt("idleMode", d.idleMode);
  cfg.idleMinutes   = prefs.getInt("idleMin", d.idleMinutes);
  cfg.dimBrightness = prefs.getInt("dimBright", d.dimBrightness);
  cfg.nightDim      = prefs.getBool("nightDim", d.nightDim);
  cfg.nightFrom     = prefs.getInt("nightFrom", d.nightFrom);
  cfg.nightTo       = prefs.getInt("nightTo", d.nightTo);
  cfg.footerSeconds = prefs.getInt("footSec", d.footerSeconds);
  cfg.goodMaxCloud  = prefs.getInt("goodCloud", d.goodMaxCloud);
  cfg.goodMinTrans  = prefs.getInt("goodTrans", d.goodMinTrans);
  cfg.goMinHours    = prefs.getInt("goHours", d.goMinHours);
  cfg.wuStation     = prefs.getString("wuStation", d.wuStation);
  cfg.wuKey         = prefs.getString("wuKey", d.wuKey);
  cfg.hostname      = prefs.getString("host", d.hostname);
  cfg.webPassword   = prefs.getString("webPass", d.webPassword);
  prefs.end();
}

void settingsSave() {
  prefs.begin("sky", false);
  prefs.putString("chartUrl", cfg.chartUrl);
  prefs.putInt("poll", cfg.pollMinutes);
  prefs.putDouble("lat", cfg.lat);
  prefs.putDouble("lon", cfg.lon);
  prefs.putString("tz", cfg.tz);
  prefs.putString("ntp1", cfg.ntp1);
  prefs.putString("ntp2", cfg.ntp2);
  prefs.putInt("ntpHours", cfg.ntpHours);
  prefs.putInt("bright", cfg.brightness);
  prefs.putInt("idleMode", cfg.idleMode);
  prefs.putInt("idleMin", cfg.idleMinutes);
  prefs.putInt("dimBright", cfg.dimBrightness);
  prefs.putBool("nightDim", cfg.nightDim);
  prefs.putInt("nightFrom", cfg.nightFrom);
  prefs.putInt("nightTo", cfg.nightTo);
  prefs.putInt("footSec", cfg.footerSeconds);
  prefs.putInt("goodCloud", cfg.goodMaxCloud);
  prefs.putInt("goodTrans", cfg.goodMinTrans);
  prefs.putInt("goHours", cfg.goMinHours);
  prefs.putString("wuStation", cfg.wuStation);
  prefs.putString("wuKey", cfg.wuKey);
  prefs.putString("host", cfg.hostname);
  prefs.putString("webPass", cfg.webPassword);
  prefs.end();
}

ChartTarget chartTarget() {
  ChartTarget t; t.ok = false;
  String u = cfg.chartUrl; u.trim();
  String host = "cds.darkdragonsastro.com", key;
  int p = u.indexOf("://");
  if (p >= 0) {
    String rest = u.substring(p + 3);
    int slash = rest.indexOf('/');
    host = slash < 0 ? rest : rest.substring(0, slash);
    int g = rest.indexOf("/g/");
    if (g < 0) return t;
    key = rest.substring(g + 3);
  } else {
    // Just the chart ID, or a partial path like "/g/abcde" or "g/abcde.html".
    key = u;
    int g = key.indexOf("g/");
    if (g >= 0 && (g == 0 || key[g - 1] == '/')) key = key.substring(g + 2);
    while (key.startsWith("/")) key = key.substring(1);
  }
  int cut = key.indexOf('.'); if (cut >= 0) key = key.substring(0, cut);
  cut = key.indexOf('/'); if (cut >= 0) key = key.substring(0, cut);
  cut = key.indexOf('?'); if (cut >= 0) key = key.substring(0, cut);
  key.trim();
  // Chart IDs are lower case and the server is case-sensitive; phone keyboards often capitalise.
  key.toLowerCase();
  host.toLowerCase();
  if (!key.length() || !host.length()) return t;
  t.host = host; t.key = key;
  t.pagePath = "/g/" + key + ".html";
  t.gifPath = "/g/" + key + "/chart.gif";
  t.ok = true;
  return t;
}
