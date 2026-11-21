#pragma once
#include <time.h>

// Low-precision sun and moon positions (Astronomical Almanac formulas).
// Checked against the astral Python library: rise/set times within ~3 minutes.

struct SkyEvents {
  time_t sunset = 0, sunrise = 0;       // next after the reference time, 0 = none within 36 h
  time_t duskAstro = 0, dawnAstro = 0;  // sun at -18 degrees
  time_t moonrise = 0, moonset = 0;
  float  moonIllum = 0;                 // 0..1
  bool   moonWaxing = true;
};

SkyEvents computeSkyEvents(time_t from, double latDeg, double lonDeg);
const char* moonPhaseName(float illum, bool waxing);
