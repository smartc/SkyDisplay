#pragma once
#include <Arduino.h>
#include <time.h>

// Weather Underground personal weather station data (metric units).
static const int WX_TRACE = 24;  // 24 h at hourly steps
static const int WX_DAYS = 5;    // tonight + next 4 days

struct WxDay {
  char name[10] = "";            // "TONIGHT", "SAT", ...
  float hi = NAN, lo = NAN;      // degrees C (hi is NAN for "tonight")
  int pop = -1, cloud = -1;      // night precip chance and cloud cover, %
  int icon = -1;                 // WU icon code for the night
};

struct Weather {
  // current observation
  bool   haveCurrent = false;
  time_t obsUtc = 0;
  float  temp = NAN, feels = NAN, dew = NAN, wind = NAN, gust = NAN, pressure = NAN, rainToday = NAN;
  int    humidity = -1, windDir = -1;
  // last 24 hours
  bool   haveHistory = false;
  float  traceT[WX_TRACE], traceD[WX_TRACE];  // NAN where no data
  float  change1h = NAN;                       // temperature change over the last hour
  float  pressure3h = NAN;                     // pressure change over 3 hours
  // forecast
  bool   haveForecast = false;
  int    cloudTonight = -1;
  WxDay  days[WX_DAYS];
  // status
  String error;                                // last error, empty if fine
};

enum WxPart { WX_CURRENT, WX_HISTORY, WX_FORECAST };

// Fetch one part into w. Returns false (and sets w.error) on failure; other parts are untouched.
// Takes the station and key as arguments (not from cfg) because it runs in the network task.
bool weatherFetch(WxPart part, Weather& w, const String& station, const String& key, double lat, double lon);
bool weatherConfigured();
