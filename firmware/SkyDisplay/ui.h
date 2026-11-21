#pragma once
#include "forecast.h"
#include "astro.h"
#include "weather.h"

struct UiState {
  const Forecast* f = nullptr;
  time_t nowUtc = 0;        // 0 = clock not set yet
  time_t lastCheckOk = 0;   // last successful chart check (UTC)
  String problem;           // short error to show in the header, if any
  SkyEvents sky;            // sun and moon events from now
  bool skyValid = false;    // false until we have a location
  int footerPage = 0;       // which footer page to show
  int screen = 0;           // Screen enum below; 0 = forecast
};

static const int FOOTER_PAGES = 3;

// Screens, in swipe order.
enum Screen { SCREEN_FORECAST = 0, SCREEN_WEATHER = 1, SCREEN_STATUS = 2, SCREEN_COUNT = 3 };

// Status screen content, filled in by the main sketch.
struct StatusItem { String label, value; bool warn = false; };
struct StatusInfo {
  StatusItem left[8], right[8];
  int nLeft = 0, nRight = 0;
  String qrUrl;   // settings page link, shown as a QR code
};

void uiBegin(uint8_t brightness);
void uiSetBacklight(uint8_t level);
void uiDraw(const UiState& s);                           // forecast screen
void uiDrawStatus(const UiState& s, const StatusInfo& st); // status screen
// Weather screen. settingsUrl is shown when the station or key isn't set up yet.
void uiDrawWeather(const UiState& s, const Weather& w, bool configured, const String& settingsUrl);
// Full-screen message (setup, connecting, errors before the first chart).
void uiMessage(const char* title, const char* line1, const char* line2 = "", const char* line3 = "");
enum TouchEvent { TOUCH_NONE, TOUCH_TAP, TOUCH_LONG, TOUCH_SWIPE_LEFT, TOUCH_SWIPE_RIGHT };
// Call often. TAP fires on release of a short press; LONG fires once while a press is held
// for LONG_PRESS_MS. A mostly horizontal drag of 60 px or more is a swipe (fires on release).
static const uint32_t LONG_PRESS_MS = 700;
TouchEvent uiTouchEvent();
void uiScreenshot(Stream& out); // raw RGB565 dump of the last frame, for debugging
