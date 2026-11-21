#pragma once
#include <Arduino.h>

// Everything adjustable from the web page. Stored in NVS (survives reflashing).
struct Settings {
  // Chart
  String chartUrl  = "";            // e.g. https://cds.darkdragonsastro.com/g/<chart id>.html; set on the settings page
  int    pollMinutes = 30;

  // Location and time
  double lat = 0, lon = 0;          // 0,0 = take from the chart page
  String tz   = "UTC0";             // POSIX TZ string, e.g. "MST7MDT,M3.2.0,M11.1.0"; set on the settings page
  String ntp1 = "pool.ntp.org";
  String ntp2 = "time.google.com";  // optional fallback; empty = none
  int    ntpHours = 3;              // re-sync interval, hours (1..24)

  // Display
  int  brightness = 200;            // 1..255
  int  idleMode = 0;                // 0 always on, 1 dim when idle, 2 screen off when idle
  int  idleMinutes = 10;
  int  dimBrightness = 12;          // used for idle dim, night hours and tap-to-dim
  bool nightDim = false;            // dim between nightFrom and nightTo (local hours)
  int  nightFrom = 22, nightTo = 7;
  int  footerSeconds = 4;           // footer rotation
  int  startScreen = 0;             // screen shown at start-up and returned to when idle (0 forecast, 1 weather, 2 status)

  // Scoring (deep-sky)
  int goodMaxCloud = 20;            // %
  int goodMinTrans = 3;             // 3 = average
  int goMinHours = 4;

  // Weather Underground (personal weather station API)
  String wuStation = "";
  String wuKey = "";                // empty = weather screen shows setup instructions

  // Device
  String hostname = "skydisplay";
  String webPassword = "";          // empty = no password on the settings page
};

extern Settings cfg;

void settingsLoad();
void settingsSave();

// One lock for settings and shared data (forecast, weather, status). The network task runs on the
// other core, so anything it shares with the screen or the web page is touched only while holding it.
void dataLock();
void dataUnlock();
struct DataGuard { DataGuard() { dataLock(); } ~DataGuard() { dataUnlock(); } };

// Serial log for the network task: takes the lock, so a line can't land in the middle of a screenshot transfer.
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Parsed from cfg.chartUrl. Accepts a full chart page URL or just the chart ID.
struct ChartTarget { String host, key, pagePath, gifPath; bool ok; };
ChartTarget chartTarget();
