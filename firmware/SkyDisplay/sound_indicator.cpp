// Sound on the SenseCAP Indicator: the buzzer is on the RP2040 co-processor, which runs SkyDisplay's own small
// firmware (firmware/IndicatorRP2040). This side times the patterns and sends one tone at a time over UART;
// a tone always ends by itself, so a stalled ESP32 can't leave the buzzer on.
#include "board.h"
#if defined(BOARD_INDICATOR)
#include "sound.h"
#include "settings.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"

static HardwareSerial rp(2);
static const int PIN_TX = 19, PIN_RX = 20;
enum : uint8_t { PKT_BEEP_OFF = 0xA2, PKT_TONE = 0xA6, PKT_PING = 0xA7, PKT_HELLO = 0xC7 };

static bool ready = false, otherFirmware = false;  // other packets but no HELLO: Seeed's factory firmware
static uint8_t fwVersion = 0;
static uint32_t pingAt = 0; static int pings = 0;
static uint32_t bytesIn = 0;  // anything at all from the RP2040 (diagnostics)

// Alarm: two beeps every 3 s; stops after 5 minutes. Chime: two rising notes.
static const uint16_t ALARM_HZ = 2700, BEEP_MS = 180, BEEP_GAP_MS = 320;
static const uint32_t ALARM_PERIOD_MS = 3000, ALARM_MAX_MS = 5UL * 60000;
static bool alarmOn = false;
static uint32_t alarmStartMs = 0, nextStepMs = 0;
static int alarmStep = 0;
static int chimeStep = -1; static uint32_t chimeAtMs = 0;

static size_t cobsEncode(const uint8_t* in, size_t len, uint8_t* out) {
  size_t o = 1, code = 0; uint8_t n = 1;
  for (size_t i = 0; i < len; i++) {
    if (in[i]) { out[o++] = in[i]; n++; }
    if (!in[i] || n == 0xFF) { out[code] = n; code = o++; n = 1; }
  }
  out[code] = n;
  return o;
}
static size_t cobsDecode(const uint8_t* in, size_t len, uint8_t* out) {
  size_t o = 0;
  for (size_t i = 0; i < len; ) {
    uint8_t n = in[i++];
    if (!n) return 0;
    for (uint8_t k = 1; k < n; k++) { if (i >= len) return 0; out[o++] = in[i++]; }
    if (n != 0xFF && i < len) out[o++] = 0;
  }
  return o;
}
static void sendPacket(const uint8_t* d, size_t len) {
  uint8_t buf[48];
  size_t n = cobsEncode(d, len, buf);
  buf[n++] = 0;
  rp.write(buf, n);
}
// Volume setting (1 low, 2 medium, 3 high) to the RP2040's drive level (100 = loudest; a piezo's loudness falls
// off quickly as the drive drops, so the steps are uneven).
static uint8_t driveLevel() { static const uint8_t L[4] = {100, 12, 35, 100}; return L[constrain(cfg.alarmVolume, 1, 3)]; }
static void playTone(uint16_t hz, uint16_t ms) {
  uint8_t p[6] = {PKT_TONE, (uint8_t)hz, (uint8_t)(hz >> 8), (uint8_t)ms, (uint8_t)(ms >> 8), driveLevel()};
  sendPacket(p, sizeof p);  // firmware v1 ignores the volume byte
}

void soundBegin() {
  // GPIO19/20 are also the ESP32-S3's own USB pins. This board's USB-C goes through the CH340 instead, so detach the
  // built-in USB PHY from them; otherwise it keeps its pull-ups and pad drivers on the UART lines.
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  rp.begin(115200, SERIAL_8N1, PIN_RX, PIN_TX);
  pingAt = millis() + 500;
}

void soundLoop() {
  // Replies from the RP2040.
  static uint8_t raw[64], pkt[64];
  static size_t n = 0;
  while (rp.available()) {
    uint8_t c = rp.read();
    bytesIn++;
    if (c != 0) { if (n < sizeof raw) raw[n++] = c; else n = 0; continue; }
    size_t len = cobsDecode(raw, n, pkt); n = 0;
    if (!len) continue;
    if (pkt[0] == PKT_HELLO && len >= 2) {
      if (!ready) Serial.printf("[sound] RP2040 buzzer firmware v%u\n", pkt[1]);
      ready = true; fwVersion = pkt[1];
    } else otherFirmware = true;  // e.g. Seeed's sensor readings
  }
  // Ask until it answers: every 5 s for the first minute, then every minute (it may be flashed later).
  if (!ready && (int32_t)(millis() - pingAt) >= 0) {
    uint8_t p = PKT_PING; sendPacket(&p, 1);
    pings++; pingAt = millis() + (pings < 12 ? 5000 : 60000);
  }
  if (alarmOn) {
    if (millis() - alarmStartMs >= ALARM_MAX_MS) { soundStop(); Serial.println("[sound] alarm stopped after 5 min"); }
    else if ((int32_t)(millis() - nextStepMs) >= 0) {
      playTone(ALARM_HZ, BEEP_MS);
      nextStepMs += alarmStep == 0 ? BEEP_GAP_MS : ALARM_PERIOD_MS - BEEP_GAP_MS;
      alarmStep ^= 1;
    }
  }
  if (chimeStep >= 0 && (int32_t)(millis() - chimeAtMs) >= 0) {
    if (chimeStep == 0) { playTone(1760, 140); chimeAtMs = millis() + 190; chimeStep = 1; }
    else { playTone(2350, 260); chimeStep = -1; }
  }
}

bool soundReady() { return ready; }
String soundFirmware() { return "RP2040 firmware v" + String(fwVersion); }
String soundStatus() {
  if (ready) return "Buzzer ready (RP2040 v" + String(fwVersion) + ")";
  if (otherFirmware) return "RP2040 has Seeed's firmware";
  return bytesIn ? "RP2040 answer not understood (" + String(bytesIn) + " bytes)" : String("No answer from the RP2040");
}

void soundAlarm() {
  if (alarmOn) return;
  alarmOn = true; alarmStartMs = nextStepMs = millis(); alarmStep = 0; chimeStep = -1;
}
void soundChime() { if (!alarmOn) { chimeStep = 0; chimeAtMs = millis(); } }
void soundStop() {
  alarmOn = false; chimeStep = -1;
  uint8_t p = PKT_BEEP_OFF; sendPacket(&p, 1);
}
bool soundAlarmOn() { return alarmOn; }
bool soundHasVolume() { return ready && fwVersion >= 2; }

#endif
