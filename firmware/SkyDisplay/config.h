#pragma once
// Fixed constants. Anything a user might change lives in settings.h and the web page.

#define FIRMWARE_VERSION "0.6.5"

// Show "stale" after this long without a successful chart check.
#define STALE_MINUTES    180

// Scoring details not exposed on the settings page.
#define FAIR_MAX_CLOUD   40   // % cloud for a fair hour
#define FAIR_MIN_TRANS   2
#define MAYBE_MIN_RUN    2
#define MAYBE_MIN_TOTAL  4    // good + fair hours for MAYBE
#define AGREE_MAX_SPREAD 3    // model spread (0-10 scale) still counted as agreeing
#define AGREE_MIN_SHARE  0.75f

// Wi-Fi setup access point name (shown on screen when no Wi-Fi is saved)
#define SETUP_AP_NAME    "SkyDisplay-Setup"
