// SkyDisplay: bedside astronomy forecast for the WT32-SC01 Plus or SenseCAP Indicator D1 (ESP32-S3).
// The board is picked in board.h (automatically, from the PSRAM type).
// Reads the Dark Dragons Clear Sky Chart GIF, decodes the forecast rows and shows
// whether now, tonight and tomorrow night are worth setting up the rigs.
// Settings page: http://skydisplay.local/ (or the IP shown in the serial log).
//
// Screens (swipe left/right): forecast, weather (Weather Underground), observatory (when turned on), status.
// Touch: tap = next footer page (status screen: "Refresh now" button), long press = dim.
// Serial commands (115200 on the WT32, 460800 on the Indicator): s = screenshot dump, r = check chart and weather now, d = dump decoded rows,
//   a = sun/moon times, n = next screen, i = info, W = forget Wi-Fi and restart.

#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_sntp.h>
#include "config.h"
#include "board.h"
#include "settings.h"
#include "forecast.h"
#include "net.h"
#include "astro.h"
#include "ui.h"
#include "web.h"
#include "weather.h"
#include "tls.h"
#include "obs.h"
#include "cache.h"

static Forecast fc;
static UiState ui;
static Weather wx;
static Observatory obs;
static volatile uint32_t nextObsMs = 0;  // observatory poll: every 15 s while its screen is shown, else every 60 s
static const uint32_t OBS_FAST_MS = 15000, OBS_SLOW_MS = 60000;
// Weather polling. Current conditions: EcoWitt every minute (local, free) or the WU station every 10 min.
// The 24 h trace is built on the device; WU history is read once (at start-up or a settings change) to seed it.
// Forecast hourly. Per display that's 24 WU calls a day with EcoWitt, about 170 without (limit 1,500 per key).
static volatile uint32_t nextWxMs[3] = {0, 0, 0};
static const uint32_t WX_CURRENT_ECOWITT_MS = 60000UL, WX_CURRENT_WU_MS = 10UL * 60000, WX_FORECAST_MS = 60UL * 60000;
static const uint32_t WX_NEVER = 0x7FFFFFFF;  // history after it has seeded the trace
static uint8_t wxFails[3] = {0, 0, 0};         // consecutive failures per part, for the retry backoff
static String wxSetupKey;  // station + key the current data came from; a change resets the weather
static time_t wxOkUtc[3] = {0, 0, 0};      // last success per part (UTC), kept with the cached weather
static uint32_t wxSavedMs = 0;             // last weather save to flash
static const uint32_t WX_SAVE_MS = 30UL * 60000;  // current conditions alone are saved at most this often (flash wear)
static volatile bool refreshPending = false;      // "Refresh now" tapped on the status screen; cleared by the next chart check
static uint32_t refreshTapMs = 0;
static volatile uint32_t nextCheckMs = 0;  // shared with the network task
static uint32_t nextDrawMs = 0, nextSkyMs = 0, lastTouchMs = 0;
static bool manualDim = false;
static uint32_t nightWakeUntilMs = 0;  // a tap during night hours brightens the screen until this time
static bool skipAdvance = true;         // draw the current footer page without advancing first
static const uint32_t SCREEN_RETURN_MS = 120000;  // other screens return to the start-up screen after this long untouched
static time_t nowUtc();
static volatile time_t lastNtpSync = 0;      // UTC time of the last successful NTP sync (0 = none since boot)
static const long NTP_STALE_SECONDS = 12L * 3600;

static String fmtLocal(time_t t, const char* fmt) {
  if (!t) return "\xE2\x80\x93";
  struct tm tmv; localtime_r(&t, &tmv); char b[32]; strftime(b, sizeof b, fmt, &tmv); return b;
}

