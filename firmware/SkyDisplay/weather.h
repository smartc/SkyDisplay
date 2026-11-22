#pragma once
#include <Arduino.h>
#include <time.h>

// Weather: current conditions from an EcoWitt gateway on the local network (no API calls) or a Weather
// Underground personal weather station; the forecast from Weather Underground. Metric units.
static const int WX_TRACE = 24;  // 24 h at hourly steps
static const int WX_DAYS = 5;    // tonight + next 4 days

struct WxDay {
  char name[10] = "";            // "TODAY", "TONIGHT", "SAT", ...
  float hi = NAN, lo = NAN;      // degrees C (hi is NAN for "tonight", lo is NAN for "today")
  int pop = -1, cloud = -1;      // precip chance and cloud cover, % (night for the day cells, daytime for today)
  int icon = -1;                 // WU icon code (night, or daytime for today)
  bool valid = false;
};

// Everything except the text fields, kept as plain data so it can be saved to flash in one piece (cache.cpp).
struct WxData {
  // current observation
  bool   haveCurrent = false;
  time_t obsUtc = 0;
  float  temp = NAN, feels = NAN, dew = NAN, wind = NAN, gust = NAN, pressure = NAN, rainToday = NAN;
  int    humidity = -1, windDir = -1;
  // last 24 hours, built on the device from hourly averages of the current readings (seeded from WU history
  // at start-up when a station is set), so the trace costs no API calls
  bool   haveHistory = false;
  float  slotT[WX_TRACE] = {}, slotD[WX_TRACE] = {}, slotP[WX_TRACE] = {};
  uint16_t slotNT[WX_TRACE] = {}, slotND[WX_TRACE] = {}, slotNP[WX_TRACE] = {};
  uint32_t slotHour[WX_TRACE] = {};            // hours since 1970 that each slot holds
  float  traceT[WX_TRACE], traceD[WX_TRACE];  // NAN where no data
  float  change1h = NAN;                       // temperature change over the last hour
  float  pressure3h = NAN;                     // pressure change over 3 hours
  // forecast
  bool   haveForecast = false;
  int    cloudTonight = -1;
  WxDay  today;                                // today's daytime forecast; not valid once the day part has passed
  WxDay  days[WX_DAYS];                        // tonight, then the next four days (night conditions)
};

struct Weather : WxData {
  String source;                               // where current conditions came from: "EcoWitt" or the WU station ID
  String forecastTitle, forecastText;          // WU text forecast: "TODAY" (daytime) or "TONIGHT" once the day is over
  // status
  String error;                                // last error from current conditions, empty if fine
  String wuError;                              // last Weather Underground forecast/history error (kept separately so
                                               // a working EcoWitt gateway doesn't hide it), empty if fine
};

enum WxPart { WX_CURRENT, WX_HISTORY, WX_FORECAST };

// Settings snapshot for the network task (it must not read cfg directly).
struct WxSetup {
  String station, key;   // Weather Underground station ID and API key (the key is needed for the forecast)
  String ecowitt;        // EcoWitt gateway address; when set, current conditions come from it
  double lat = 0, lon = 0;
};

// Fetch one part into w. Returns false (and sets w.error) on failure; other parts are untouched.
// WX_CURRENT reads the EcoWitt gateway when one is set, else the WU station. WX_HISTORY (WU) only seeds the trace.
bool weatherFetch(WxPart part, Weather& w, const WxSetup& s);
bool weatherConfigured();      // some source of current conditions is set up
bool weatherHasForecast();     // a WU API key is set (needed for the forecast)
bool weatherUsesEcowitt();
