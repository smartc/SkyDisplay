#pragma once
#include "forecast.h"
#include "astro.h"
#include "weather.h"
#include "obs.h"
#include "po.h"

struct UiState {
  const Forecast* f = nullptr;
  time_t nowUtc = 0;        // 0 = clock not set yet
  time_t lastCheckOk = 0;   // last successful chart check (UTC)
  String problem;           // short error to show in the header, if any
  SkyEvents sky;            // sun and moon events from now
  bool skyValid = false;    // false until we have a location
  int footerPage = 0;       // which footer page to show
  int screen = 0;           // Screen enum below; 0 = forecast
  bool needsSetup = false;  // no chart configured yet: forecast screen shows the first-run QR code
  String settingsUrl;       // e.g. http://192.168.1.50/ (the display's address)
  int dotIndex = 0, dotCount = 3;  // header dots: position of this screen in the swipe order
  int rigPage = 0;          // observatory screen: which two rigs are shown
  bool wxText = false;      // weather screen on the small display: text forecast shown in place of the day strip
  int poIndex = 0;          // notifications screen: which message is shown in full
  int dotAlert = -1;        // header dot drawn amber (the notifications screen, while messages are unread)
  int sound = -1;           // notifications screen sound button: -1 not shown (no sound on this board), 0 off, 1-3 volume
  bool soundLevels = false; // the button cycles volumes (Off, High, Med, Low); otherwise just On / Off
  bool soundMissing = false;  // the board has sound but its hardware didn't answer
  bool alarmSounding = false;
  bool poListen = true;     // notifications monitoring on (the screen's Monitoring switch)
  bool poAuto = false;      // ... in Auto mode
  time_t poUntil = 0;       // ... switching itself off at this time (0 = not scheduled)
  bool poConfirmAll = false;  // "Clear all" tapped once: the button asks for a second tap
};

static const int FOOTER_PAGES = 3;

// Screens. Swipe order is forecast, weather, observatory and notifications (each when turned on), status.
enum Screen { SCREEN_FORECAST = 0, SCREEN_WEATHER = 1, SCREEN_STATUS = 2, SCREEN_OBS = 3, SCREEN_PO = 4, SCREEN_COUNT = 5 };
static const int OBS_RIGS_PER_PAGE = 2;

// Status screen content, filled in by the main sketch.
struct StatusItem { String label, value; bool warn = false; };
struct StatusInfo {
  StatusItem left[8], right[8];
  int nLeft = 0, nRight = 0;
  String qrUrl;   // settings page link, shown as a QR code
  bool refreshing = false;  // "Refresh now" was tapped and the check hasn't finished yet
};

void uiBegin(uint8_t brightness);
void uiSetBacklight(uint8_t level);
void uiDraw(const UiState& s);                           // forecast screen
void uiDrawStatus(const UiState& s, const StatusInfo& st); // status screen
// Weather screen. settingsUrl is shown when the station or key isn't set up yet.
void uiDrawWeather(const UiState& s, const Weather& w, bool configured, const String& settingsUrl);
// Observatory screen.
void uiDrawObservatory(const UiState& s, const Observatory& o, const String& settingsUrl);
// Notifications screen: the message at s.poIndex in full, the next ones as a list. linked = the display is a
// device on a Pushover account.
void uiDrawNotifications(const UiState& s, const PoInbox& in, bool linked, const String& deviceName, const String& settingsUrl);
// What a tap at (x, y) on the notifications screen hits. PO_TAP_NEWER / PO_TAP_OLDER: the top or bottom half
// of the screen away from the buttons.
enum PoTap { PO_TAP_NEWER, PO_TAP_OLDER, PO_TAP_SOUND, PO_TAP_MONITOR, PO_TAP_CLEAR, PO_TAP_CLEAR_ALL, PO_TAP_ACK };
PoTap uiPoTap(const UiState& s, const PoInbox& in, int x, int y);
// Wi-Fi setup: QR code to join the setup network (open, no password) and QR code for the setup page.
void uiWifiSetupScreen(const char* apName, const String& setupUrl);
// First run after Wi-Fi: QR code for the settings page, to add the chart and time zone.
void uiFirstRunScreen(const UiState& s);
// Full-screen message (setup, connecting, errors before the first chart).
void uiMessage(const char* title, const char* line1, const char* line2 = "", const char* line3 = "");
enum TouchEvent { TOUCH_NONE, TOUCH_TAP, TOUCH_LONG, TOUCH_SWIPE_LEFT, TOUCH_SWIPE_RIGHT };
// Call often. TAP fires on release of a short press; LONG fires once while a press is held
// for LONG_PRESS_MS. A mostly horizontal drag of 60 px or more is a swipe (fires on release).
static const uint32_t LONG_PRESS_MS = 700;
TouchEvent uiTouchEvent();
void uiTapPoint(int& x, int& y);           // where the last TOUCH_TAP was (screen pixels)
bool uiStatusRefreshHit(int x, int y);     // that point is on the status screen's "Refresh now" button
void uiScreenshot(Stream& out); // raw RGB565 dump of the last frame, for debugging
