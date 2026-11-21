// SkyDisplay: bedside astronomy forecast for the WT32-SC01 Plus (ESP32-S3).
// Reads the Dark Dragons Clear Sky Chart GIF, decodes the forecast rows and shows
// whether now, tonight and tomorrow night are worth setting up the rigs.
// Settings page: http://skydisplay.local/ (or the IP shown in the serial log).
//
// Screens (swipe left/right): forecast, weather (Weather Underground), status.
// Touch: tap = next footer page, long press = dim.
// Serial commands (115200): s = screenshot dump, r = check chart and weather now, d = dump decoded rows,
//   a = sun/moon times, n = next screen, i = info, W = forget Wi-Fi and restart.

#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_sntp.h>
#include "config.h"
#include "settings.h"
#include "forecast.h"
#include "net.h"
#include "astro.h"
#include "ui.h"
#include "web.h"
#include "weather.h"

static Forecast fc;
static UiState ui;
static Weather wx;
// Weather polling: current every 5 min, 24 h history every 15 min, forecast hourly (~410 calls/day of 1,500).
static volatile uint32_t nextWxMs[3] = {0, 0, 0};
static const uint32_t WX_PERIOD_MS[3] = {5UL * 60000, 15UL * 60000, 60UL * 60000};
static String wxSetupKey;  // station + key the current data came from; a change resets the weather
static volatile uint32_t nextCheckMs = 0;  // shared with the network task
static uint32_t nextDrawMs = 0, nextSkyMs = 0, lastTouchMs = 0;
static bool manualDim = false;
static uint32_t nightWakeUntilMs = 0;  // a tap during night hours brightens the screen until this time
static bool skipAdvance = true;         // draw the current footer page without advancing first
static const uint32_t SCREEN_RETURN_MS = 120000;  // other screens return to the forecast after this long untouched
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
  else R("WEATHER", cfg.wuStation + (wx.haveCurrent ? ", " + fmtLocal(wx.obsUtc, "%H:%M") : String(", waiting")));
  st.qrUrl = "http://" + WiFi.localIP().toString() + "/";
}

static void drawScreen() {
  ui.nowUtc = nowUtc();
  if (ui.screen == SCREEN_STATUS) { StatusInfo st; buildStatus(st); uiDrawStatus(ui, st); }
  else if (ui.screen == SCREEN_WEATHER) uiDrawWeather(ui, wx, weatherConfigured(), "http://" + WiFi.localIP().toString() + "/");
  else uiDraw(ui);
}

static int currentBacklight = -1;
static volatile bool chartJustUpdated = false;  // set by the network task; the loop then recomputes sun/moon
static volatile bool redrawNeeded = false;      // set by the network task when shown data changed

static time_t nowUtc() { time_t t = time(nullptr); return t > 1700000000 ? t : 0; }

static void onPortal(WiFiManager*) {
  uiMessage("Wi-Fi setup",
            "Join the Wi-Fi network \"" SETUP_AP_NAME "\" from a phone or laptop,",
            "then browse to http://192.168.4.1",
            "and pick your network.");
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
  DataGuard g;
  if (r == FETCH_FAILED) {
    logf("[net] chart check failed: %s\n", err.c_str());
    ui.problem = err.substring(0, 40);
    nextCheckMs = millis() + 5UL * 60 * 1000;  // retry sooner after a failure
  } else {
    ui.problem = "";
    ui.lastCheckOk = nowUtc();
    if (r == FETCH_UPDATED) { fc = work; chartJustUpdated = true; logf("[main] chart updated: %d columns\n", fc.n); }
    nextCheckMs = millis() + (uint32_t)cfg.pollMinutes * 60 * 1000;
  }
  logf("[net] chart check %u ms\n", (unsigned)(millis() - t0));
  redrawNeeded = true;
}

// At most one weather request per pass.
static void netPollWeather() {
  String station, key; double lat, lon; Weather work; bool configured;
  {
    DataGuard g;
    String setup = cfg.wuStation + "|" + cfg.wuKey;
    if (setup != wxSetupKey) { wx = Weather(); wxSetupKey = setup; nextWxMs[0] = nextWxMs[1] = nextWxMs[2] = millis(); redrawNeeded = true; }
    configured = weatherConfigured();
    station = cfg.wuStation; key = cfg.wuKey;
    lat = cfg.lat; lon = cfg.lon;
    if (lat == 0 && lon == 0) { lat = fc.siteLat; lon = fc.siteLon; }
    work = wx;
  }
  if (!configured) return;
  for (int p = 0; p < 3; p++) {
    if ((int32_t)(millis() - nextWxMs[p]) < 0) continue;
    if (p == WX_FORECAST && !work.haveCurrent) continue;  // fetch current first
    bool ok = weatherFetch((WxPart)p, work, station, key, lat, lon);
    // On failure retry in 2 minutes (a rejected key waits the full period, to stay well inside the call limit).
    bool keyProblem = work.error.indexOf("API key") >= 0 || work.error.indexOf("not found") >= 0;
    DataGuard g;
    if (wxSetupKey == station + "|" + key) wx = work;  // ignore results if the settings changed meanwhile
    nextWxMs[p] = millis() + (ok || keyProblem ? WX_PERIOD_MS[p] : 2UL * 60000);
    redrawNeeded = true;
    return;
  }
}

