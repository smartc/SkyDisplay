#pragma once
// Observatory screen data: roof, safety, rain, sky conditions (ASCOM Alpaca), mount park sensors and
// safety sensors (UDP broadcast, Dark Dragons park-sensor JSON on port 23435) and imaging rigs (NINA
// Advanced API plugin).
#include <Arduino.h>
#include <time.h>
#include "settings.h"

static const int OBS_MAX_PARK = 8;
static const int OBS_MAX_RIGS = Settings::MAX_RIGS;

// Alpaca Dome shutter states
enum { SHUTTER_OPEN = 0, SHUTTER_CLOSED = 1, SHUTTER_OPENING = 2, SHUTTER_CLOSING = 3, SHUTTER_ERROR = 4 };

// A rig's pier, from the park sensor tied to it.
enum { PIER_NONE = -1, PIER_OFFLINE = 0, PIER_PARKED = 1, PIER_UNPARKED = 2 };

enum RigState {
  RIG_UNKNOWN,   // not polled yet
  RIG_INACTIVE,  // NINA not running (normal outside imaging runs)
  RIG_IDLE,      // NINA running, no sequence running
  RIG_IMAGING,   // sequence running and the camera is exposing / guiding
  RIG_WAITING,   // sequence running, between targets or waiting (for dark, the roof, a target to rise...)
  RIG_FINISHED,  // the night's sequence finished; shown until the hold hour
  RIG_LOST,      // NINA stopped answering in the middle of a sequence; shown until the hold hour
  RIG_ERROR      // NINA answers but the sequence failed
};

struct ObsRoof {
  bool configured = false, online = false;
  int shutter = -1;
  String name, error;
  time_t since = 0;   // when the shutter state last changed (0 = not seen change since boot)
};
struct ObsSafety { bool configured = false, online = false, safe = false; String reason; };
struct ObsRain { bool configured = false, online = false, wet = false; };
struct ObsSky { bool configured = false, online = false; float cloud = NAN, sqm = NAN, lux = NAN, skyT = NAN, ambient = NAN; };
struct ObsPark { String id, name; bool parked = false; bool online = false; };

struct ObsRig {
  String name;
  bool configured = false, online = false;
  RigState state = RIG_UNKNOWN;
  int pier = PIER_NONE;              // park sensor tied to this rig (PIER_NONE when none is set)
  bool seqRunning = false, exposing = false, parked = false, tracking = false;
  String guider;                     // Guiding, LostLock, Looping, Stopped, or empty
  float rms = NAN;                   // total guiding RMS, arcsec
  // latest light frame
  String target, filter;
  float exposure = NAN, hfr = NAN;
  int frames = 0;                    // light frames in NINA's image history (this session)
  time_t lastImage = 0;              // when a new frame was first seen
  // session tracking (kept in RAM only)
  bool session = false;              // a sequence ran since NINA was last seen idle
  time_t endedAt = 0;                // when the run finished or NINA was lost
  time_t lastSeen = 0;
  String lastRun;                    // summary of the previous run, for the inactive state
  String error;
};

struct Observatory {
  ObsRoof roof;
  ObsSafety safety;
  ObsRain rain;
  ObsSky sky;
  ObsPark park[OBS_MAX_PARK]; int nPark = 0;
  ObsRig rigs[OBS_MAX_RIGS]; int nRigs = 0;
  time_t polled = 0;
};

// Snapshot of the settings the network task needs (taken under the data lock).
struct ObsConfig {
  String roof, safety, rain, sky;
  String rigName[OBS_MAX_RIGS], rigHost[OBS_MAX_RIGS], rigPark[OBS_MAX_RIGS];
  int holdHour = 12;
};

void obsBegin();                                  // start listening for park / safety sensor broadcasts
void obsPoll(Observatory& o, const ObsConfig& c); // network task: poll everything once
String obsDiscoverJson();                         // Alpaca discovery for the settings page: [{"addr","type","name"}]
int obsHeardParkSensors(String* names, int maxN); // names of park sensors heard on UDP (for the settings page)
