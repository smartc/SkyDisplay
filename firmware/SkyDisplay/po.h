#pragma once
// Notifications screen: the display is an unofficial Pushover Open Client (https://pushover.net/api/client),
// not released or supported by Pushover. It registers as a device on the user's account, listens on Pushover's
// WebSocket and downloads new messages when told to. Messages are deleted from Pushover's servers once
// downloaded, so the display keeps the latest PO_MAX itself, in a file on its data partition.
#include <Arduino.h>
#include <time.h>

static const int PO_MAX = 20;

struct PoMsg {
  uint64_t id = 0;           // Pushover's message id for this device (larger than 32 bits)
  uint32_t date = 0;         // UTC
  int8_t   priority = 0;     // -2 lowest .. 2 emergency
  uint8_t  acked = 0;        // emergency messages: 0 not yet, 1 acknowledged, 2 being sent
  char     app[32] = "";
  char     title[80] = "";   // empty: show app
  char     text[480] = "";   // plain text (HTML tags removed); '\n' between paragraphs
  char     receipt[32] = ""; // emergency messages: for acknowledging
};

enum PoState { PO_OFF, PO_NOT_LINKED, PO_PAUSED, PO_CONNECTING, PO_CONNECTED, PO_RETRYING, PO_STOPPED };

struct PoInbox {
  PoMsg  msg[PO_MAX];        // newest first
  int    n = 0;
  int    unread = 0;         // arrived since the screen was last shown
  uint32_t arrivals = 0;     // counts downloads that brought new messages, so the main loop can react (wake, switch screen)
  uint32_t changes = 0;      // counts any change to the messages (arrival, acknowledged, cleared), for redrawing
  int8_t newestPriority = 0;
  PoState state = PO_OFF;
  String  error;             // why it stopped or is retrying, empty if fine
};

extern PoInbox poInbox;      // guarded by the data lock (settings.h)

void poBegin();              // mount storage, load saved messages, start the background task
void poSyncNow();            // download now (e.g. "Check now")
void poRestart();            // settings changed (linked, unlinked, turned on or off)
void poAcknowledge(int index);  // emergency message at index: acknowledge it (user action only)
void poDelete(int index);    // forget one stored message
void poClear();              // forget all stored messages

// Settings page: log in and register this display as a device. Runs in the web handler (a few seconds).
// Returns "" on success; "2fa" when a two-factor code is needed; otherwise a short error.
String poLink(const String& email, const String& password, const String& twofa, const String& deviceName);
void poUnlink();

// Monitoring was switched on, off or to Auto, or the auto-off hour changed: schedule (or cancel) the automatic
// switch-off, at the next cfg.poAutoOff:00 local time that is at least 3 hours away. Doesn't save the settings.
void poListenChanged();

// Readable state for the status screen and settings page.
String poStateText();
