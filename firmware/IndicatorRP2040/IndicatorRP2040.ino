// IndicatorRP2040: buzzer firmware for the SenseCAP Indicator's RP2040 co-processor, driven by SkyDisplay on the
// ESP32-S3. Replaces Seeed's factory RP2040 firmware (which only beeps at power-on); SkyDisplay doesn't use the
// RP2040's sensors, so this firmware leaves them off.
//
// Link: Serial1 (TX GPIO16, RX GPIO17) at 115200 to the ESP32-S3, COBS-framed packets ending in 0x00, the same
// framing as Seeed's firmware. Each packet is one type byte and a little-endian payload.
//   0xA1 BEEP_ON  u32 ms              one beep at the default pitch (compatible with Seeed's packet)
//   0xA2 BEEP_OFF                     stop any sound
//   0xA6 TONE     u16 Hz, u16 ms      one tone (the ESP32 times the patterns; a tone always ends by itself)
//   0xA7 PING                         answered with HELLO
//   0xC7 HELLO    u8 version          sent every 2 s until the first PING, then in answer to PING
//
// Build with the Raspberry Pi Pico Arduino core (earlephilhower), board "Seeed INDICATOR RP2040"
// (rp2040:rp2040:seeed_indicator_rp2040).
// Flashing: see README.md in this folder.

#define FW_VERSION 1
static const int BUZZER = 19;              // passive piezo: needs a square wave
static const uint16_t DEFAULT_HZ = 2700;   // near the piezo's loudest pitch

enum : uint8_t { PKT_BEEP_ON = 0xA1, PKT_BEEP_OFF = 0xA2, PKT_TONE = 0xA6, PKT_PING = 0xA7, PKT_HELLO = 0xC7 };

// ---------- COBS ----------
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

static void sendPacket(const uint8_t* data, size_t len) {
  uint8_t buf[48];
  size_t n = cobsEncode(data, len, buf);
  buf[n++] = 0;
  Serial1.write(buf, n);
}
static bool pinged = false;  // the ESP32 has asked; until then HELLO repeats (it may start later than us)
static void sendHello() { uint8_t p[2] = {PKT_HELLO, FW_VERSION}; sendPacket(p, 2); }

static uint16_t u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void handle(const uint8_t* p, size_t len) {
  if (!len) return;
  switch (p[0]) {
    case PKT_BEEP_ON:
      if (len >= 5) tone(BUZZER, DEFAULT_HZ, min(u32(p + 1), (uint32_t)10000));
      break;
    case PKT_BEEP_OFF: noTone(BUZZER); digitalWrite(BUZZER, LOW); break;
    case PKT_TONE:
      if (len >= 5) {
        uint16_t hz = constrain(u16(p + 1), 100, 8000), ms = min(u16(p + 3), (uint16_t)10000);
        if (ms) tone(BUZZER, hz, ms);
      }
      break;
    case PKT_PING: pinged = true; sendHello(); break;
    default: break;  // e.g. Seeed's collect-interval and power packets: nothing to do
  }
}

void setup() {
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);  // silent start (no power-on beep: the display may be next to someone asleep)
  Serial1.setRX(17);
  Serial1.setTX(16);
  Serial1.begin(115200);
  sendHello();
}

void loop() {
  static uint8_t raw[64], pkt[64];
  static size_t n = 0;
  static uint32_t helloAt = 0;
  if (!pinged && millis() - helloAt >= 2000) { helloAt = millis(); sendHello(); }
  while (Serial1.available()) {
    uint8_t c = Serial1.read();
    if (c == 0) { size_t len = cobsDecode(raw, n, pkt); handle(pkt, len); n = 0; }
    else if (n < sizeof raw) raw[n++] = c;
    else n = 0;  // too long: drop it
  }
}
