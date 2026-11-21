#pragma once
#include <Arduino.h>

// Callbacks into the main sketch.
struct WebHooks {
  void (*checkNow)();            // re-check the chart
  void (*applySettings)(bool chartChanged, bool timeChanged);
  String (*statusLine)();        // one-line status for the page
};

void webBegin(const WebHooks& hooks);
void webLoop();
bool webUpdating();  // true while a firmware upload is in progress