static void netTask(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && nowUtc()) {
      if ((int32_t)(millis() - nextCheckMs) >= 0) netCheckChart();
      netPollWeather();
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ---------- web hooks ----------
static void webCheckNow() { nextCheckMs = 0; nextWxMs[0] = nextWxMs[1] = nextWxMs[2] = millis(); }
static void webApply(bool chartChanged, bool timeChanged) {
  if (chartChanged) { fc.valid = false; netForgetEtag(); nextCheckMs = 0; }
  if (timeChanged) { applyTime(); nextSkyMs = 0; }
  currentBacklight = -1;  // re-apply brightness
  uint32_t soonest = millis() + (uint32_t)cfg.pollMinutes * 60 * 1000;
  if ((int32_t)(nextCheckMs - soonest) > 0) nextCheckMs = soonest;
  nextDrawMs = 0;
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
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(300);
  Serial.println("\n[main] SkyDisplay " FIRMWARE_VERSION " starting");
  settingsLoad();
  setenv("TZ", cfg.tz.c_str(), 1); tzset();
  Serial.printf("[main] PSRAM %u bytes, free heap %u\n", (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());
  uiBegin(cfg.brightness);
  uiMessage("SkyDisplay", "Connecting to Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.hostname.c_str());
  WiFiManager wm;
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

  webBegin({webCheckNow, webApply, webStatus});
  ui.f = &fc;
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
    if (c == 's') { Serial.setTxTimeoutMs(1000); uiScreenshot(Serial); Serial.flush(); Serial.setTxTimeoutMs(0); }  // someone is reading: wait for it
    else if (c == 'r') webCheckNow();
    else if (c == 'd') dumpRows();
    else if (c == 'a') printSky();
    else if (c == 'n') { ui.screen = (ui.screen + 1) % SCREEN_COUNT; lastTouchMs = millis(); drawScreen(); }  // next screen (testing)
    else if (c >= '0' && c <= '2') { ui.screen = c - '0'; lastTouchMs = millis(); drawScreen(); }      // jump to a screen (testing)
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
        int d = te == TOUCH_SWIPE_LEFT ? 1 : -1;
        ui.screen = (ui.screen + d + SCREEN_COUNT) % SCREEN_COUNT;
        Serial.printf("[main] swipe: screen %d\n", ui.screen);
      } else if (ui.screen == SCREEN_FORECAST) {
        ui.footerPage = (ui.footerPage + 1) % FOOTER_PAGES;
        Serial.printf("[main] tap: footer page %d\n", ui.footerPage);
      }
    }
    // Redraw now; the rotation timer restarts from here.
    { int bl = targetBacklight(); uiSetBacklight(bl); currentBacklight = bl; }
    if (currentBacklight > 0) { uint32_t t0 = millis(); DataGuard g; drawScreen(); perfNote("draw", t0); }
    nextDrawMs = millis() + (uint32_t)cfg.footerSeconds * 1000;
    skipAdvance = true;
  }

  int bl = targetBacklight();
  if (bl != currentBacklight) { uiSetBacklight(bl); currentBacklight = bl; }

  // Footer rotation: advance a page, then draw. A tap already drew, so it skips one advance.
  if (ui.screen != SCREEN_FORECAST && millis() - lastTouchMs > SCREEN_RETURN_MS) { ui.screen = SCREEN_FORECAST; nextDrawMs = 0; }
  if ((int32_t)(millis() - nextDrawMs) >= 0) {
    if (!skipAdvance && ui.screen == SCREEN_FORECAST) ui.footerPage = (ui.footerPage + 1) % FOOTER_PAGES;
    skipAdvance = false;
    if (currentBacklight > 0) { uint32_t t0 = millis(); DataGuard g; drawScreen(); perfNote("draw", t0); }  // no point drawing while the screen is off
    nextDrawMs = millis() + (uint32_t)cfg.footerSeconds * 1000;
  }
  delay(10);
}
