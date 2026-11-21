#pragma once
#include <Arduino.h>

// Small HTTPS server (port 443) for the one page that needs a secure context: getting the phone's
// GPS location. Browsers only allow geolocation on HTTPS pages; the settings page itself stays on HTTP.
//
// The certificate is self-signed and generated on the device at first boot (ECDSA P-256), then stored
// in NVS. No key ever lives in the source code. Phones show a "not private" warning the first time.
bool httpsBegin(const String& hostname);