static void buildStatus(StatusInfo& st) {
  auto L = [&](const String& l, const String& v, bool warn = false) { if (st.nLeft < 8) { st.left[st.nLeft++] = {l, v, warn}; } };
  auto R = [&](const String& l, const String& v, bool warn = false) { if (st.nRight < 8) { st.right[st.nRight++] = {l, v, warn}; } };
  int rssi = WiFi.RSSI();
  const char* q = rssi >= -60 ? "good" : rssi >= -70 ? "fair" : "weak";
  bool connected = WiFi.status() == WL_CONNECTED;
  L("WI-FI", connected ? WiFi.SSID() + "  " + String(rssi) + " dBm, " + q : String("Not connected"), !connected || rssi < -70);
  L("IP ADDRESS", WiFi.localIP().toString());
  L("SETTINGS PAGE", "http://" + cfg.hostname + "/  or  .local");
  ChartTarget ct = chartTarget();
  L("CHART", ct.ok ? ct.key : String("URL not understood"), !ct.ok);
  if (ui.problem.length()) L("LAST CHECK", ui.problem, true);
  else L("LAST CHECK", ui.lastCheckOk ? fmtLocal(ui.lastCheckOk, "%H:%M") + ", every " + String(cfg.pollMinutes) + " min" : String("Not yet"));
  L("CHART STARTS", fc.valid ? fmtLocal(fc.startUtc, "%a %H:%M") + ", " + String(fc.n) + " hours" : String("\xE2\x80\x93"));
  bool own = !(cfg.lat == 0 && cfg.lon == 0);
  double la = own ? cfg.lat : fc.siteLat, lo = own ? cfg.lon : fc.siteLon;
  L("LOCATION", (la == 0 && lo == 0) ? String("Unknown") : String(la, 4) + ", " + String(lo, 4) + (own ? "" : " (chart)"));

  R("TIME ZONE", cfg.tz);
  R("NTP", cfg.ntp1 + (cfg.ntp2.length() ? " +1" : ""));
  {
    time_t last = lastNtpSync, now = nowUtc();
    if (!now) R("CLOCK", "Not set", true);
    else if (!last) R("CLOCK", "No NTP sync yet", true);
    else {
      long age = (long)(now - last);
      if (age > NTP_STALE_SECONDS) R("CLOCK", "Last sync " + String(age / 3600) + " h ago", true);
      else R("CLOCK", "Synced " + fmtLocal(last, "%H:%M") + " (" + String(cfg.ntpHours) + " h)");
    }
  }
  R("BRIGHTNESS", String(cfg.brightness) + ", dim " + String(cfg.dimBrightness));
  static const char* IDLE[] = {"Stays on", "Dims", "Turns off"};
  R("WHEN IDLE", cfg.idleMode ? String(IDLE[cfg.idleMode]) + " after " + String(cfg.idleMinutes) + " min" : String(IDLE[0]));
  uint32_t up = millis() / 60000;
  R("FIRMWARE", String(FIRMWARE_VERSION) + ", up " + (up >= 60 ? String(up / 60) + "h " : "") + String(up % 60) + "m");
  if (!weatherConfigured()) R("WEATHER", "Not set up");
  else if (wx.error.length()) R("WEATHER", wx.error, true);
  else if (wx.wuError.length()) R("WEATHER", wx.wuError, true);
  else R("WEATHER", (weatherUsesEcowitt() ? String("EcoWitt") : cfg.wuStation) + (wx.haveCurrent ? ", " + fmtLocal(wx.obsUtc, "%H:%M") : String(", waiting")));
  st.qrUrl = "http://" + WiFi.localIP().toString() + "/";
  st.refreshing = refreshPending && millis() - refreshTapMs < 60000;  // gives up on the label after a minute (no Wi-Fi)
}

// Swipe order; the observatory screen only when it's turned on.
static int screenOrder(int* out) {
  int n = 0; out[n++] = SCREEN_FORECAST; out[n++] = SCREEN_WEATHER;
  if (cfg.obsEnabled) out[n++] = SCREEN_OBS;
  out[n++] = SCREEN_STATUS; return n;
}
static int homeScreen() { return (cfg.startScreen == SCREEN_OBS && !cfg.obsEnabled) ? SCREEN_FORECAST : cfg.startScreen; }
static void stepScreen(int d) {
  int ord[SCREEN_COUNT]; int n = screenOrder(ord), pos = 0;
  for (int i = 0; i < n; i++) if (ord[i] == ui.screen) pos = i;
  ui.screen = ord[(pos + d + n) % n];
  ui.wxText = false;  // the weather screen always opens on the day strip
}

static void drawScreen() {
  ui.nowUtc = nowUtc();
  { int ord[SCREEN_COUNT]; ui.dotCount = screenOrder(ord); ui.dotIndex = 0; for (int i = 0; i < ui.dotCount; i++) if (ord[i] == ui.screen) ui.dotIndex = i; }
  ui.needsSetup = !chartTarget().ok;  // fresh device: show the first-run QR code instead of the forecast
  ui.settingsUrl = "http://" + WiFi.localIP().toString() + "/";
  if (ui.screen == SCREEN_STATUS) { StatusInfo st; buildStatus(st); uiDrawStatus(ui, st); }
  else if (ui.screen == SCREEN_WEATHER) uiDrawWeather(ui, wx, weatherConfigured(), "http://" + WiFi.localIP().toString() + "/");
  else if (ui.screen == SCREEN_OBS) uiDrawObservatory(ui, obs, "http://" + WiFi.localIP().toString() + "/");
  else uiDraw(ui);
}

