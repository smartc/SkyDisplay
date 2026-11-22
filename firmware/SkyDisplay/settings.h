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
  int  startScreen = 0;             // screen shown at start-up and returned to when idle (0 forecast, 1 weather, 2 status, 3 observatory, 4 notifications)

  // Scoring (deep-sky)
  int goodMaxCloud = 20;            // %
  int goodMinTrans = 3;             // 3 = average
  int goMinHours = 4;

  // Weather Underground (personal weather station API)
  String wuStation = "";
  String wuKey = "";                // needed for the forecast (and for current conditions without EcoWitt)
  String ecowittHost = "";          // EcoWitt gateway address; when set, current conditions come from it (no API calls)

  // Observatory screen. Alpaca devices are written "host[:port][/device]" (port 11111 and device 0 by default);
  // a leading https:// is allowed for devices behind a proxy. Empty = not shown.
  bool   obsEnabled = false;
  String obsRoof = "";              // Alpaca Dome (roll-off roof)
  String obsSafety = "";            // Alpaca SafetyMonitor giving the safety verdict (e.g. Llama Watch)
  String obsRain = "";              // rain sensor: Alpaca SafetyMonitor address, or "udp" / "udp:<name>" for a UDP safety broadcast
  String obsSky = "";               // Alpaca ObservingConditions (cloud cover, SQM, sky brightness, sky temperature)
  static const int MAX_RIGS = 6;
  String rigName[MAX_RIGS], rigHost[MAX_RIGS];  // NINA Advanced API: "host[:port]" (port 1888 by default)
  String rigPark[MAX_RIGS];         // optional park sensor: the name or serial of one heard on the UDP broadcast, or an
                                    // Alpaca Switch address "host:port[/device][#switch]" (on = parked)
  int    obsHoldHour = 12;          // a finished (or lost) night's run stays on screen until this local hour

  // Notifications screen (unofficial Pushover Open Client). The secret and device id come from linking the display
  // on the settings page (the account password is never stored).
  bool   poEnabled = false;
  int    poWake = 1;                // new message: 0 just counts it, 1 high priority and up wake the screen and show it, 2 all do
  String poSecret = "", poDevice = "", poDeviceName = "";
  // Alarm sound (boards with BOARD_HAS_SOUND): which notifications sound it, and the on/off switch (also on the screen).
  int    alarmLevel = 1;            // 0 never, 1 emergency (beep-beep until acknowledged), 2 also high priority (one chime)
  bool   soundArmed = true;

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
