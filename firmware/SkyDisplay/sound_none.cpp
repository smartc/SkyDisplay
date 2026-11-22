// Boards without sound support (BOARD_HAS_SOUND 0 in board.h): everything is silent.
#include "board.h"
#if !BOARD_HAS_SOUND
#include "sound.h"

void soundBegin() {}
void soundLoop() {}
bool soundReady() { return false; }
String soundStatus() { return "No sound on this board"; }
void soundAlarm() {}
void soundChime() {}
void soundStop() {}
bool soundAlarmOn() { return false; }
bool soundHasVolume() { return false; }

#endif
