#pragma once
#include "forecast.h"
#include "settings.h"

enum FetchResult { FETCH_UPDATED, FETCH_UNCHANGED, FETCH_FAILED };

// Check the chart at ct. On FETCH_UPDATED, f holds the newly decoded forecast.
// On failure, f is left untouched and err says why. Runs in the network task.
FetchResult fetchChart(Forecast& f, const ChartTarget& ct, String& err);
// Forget the cached ETag so the next check downloads the chart again (e.g. after the URL changes).
void netForgetEtag();
// ETag of the chart now held, for the flash cache (read and set from the network task, or before it starts).
String netEtag();
void netSetEtag(const String& e);