static int currentBacklight = -1;
static volatile bool chartJustUpdated = false;  // set by the network task; the loop then recomputes sun/moon
static volatile bool redrawNeeded = false;      // set by the network task when shown data changed

static time_t nowUtc() { time_t t = time(nullptr); return t > 1700000000 ? t : 0; }

static void onPortal(WiFiManager*) {
  // Hand out the setup network's own address as the DNS server. Without this (esp32 core 3.x only offers DNS
  // when one is passed explicitly, and WiFiManager doesn't), phones can't resolve their connectivity-check host,
  // decide the network has "no internet" instead of a sign-in page, and send browser traffic over mobile data.
  // With it, WiFiManager's catch-all DNS answers the check and the phone shows its "Sign in to network" page.
  // This runs before WiFiManager starts its DNS and web servers.
  IPAddress ip(192, 168, 4, 1), mask(255, 255, 255, 0), leaseStart(192, 168, 4, 2);
  if (!WiFi.softAPConfig(ip, ip, mask, leaseStart, ip)) Serial.println("[main] setup AP DNS config failed");
  uiWifiSetupScreen(SETUP_AP_NAME, "http://" + WiFi.softAPIP().toString() + "/");
}

// ---------- clock: NTP keeps running in the background and re-syncs every cfg.ntpHours ----------
static void onNtpSync(struct timeval* tv) {  // called by the SNTP client on every successful sync
  lastNtpSync = tv ? tv->tv_sec : time(nullptr);
}

static void applyTime() {
  sntp_set_time_sync_notification_cb(onNtpSync);
  sntp_set_sync_interval((uint32_t)constrain(cfg.ntpHours, 1, 24) * 3600UL * 1000UL);  // read by sntp_init below
  configTzTime(cfg.tz.c_str(), cfg.ntp1.c_str(), cfg.ntp2.length() ? cfg.ntp2.c_str() : nullptr);
}

static void updateSky() {
  double lat = cfg.lat, lon = cfg.lon;
  if (lat == 0 && lon == 0) { lat = fc.siteLat; lon = fc.siteLon; }
  time_t now = nowUtc();
  ui.skyValid = now && !(lat == 0 && lon == 0);
  if (ui.skyValid) ui.sky = computeSkyEvents(now, lat, lon);
  nextSkyMs = millis() + 5UL * 60 * 1000;
}

// ---------- network task (core 0) ----------
// Chart and weather downloads take 0.5 to 2 seconds each. They run here so the screen never waits.
// Work happens on private copies; results are swapped in under the data lock.
static void netCheckChart() {
  ChartTarget ct; Forecast work;
  { DataGuard g; ct = chartTarget(); work = fc; }
  logf("[net] checking chart...\n");
  String err;
  uint32_t t0 = millis();
  FetchResult r = fetchChart(work, ct, err);
  time_t checked = 0;
  {
    DataGuard g;
    if (r == FETCH_FAILED) {
      logf("[net] chart check failed: %s\n", err.c_str());
      ui.problem = err.substring(0, 40);
      nextCheckMs = millis() + 5UL * 60 * 1000;  // retry sooner after a failure
    } else {
      ui.problem = "";
      ui.lastCheckOk = checked = nowUtc();
      if (r == FETCH_UPDATED) { fc = work; chartJustUpdated = true; logf("[main] chart updated: %d columns\n", fc.n); }
      nextCheckMs = millis() + (uint32_t)cfg.pollMinutes * 60 * 1000;
    }
    refreshPending = false;
  }
  // Flash writes outside the lock, so the screen doesn't wait for them.
  if (r == FETCH_UPDATED) cacheSaveChart(work, ct.key, netEtag(), checked);
  else if (r == FETCH_UNCHANGED) cacheSaveChartChecked(checked);
  logf("[net] chart check %u ms\n", (unsigned)(millis() - t0));
  redrawNeeded = true;
}

