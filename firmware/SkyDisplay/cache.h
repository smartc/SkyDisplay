#pragma once
#include "forecast.h"
#include "weather.h"

// The last chart and weather, kept in flash (NVS, namespace "cache"), so a restart shows them straight away
// and only downloads what is due on the usual schedule. Each copy is tied to the chart ID or weather setup it
// came from; a different one (or a different firmware layout) reads as "no cache".

// Chart: the decoded forecast, its ETag and when it was last checked (UTC).
void cacheSaveChart(const Forecast& f, const String& chartKey, const String& etag, time_t checkedUtc);
void cacheSaveChartChecked(time_t checkedUtc);  // a check found the chart unchanged
bool cacheLoadChart(Forecast& f, const String& chartKey, String& etag, time_t& checkedUtc);

// Weather: everything but the error text, and when each part (WxPart order) last succeeded (UTC).
void cacheSaveWeather(const Weather& w, const String& setupKey, const time_t okUtc[3]);
bool cacheLoadWeather(Weather& w, const String& setupKey, time_t okUtc[3]);

void cacheLogUsage();  // NVS space, to the serial log
