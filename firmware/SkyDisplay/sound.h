#pragma once
// Alarm sound. One interface for every board; each board's file (sound_indicator.cpp, ...) implements it, and boards
// without sound (BOARD_HAS_SOUND 0 in board.h) get the silent version in sound_none.cpp.
#include <Arduino.h>

void   soundBegin();
void   soundLoop();        // call often from the main loop: runs the patterns and reads the sound chip's replies
bool   soundReady();       // the sound hardware answered (on the Indicator: SkyDisplay's RP2040 firmware is installed)
String soundStatus();      // short text for the status screen and settings page
String soundFirmware();    // e.g. "RP2040 firmware v2" (About screen)

void   soundAlarm();       // beep-beep every 3 s until soundStop(), or for 5 minutes at most
void   soundChime();       // one short two-note chime
void   soundStop();
bool   soundAlarmOn();     // the alarm is sounding
bool   soundHasVolume();   // the volume setting works (on the Indicator: RP2040 firmware v2 or later)
