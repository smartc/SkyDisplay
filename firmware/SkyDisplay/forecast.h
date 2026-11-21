#pragma once
#include <Arduino.h>
#include <time.h>

static const int MAX_COLS = 100;

// One decoded Clear Sky Chart. Values use the chart's own scales;
// -1 means "no data" (a black cell, e.g. HRRR past its range).
struct Forecast {
  bool    valid = false;
  time_t  startUtc = 0;      // time of column 0
  double  siteLat = 0, siteLon = 0;  // from the chart page (0,0 = not found)
  int     n = 0;             // number of hourly columns
  int8_t  rdps[MAX_COLS], ecmwf[MAX_COLS], hrrr[MAX_COLS];  // cloud 0 overcast .. 10 clear
  int8_t  trans[MAX_COLS], seeing[MAX_COLS];                 // 0 too cloudy, 1..5
  int8_t  wind[MAX_COLS];                                    // 0 strongest .. 5 calm
  int8_t  hum[MAX_COLS];                                     // 0 (<25%) .. 15 (95-100%)
  uint32_t dark[MAX_COLS];                                   // raw RGB888 of darkness cell
  bool    night[MAX_COLS];                                   // after astronomical twilight (moon or not)
};

// Decode chart.gif bytes into f (startUtc must be set by the caller).
// Returns an empty string on success, otherwise a short reason.
String decodeChartGif(const uint8_t* data, int len, Forecast& f);

// Parse "2026-11-14T07:00:00Z" into a UTC time_t. Returns 0 on failure.
time_t parseIsoUtc(const char* s);

// ---------- scoring ----------
struct HourEval {
  bool ok = false;
  int  q = 0;          // 0 bad, 1 fair, 2 good
  int  cover = 100;    // % cloud (mean of available models)
  int  spread = 0;     // max-min of model values
  int  t = 0;          // transparency
  float mean = 0;      // mean cloud value 0..10
};

struct Night { int s, e; bool partial; };

enum Word { W_NONE, W_DAY, W_GO, W_MAYBE, W_NO, W_CLEAR, W_FAIR };

struct Verdict {
  Word word = W_NONE;
  char l1[40] = "";
  char l2[40] = "";
  int  hum = 0, wind = 5;  // worst humidity / calmest wind over the night (chart scales)
  int  moonFrom = -1, moonTo = -1, moonHours = 0;  // moonlit columns within the night
  bool hasNight = false;
};

bool     isDark(const Forecast& f, int i);     // after astronomical twilight, moon or not
bool     isMoonlit(const Forecast& f, int i);  // dark, but with the moon up
HourEval hourEval(const Forecast& f, int i);
int      findNights(const Forecast& f, Night* out, int maxN);
Verdict  rateNight(const Forecast& f, const Night* n);
Verdict  rateNow(const Forecast& f, int i, const Night* nights, int nn);
int      localHour(const Forecast& f, int i);
