#include "net.h"
#include "config.h"
#include "settings.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

static const char* UA = "SkyDisplay/" FIRMWARE_VERSION " (ESP32-S3 bedside display)";
static String etag;
static const size_t GIF_MAX = 256 * 1024;
static uint8_t* gifBuf = nullptr;

// Prototype: TLS without certificate checking. TODO: pin Amazon's root CA (CloudFront).
static void prepClient(WiFiClientSecure& c) { c.setInsecure(); c.setTimeout(15000); }  // ms (Stream::setTimeout)

// Read the whole body into gifBuf. Returns byte count or -1.
static int readBody(HTTPClient& http) {
  if (!gifBuf) gifBuf = (uint8_t*)ps_malloc(GIF_MAX);
  if (!gifBuf) return -1;
  int expected = http.getSize();  // -1 when chunked
  WiFiClient* s = http.getStreamPtr();
  size_t got = 0; uint32_t last = millis();
  while (http.connected() && (expected < 0 || (int)got < expected) && got < GIF_MAX) {
    size_t avail = s->available();
    if (avail) {
      int r = s->readBytes(gifBuf + got, min(avail, GIF_MAX - got));
      if (r > 0) { got += r; last = millis(); }
    } else {
      if (millis() - last > 10000) break;
      delay(2);
    }
  }
  if (expected >= 0 && (int)got != expected) return -1;
  return (int)got;
}

// Matches "<prefix><up to 24 chars>" in a byte stream and captures the chars up to a stop
// character. Used for "map/cloud/<n>.html?valid=<time>" and "maps?q=<lat>,<lon>".
struct Grab {
  const char* key; char val[32]; int len = -1; bool done = false;
  void feed(const char* win, size_t wl, char c) {
    if (done) return;
    if (len >= 0) {
      if (c == '"' || c == '&' || c == '\'' || c == '<' || len >= 31) { done = true; return; }
      val[len++] = c; val[len] = 0; return;
    }
    size_t kl = strlen(key);
    if (wl >= kl && memcmp(win + wl - kl, key, kl) == 0) { len = 0; val[0] = 0; }
  }
};

// Stream the chart page until the first "map/cloud/<n>.html?valid=..." link, which gives
// the UTC time of chart column 0. <n> is not always 1: hours that have passed drop off the
// front, so the first link can be e.g. map/cloud/7. The site's coordinates appear earlier
// in the page in a Google Maps link ("maps?q=lat,lon"). Stops reading once the time is found.
static time_t fetchPageInfo(const ChartTarget& ct, double& lat, double& lon, String& err) {
  WiFiClientSecure client; prepClient(client);
  HTTPClient http;
  http.setUserAgent(UA);
  if (!http.begin(client, "https://" + ct.host + ct.pagePath)) { err = "page: begin failed"; return 0; }
  int code = http.GET();
  if (code != 200) { err = "page: HTTP " + String(code); http.end(); return 0; }
  WiFiClient* s = http.getStreamPtr();
  char win[48] = {0}; size_t wl = 0;
  size_t total = 0; uint32_t last = millis();
  Grab coords; coords.key = "maps?q=";
  Grab stamp;  stamp.key = ".html?valid=";
  int sinceMap = -1;  // chars since "map/cloud/"
  while (http.connected() && total < 300000 && !stamp.done) {
    int c = s->read();
    if (c < 0) { if (millis() - last > 10000) break; delay(2); continue; }
    last = millis(); total++;
    if (wl == sizeof(win) - 1) { memmove(win, win + 1, wl - 1); wl--; }
    win[wl++] = (char)c; win[wl] = 0;
    coords.feed(win, wl, (char)c);
    if (wl >= 10 && memcmp(win + wl - 10, "map/cloud/", 10) == 0) { sinceMap = 0; continue; }
    if (stamp.len >= 0) { stamp.feed(win, wl, (char)c); continue; }
    if (sinceMap >= 0) {
      sinceMap++;
      if (sinceMap > 20) sinceMap = -1;
      else stamp.feed(win, wl, (char)c);
    }
  }
  http.end();
  if (coords.len > 0) {
    char* comma = strchr(coords.val, ',');
    if (comma) { lat = atof(coords.val); lon = atof(comma + 1); }
  }
  time_t t = stamp.len > 0 ? parseIsoUtc(stamp.val) : 0;
  if (!t) err = "page: start time not found";
  else logf("[net] column 0 = %s, site %.5f,%.5f (read %u bytes of page)\n", stamp.val, lat, lon, (unsigned)total);
  return t;
}

static volatile bool forgetEtag = false;  // set from the main loop, acted on in the network task
void netForgetEtag() { forgetEtag = true; }
String netEtag() { return etag; }
void netSetEtag(const String& e) { etag = e; }

FetchResult fetchChart(Forecast& f, const ChartTarget& ct, String& err) {
  HttpsGuard tls;  // the image and the page, one connection at a time with the other tasks
  if (forgetEtag) { etag = ""; forgetEtag = false; }
  if (WiFi.status() != WL_CONNECTED) { err = "no Wi-Fi"; return FETCH_FAILED; }
  if (!ct.ok) { err = "chart URL not understood"; return FETCH_FAILED; }
  WiFiClientSecure client; prepClient(client);
  HTTPClient http;
  http.setUserAgent(UA);
  if (!http.begin(client, "https://" + ct.host + ct.gifPath)) { err = "gif: begin failed"; return FETCH_FAILED; }
  const char* keys[] = {"ETag"};
  http.collectHeaders(keys, 1);
  if (etag.length() && f.valid) http.addHeader("If-None-Match", etag);
  int code = http.GET();
  if (code == 304) { http.end(); logf("[net] chart unchanged (304)\n"); return FETCH_UNCHANGED; }
  if (code != 200) { err = "gif: HTTP " + String(code); http.end(); return FETCH_FAILED; }
  String newTag = http.header("ETag");
  int len = readBody(http);
  http.end();
  if (len <= 0) { err = "gif: download incomplete"; return FETCH_FAILED; }
  logf("[net] chart.gif %d bytes, ETag '%s'\n", len, newTag.c_str());

  Forecast nf;
  String e = decodeChartGif(gifBuf, len, nf);
  if (e.length()) { err = "decode: " + e; return FETCH_FAILED; }
  nf.startUtc = fetchPageInfo(ct, nf.siteLat, nf.siteLon, err);
  if (!nf.startUtc && err.startsWith("page: HTTP -")) {  // couldn't connect (often memory for TLS): one more try
    logf("[net] %s, retrying in 3 s\n", err.c_str());
    delay(3000); err = "";
    nf.startUtc = fetchPageInfo(ct, nf.siteLat, nf.siteLon, err);
  }
  if (!nf.startUtc) return FETCH_FAILED;
  f = nf;
  etag = newTag;
  return FETCH_UPDATED;
}