// At most one weather request per pass.
static void netPollWeather() {
  WxSetup s; Weather work; bool configured; String setupKey;
  {
    DataGuard g;
    String setup = cfg.wuStation + "|" + cfg.wuKey + "|" + cfg.ecowittHost;
    if (setup != wxSetupKey) { wx = Weather(); wxSetupKey = setup; nextWxMs[0] = nextWxMs[1] = nextWxMs[2] = millis(); wxFails[0] = wxFails[1] = wxFails[2] = 0; wxOkUtc[0] = wxOkUtc[1] = wxOkUtc[2] = 0; wxSavedMs = 0; redrawNeeded = true; }
    configured = weatherConfigured();
    s.station = cfg.wuStation; s.key = cfg.wuKey; s.ecowitt = cfg.ecowittHost;
    s.lat = cfg.lat; s.lon = cfg.lon;
    if (s.lat == 0 && s.lon == 0) { s.lat = fc.siteLat; s.lon = fc.siteLon; }
    setupKey = setup;
    work = wx;
  }
  if (!configured) return;
  bool eco = s.ecowitt.length() > 0;
  for (int p = 0; p < 3; p++) {
    if ((int32_t)(millis() - nextWxMs[p]) < 0) continue;
    if (p != WX_CURRENT && !work.haveCurrent) continue;  // fetch current first
    bool ok = weatherFetch((WxPart)p, work, s);
    // On failure retry in 2 minutes (a rejected key waits the full period, to stay well inside the call limit).
    String why = p == WX_CURRENT ? work.error : work.wuError;
    bool keyProblem = why.indexOf("API key") >= 0 || why.indexOf("not found") >= 0;
    uint32_t period = p == WX_CURRENT ? (eco ? WX_CURRENT_ECOWITT_MS : WX_CURRENT_WU_MS) : p == WX_HISTORY ? WX_NEVER : WX_FORECAST_MS;
    bool save = false; time_t okUtc[3];
    {
      DataGuard g;
      if (wxSetupKey != setupKey) return;  // ignore results if the settings changed meanwhile
      wx = work;
      if (ok) wxOkUtc[p] = nowUtc();
      // Failures back off: 2, 4, 8, 16, then every 30 min (failed calls count against the WU limit too).
      // The history only seeds the trace, so it gives up after 3 tries until the next restart or settings change.
      wxFails[p] = ok ? 0 : (uint8_t)min(wxFails[p] + 1, 10);
      uint32_t retry = (eco && p == WX_CURRENT) ? WX_CURRENT_ECOWITT_MS : min(30UL * 60000, (2UL * 60000) << min((int)wxFails[p] - 1, 4));
      if (!ok && p == WX_HISTORY && wxFails[p] >= 3) retry = WX_NEVER;
      nextWxMs[p] = millis() + (ok || keyProblem ? period : retry);
      redrawNeeded = true;
      // Save after every forecast or history download; current conditions only now and then (they come every
      // minute from EcoWitt, and after a restart only the hourly trace needs them).
      save = ok && (p != WX_CURRENT || !wxSavedMs || millis() - wxSavedMs >= WX_SAVE_MS);
      if (save) { wxSavedMs = millis(); memcpy(okUtc, wxOkUtc, sizeof okUtc); }
    }
    if (save) cacheSaveWeather(work, setupKey, okUtc);
    return;
  }
}

static void netPollObservatory() {
  ObsConfig c; Observatory work;
  {
    DataGuard g;
    if (!cfg.obsEnabled) return;
    c.roof = cfg.obsRoof; c.safety = cfg.obsSafety; c.rain = cfg.obsRain; c.sky = cfg.obsSky; c.holdHour = cfg.obsHoldHour;
    for (int i = 0; i < OBS_MAX_RIGS; i++) { c.rigName[i] = cfg.rigName[i]; c.rigHost[i] = cfg.rigHost[i]; c.rigPark[i] = cfg.rigPark[i]; }
    work = obs;
  }
  uint32_t t0 = millis();
  obsPoll(work, c);
  DataGuard g;
  obs = work;
  bool shown = ui.screen == SCREEN_OBS;
  if (shown) redrawNeeded = true;
  nextObsMs = millis() + (shown ? OBS_FAST_MS : OBS_SLOW_MS);
  if (millis() - t0 > 5000) logf("[obs] poll took %u ms\n", (unsigned)(millis() - t0));
}

static void netTask(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && nowUtc()) {
      if ((int32_t)(millis() - nextCheckMs) >= 0) netCheckChart();
      netPollWeather();
      if ((int32_t)(millis() - nextObsMs) >= 0) netPollObservatory();
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ---------- start-up: the last chart and weather from flash ----------
// The screens fill straight away, and each part is downloaded only when it's due on its usual schedule
// (counted from when it was last fetched, before the restart).
static void restoreCache() {
  cacheLogUsage();
  time_t now = nowUtc();
  auto dueIn = [&](time_t okUtc, uint32_t periodMs) -> uint32_t {  // ms until the next fetch (0 = now)
    if (!now || !okUtc || okUtc > now) return 0;
    uint64_t ageMs = (uint64_t)(now - okUtc) * 1000;
    return ageMs >= periodMs ? 0 : periodMs - (uint32_t)ageMs;
  };

  ChartTarget ct = chartTarget();
  String etag; time_t checked = 0;
  if (ct.ok && cacheLoadChart(fc, ct.key, etag, checked)) {
    if (now && now >= fc.startUtc + (time_t)fc.n * 3600) {  // every hour of it has passed
      fc.valid = false;
      Serial.println("[cache] saved chart is out of date, not used");
    } else {
      netSetEtag(etag);  // the first check is then a cheap "unchanged?" request
      ui.lastCheckOk = checked;
      chartJustUpdated = true;
      uint32_t due = dueIn(checked, (uint32_t)cfg.pollMinutes * 60000UL);
      nextCheckMs = millis() + due;
      Serial.printf("[cache] chart from flash, next check in %u min\n", (unsigned)(due / 60000));
    }
  }

  String setup = cfg.wuStation + "|" + cfg.wuKey + "|" + cfg.ecowittHost;
  time_t ok[3] = {0, 0, 0};
  if (weatherConfigured() && cacheLoadWeather(wx, setup, ok)) {
    long age = (now && ok[WX_CURRENT] && ok[WX_CURRENT] <= now) ? (long)(now - ok[WX_CURRENT]) : -1;
    if (now && (age < 0 || age > 24L * 3600)) {
      wx = Weather();
      Serial.println("[cache] saved weather is over a day old, not used");
    } else {
      wxSetupKey = setup;  // so the first poll keeps it instead of starting afresh
      memcpy(wxOkUtc, ok, sizeof wxOkUtc);
      wxSavedMs = millis();
      if (age > 3600) wx.haveCurrent = false;  // too old to show as "now"; fetched first anyway
      bool eco = cfg.ecowittHost.length() > 0;
      nextWxMs[WX_CURRENT] = millis() + dueIn(ok[WX_CURRENT], eco ? WX_CURRENT_ECOWITT_MS : WX_CURRENT_WU_MS);
      nextWxMs[WX_FORECAST] = millis() + dueIn(ok[WX_FORECAST], WX_FORECAST_MS);
      // The saved trace replaces the WU history call unless the display was off long enough to leave a gap.
      bool traceOk = wx.haveHistory && age >= 0 && age < 2 * 3600;
      nextWxMs[WX_HISTORY] = traceOk ? millis() + WX_NEVER : millis();
      Serial.printf("[cache] weather from flash (%ld min old), forecast in %u min%s\n", age / 60,
                    (unsigned)((nextWxMs[WX_FORECAST] - millis()) / 60000), traceOk ? ", no history call" : "");
    }
  }
}

// ---------- web hooks ----------
static void webCheckNow() { nextCheckMs = 0; nextWxMs[0] = nextWxMs[2] = millis(); nextObsMs = millis(); }  // history only seeds once
static void webApply(bool chartChanged, bool timeChanged) {
  if (chartChanged) { fc.valid = false; netForgetEtag(); nextCheckMs = 0; }
  if (timeChanged) { applyTime(); nextSkyMs = 0; }
  currentBacklight = -1;  // re-apply brightness
  nextObsMs = millis();   // observatory devices may have changed
  uint32_t soonest = millis() + (uint32_t)cfg.pollMinutes * 60 * 1000;
  if ((int32_t)(nextCheckMs - soonest) > 0) nextCheckMs = soonest;
  nextDrawMs = 0;
}
static void webShowScreen(int s) {
  if (s < 0 || s >= SCREEN_COUNT || (s == SCREEN_OBS && !cfg.obsEnabled)) return;
  ui.screen = s; lastTouchMs = millis(); drawScreen();
}
static String webStatus() {
  String s = "Firmware " FIRMWARE_VERSION " | chart " + chartTarget().key;
  if (ui.problem.length()) s += " | problem: " + ui.problem;
  else if (ui.lastCheckOk) {
    struct tm tmv; localtime_r(&ui.lastCheckOk, &tmv);
    char b[24]; snprintf(b, sizeof b, " | checked %02d:%02d", tmv.tm_hour, tmv.tm_min); s += b;
  }
  return s;
}

// ---------- backlight: idle, night hours, tap to dim ----------
static bool inNightHours() {
  if (!cfg.nightDim || !nowUtc()) return false;
  time_t t = nowUtc(); struct tm tmv; localtime_r(&t, &tmv);
  int h = tmv.tm_hour;
  return cfg.nightFrom <= cfg.nightTo ? (h >= cfg.nightFrom && h < cfg.nightTo) : (h >= cfg.nightFrom || h < cfg.nightTo);
}
static bool isIdle() { return cfg.idleMode > 0 && millis() - lastTouchMs > (uint32_t)cfg.idleMinutes * 60000UL; }
static int targetBacklight() {
  if (BLANK_DURING_FLASH_WRITES && webUpdating()) return 0;  // see board.h
  if (isIdle()) return cfg.idleMode == 2 ? 0 : cfg.dimBrightness;
  if (manualDim) return cfg.dimBrightness;
  if (inNightHours() && (int32_t)(millis() - nightWakeUntilMs) >= 0) return cfg.dimBrightness;
  return cfg.brightness;
}

static void printSky() {
  updateSky();
  if (!ui.skyValid) { Serial.println("no location yet"); return; }
  auto p = [](const char* n, time_t t) {
    if (!t) { Serial.printf("%-10s none\n", n); return; }
    struct tm tmv; localtime_r(&t, &tmv); Serial.printf("%-10s %02d-%02d %02d:%02d\n", n, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
  };
  p("sunset", ui.sky.sunset); p("dusk-18", ui.sky.duskAstro); p("dawn-18", ui.sky.dawnAstro); p("sunrise", ui.sky.sunrise);
  p("moonrise", ui.sky.moonrise); p("moonset", ui.sky.moonset);
  Serial.printf("moon      %.0f%% %s (%s)\n", ui.sky.moonIllum * 100, ui.sky.moonWaxing ? "waxing" : "waning", moonPhaseName(ui.sky.moonIllum, ui.sky.moonWaxing));
}

static void dumpRows() {
  if (!fc.valid) { Serial.println("no chart"); return; }
  auto row = [](const char* name, const int8_t* v) {
    Serial.printf("%-7s", name);
    for (int i = 0; i < fc.n; i++) Serial.print(v[i] < 0 ? '.' : (v[i] < 10 ? (char)('0' + v[i]) : (char)('A' + v[i] - 10)));
    Serial.println();
  };
  Serial.printf("start %ld  cols %d  site %.5f,%.5f\n", (long)fc.startUtc, fc.n, fc.siteLat, fc.siteLon);
  row("rdps", fc.rdps); row("ecmwf", fc.ecmwf); row("hrrr", fc.hrrr);
  row("trans", fc.trans); row("seeing", fc.seeing); row("wind", fc.wind); row("hum", fc.hum);
  Serial.printf("%-7s", "dark");
  for (int i = 0; i < fc.n; i++) Serial.print(isMoonlit(fc, i) ? 'm' : isDark(fc, i) ? '#' : '-');
  Serial.println();
  Serial.printf("%-7s", "hour");
  for (int i = 0; i < fc.n; i++) Serial.print((char)('0' + localHour(fc, i) % 10));
  Serial.println();
}

void setup() {
  // USB serial: when no program on the PC is reading the port, the ESP32 still sees the USB host and
  // each write waits (100 ms by default) for buffer space. With logging on taps and network events that
  // stalled the whole UI for seconds. Never wait: a bigger buffer, and lines that don't fit are dropped.
  Serial.setTxBufferSize(4096);
  Serial.begin(SERIAL_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);
  Serial.println("\n[main] SkyDisplay " FIRMWARE_VERSION " (" BOARD_NAME ") starting");
  settingsLoad();
  setenv("TZ", cfg.tz.c_str(), 1); tzset();
  Serial.printf("[main] PSRAM %u bytes, free heap %u\n", (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());
  uiBegin(cfg.brightness);
  uiMessage("SkyDisplay", "Connecting to Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.hostname.c_str());
  WiFiManager wm;
  wm.setDebugOutput(true, WM_DEBUG_VERBOSE);  // logs each request the setup page receives
  // Log devices joining the setup network and getting an address (diagnoses phones that can't open the page).
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    const uint8_t* m = info.wifi_ap_staconnected.mac;
    Serial.printf("[setup] device joined setup network: %02x:%02x:%02x:..\n", m[0], m[1], m[2]);
  }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    Serial.printf("[setup] gave address %s to a device\n", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
  }, ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);
  wm.setAPCallback(onPortal);
  wm.setConnectTimeout(20);
  wm.setHostname(cfg.hostname.c_str());
  if (!wm.autoConnect(SETUP_AP_NAME)) {
    uiMessage("Wi-Fi not set up", "Restarting...");
    delay(3000);
    ESP.restart();
  }
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // modem sleep drops multicast, which breaks skydisplay.local (mDNS); we're on USB power anyway
  Serial.printf("[main] Wi-Fi connected, IP %s\n", WiFi.localIP().toString().c_str());

  uiMessage("SkyDisplay", "Setting the clock...");
  applyTime();
  for (int i = 0; i < 40 && !nowUtc(); i++) delay(250);
  Serial.printf("[main] clock %s, NTP re-sync every %u h\n", nowUtc() ? "set" : "NOT set yet", (unsigned)(sntp_get_sync_interval() / 3600000UL));
  restoreCache();

  webBegin({webCheckNow, webApply, webStatus, webShowScreen});
  httpsBegin(cfg.hostname);  // HTTPS location page for "Use my location" (browsers need HTTPS for GPS)
  ui.f = &fc;
  ui.screen = homeScreen();
  if (cfg.obsEnabled) obsBegin();
  lastTouchMs = millis();
  // Network work (chart + weather) on core 0, so drawing and touch on core 1 never wait for it.
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

// Log anything that holds up the loop (touch is read in its own task, but the screen only
// responds once the loop gets back to it).
static uint32_t perfMaxDraw = 0;
static void perfNote(const char* what, uint32_t t0) {
  uint32_t dt = millis() - t0;
  if (!strcmp(what, "draw")) perfMaxDraw = max(perfMaxDraw, dt);
  if (dt > 300) Serial.printf("[perf] %s took %u ms\n", what, (unsigned)dt);
}

static uint32_t loopStartMs = 0, perfMaxLoop = 0;

void loop() {
  if (loopStartMs) perfMaxLoop = max(perfMaxLoop, millis() - loopStartMs - 10);  // minus the delay(10) at the end
  loopStartMs = millis();
  { DataGuard g; webLoop(); }

  if (Serial.available()) {
    int c = Serial.read();
    DataGuard g;
#if ARDUINO_USB_CDC_ON_BOOT
    if (c == 's') { Serial.setTxTimeoutMs(1000); uiScreenshot(Serial); Serial.flush(); Serial.setTxTimeoutMs(0); }  // someone is reading: wait for it
#else
    if (c == 's') { uiScreenshot(Serial); Serial.flush(); }
#endif
    else if (c == 'r') webCheckNow();
    else if (c == 'd') dumpRows();
    else if (c == 'a') printSky();
    else if (c == 'n') { stepScreen(1); lastTouchMs = millis(); drawScreen(); }  // next screen (testing)
    else if (c >= '0' && c <= '3') { ui.screen = c - '0'; lastTouchMs = millis(); if (ui.screen == SCREEN_OBS) nextObsMs = millis(); drawScreen(); }  // jump to a screen (testing)
    // Previews of the setup screens (testing); the next redraw returns to normal.
    else if (c == 'q') { uiWifiSetupScreen(SETUP_AP_NAME, "http://192.168.4.1/"); nextDrawMs = millis() + 15000; }
    else if (c == 'Q') { UiState p = ui; p.nowUtc = nowUtc(); p.settingsUrl = "http://" + WiFi.localIP().toString() + "/"; uiFirstRunScreen(p); nextDrawMs = millis() + 15000; }
    else if (c == 'i') {
      Serial.printf("[info] fw %s now %ld start %ld cols %d problem '%s' backlight %d ip %s heap %u (min %u) psram free %u, slowest draw %u ms, slowest loop pass %u ms\n",
                    FIRMWARE_VERSION, (long)nowUtc(), (long)fc.startUtc, fc.n, ui.problem.c_str(), currentBacklight,
                    WiFi.localIP().toString().c_str(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                    (unsigned)ESP.getFreePsram(), (unsigned)perfMaxDraw, (unsigned)perfMaxLoop);
      perfMaxLoop = 0; perfMaxDraw = 0;
    }
    else if (c == 'W') { WiFiManager wm; wm.resetSettings(); ESP.restart(); }
  }

  if (chartJustUpdated) { chartJustUpdated = false; DataGuard g; updateSky(); }
  if (redrawNeeded) { redrawNeeded = false; if (ui.screen != SCREEN_FORECAST || fc.valid) { nextDrawMs = 0; skipAdvance = true; } }
  if ((int32_t)(millis() - nextSkyMs) >= 0) { DataGuard g; updateSky(); nextDrawMs = 0; skipAdvance = true; }

  // Touch: tap = next footer page (or wake, if dimmed); long press = toggle dim.
  TouchEvent te = uiTouchEvent();
  if (te != TOUCH_NONE) {
    bool dimmed = currentBacklight < cfg.brightness;  // idle, off, night hours or long-press dim
    lastTouchMs = millis();
    // While night dimming applies, any touch keeps the screen bright for the idle period (at least a minute).
    uint32_t keepBright = (uint32_t)max(1, cfg.idleMinutes) * 60000UL;
    if (te == TOUCH_LONG) {
      manualDim = !dimmed;
      nightWakeUntilMs = manualDim ? millis() : millis() + keepBright;
      Serial.printf("[main] long press: dim %s\n", manualDim ? "on" : "off");
    } else if (dimmed) {  // any tap or swipe on a dimmed screen only wakes it
      manualDim = false;
      nightWakeUntilMs = millis() + keepBright;
      Serial.println("[main] touch: wake");
    } else {
      if (inNightHours()) nightWakeUntilMs = millis() + keepBright;
      if (te == TOUCH_SWIPE_LEFT || te == TOUCH_SWIPE_RIGHT) {
        stepScreen(te == TOUCH_SWIPE_LEFT ? 1 : -1);
        Serial.printf("[main] swipe: screen %d\n", ui.screen);
        if (ui.screen == SCREEN_OBS && (int32_t)(nextObsMs - millis()) > (int32_t)OBS_FAST_MS) nextObsMs = millis();  // fresh data soon
      } else if (ui.screen == SCREEN_FORECAST) {
        ui.footerPage = (ui.footerPage + 1) % FOOTER_PAGES;
        Serial.printf("[main] tap: footer page %d\n", ui.footerPage);
      } else if (ui.screen == SCREEN_OBS) {
        ui.rigPage++;  // next two rigs (wraps when drawn)
      } else if (ui.screen == SCREEN_WEATHER) {
        ui.wxText = !ui.wxText;  // small screen: text forecast <-> day strip (the square screen shows both)
      } else if (ui.screen == SCREEN_STATUS) {
        int tx, ty; uiTapPoint(tx, ty);
        if (uiStatusRefreshHit(tx, ty)) {  // same as "Check chart and weather now" on the settings page
          refreshPending = true; refreshTapMs = millis();
          webCheckNow();
          Serial.println("[main] tap: refresh now");
        } else Serial.printf("[main] tap at %d,%d\n", tx, ty);
      }
    }
    // Redraw now; the rotation timer restarts from here.
    { int bl = targetBacklight(); uiSetBacklight(bl); currentBacklight = bl; }
    if (currentBacklight > 0 && !webUpdating()) { uint32_t t0 = millis(); DataGuard g; drawScreen(); perfNote("draw", t0); }
    nextDrawMs = millis() + (uint32_t)cfg.footerSeconds * 1000;
    skipAdvance = true;
  }

  int bl = targetBacklight();
  if (bl != currentBacklight) { uiSetBacklight(bl); currentBacklight = bl; }

  // Footer rotation: advance a page, then draw. A tap already drew, so it skips one advance.
  if (ui.screen != homeScreen() && millis() - lastTouchMs > SCREEN_RETURN_MS) { ui.screen = homeScreen(); ui.wxText = false; nextDrawMs = 0; }
  if (ui.screen == SCREEN_OBS && !cfg.obsEnabled) ui.screen = SCREEN_FORECAST;  // turned off on the settings page
  if (webUpdating()) { delay(10); return; }  // keep the update message on screen
  if ((int32_t)(millis() - nextDrawMs) >= 0) {
    if (!skipAdvance && ui.screen == SCREEN_FORECAST) ui.footerPage = (ui.footerPage + 1) % FOOTER_PAGES;
    if (!skipAdvance && ui.screen == SCREEN_OBS && obs.nRigs > OBS_RIGS_PER_PAGE) ui.rigPage++;
    skipAdvance = false;
    if (currentBacklight > 0) { uint32_t t0 = millis(); DataGuard g; drawScreen(); perfNote("draw", t0); }  // no point drawing while the screen is off
    nextDrawMs = millis() + (uint32_t)cfg.footerSeconds * 1000;
  }
  delay(10);
}
