#include "ui.h"
#include "config.h"
#include "settings.h"
#include "lgfx_wt32sc01plus.h"
#include "fonts.h"

static LGFX_WT32SC01Plus lcd;
static LGFX_Sprite spr(&lcd);
static const int W = 480, H = 320;

// ---------- smooth (anti-aliased) fonts, loaded once ----------
struct SmoothFont {
  lgfx::PointerWrapper data;
  lgfx::VLWfont font;
  bool load(const uint8_t* a, size_t len) { data.set(a, len); return font.loadFont(&data); }
};
static SmoothFont fSmall, fBody, fBodyB, fTitle, fBig, fHuge;

// ---------- palette ----------
static const uint32_t C_BG = 0x04060B, C_TILE = 0x0C111C, C_LINE = 0x1B2337, C_FG = 0xE6ECF7, C_MUTED = 0x9AA6BD,
                      C_GO = 0x3DDC84, C_MAYBE = 0xFFB43D, C_NO = 0xFF5A5A, C_IDLE = 0x5B6577,
                      C_MID = 0xD9333F, C_EMPTY = 0x141A26;
static const uint32_t CLOUD_C[11] = {0xFAFAFA,0xE9E9E9,0xC1C1C1,0xADEDED,0x99D9D9,0x76B6F6,0x62A2E2,0x4E8ECE,0x2666A6,0x135393,0x003E7E};
static const uint32_t FIVE_C[6]   = {0xF9F9F9,0xC7C7C7,0x95D5D5,0x63A3E3,0x2C6CAC,0x003F7F};
static const char* DOW[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
static const char* MON[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
static const int HUM_LO[16] = {0,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95};
static const char* WIND_TXT[6] = {"over 72 km/h", "46\xE2\x80\x93" "72 km/h", "28\xE2\x80\x93" "45 km/h",
                                  "19\xE2\x80\x93" "27 km/h", "9\xE2\x80\x93" "18 km/h", "0\xE2\x80\x93" "8 km/h"};

static inline uint16_t c565(uint32_t c) { return lgfx::color565((c >> 16) & 255, (c >> 8) & 255, c & 255); }
static uint32_t blend(uint32_t a, uint32_t b, float t) {  // t=0 -> a, t=1 -> b
  int r = ((a >> 16) & 255) + (int)((((b >> 16) & 255) - ((a >> 16) & 255)) * t);
  int g = ((a >> 8) & 255) + (int)((((b >> 8) & 255) - ((a >> 8) & 255)) * t);
  int bl = (a & 255) + (int)(((b & 255) - (a & 255)) * t);
  return (uint32_t)(r << 16 | g << 8 | bl);
}

static uint32_t wordColor(Word w) {
  switch (w) { case W_GO: case W_CLEAR: return C_GO; case W_MAYBE: case W_FAIR: return C_MAYBE; case W_NO: return C_NO; default: return C_IDLE; }
}
static const char* wordText(Word w) {
  switch (w) { case W_GO: return "GO"; case W_MAYBE: return "MAYBE"; case W_NO: return "NO"; case W_CLEAR: return "CLEAR";
               case W_FAIR: return "FAIR"; case W_DAY: return "DAY"; default: return "\xE2\x80\x93"; }
}

// Draw text at native size (no scaling). If maxW is set and the text is too wide,
// it is shortened with "..".
static void text(const char* s, int x, int y, SmoothFont& f, uint32_t col, textdatum_t datum = top_left, int maxW = 0) {
  spr.setFont(&f.font);
  spr.setTextSize(1);
  spr.setTextDatum(datum);
  spr.setTextColor(c565(col));
  if (maxW > 0 && spr.textWidth(s) > maxW) {
    char buf[64]; strncpy(buf, s, sizeof buf - 3); buf[sizeof buf - 3] = 0;
    size_t n = strlen(buf);
    while (n > 1) { buf[n - 1] = 0; n--; char t[66]; snprintf(t, sizeof t, "%s..", buf); if (spr.textWidth(t) <= maxW) { spr.drawString(t, x, y); return; } }
  }
  spr.drawString(s, x, y);
}

// Like text(), but if it doesn't fit maxW in font f, tries the smaller font first, then shortens.
static int textFit(const char* s, int x, int y, SmoothFont& f, SmoothFont& smaller, uint32_t col, textdatum_t datum, int maxW) {
  spr.setFont(&f.font);
  SmoothFont& use = spr.textWidth(s) <= maxW ? f : smaller;
  text(s, x, y, use, col, datum, maxW);
  spr.setFont(&use.font);
  return min((int)spr.textWidth(s), maxW);
}

static void led(int x, int y, uint32_t col, bool on) {
  if (on) for (int r = 24; r > 11; r--) { float t = 1.0f - (r - 11) / 13.0f; spr.fillCircle(x, y, r, c565(blend(C_TILE, col, 0.55f * t * t))); }
  spr.fillSmoothCircle(x, y, 10, c565(col));
  spr.fillSmoothCircle(x - 3, y - 3, 3, c565(blend(col, 0xFFFFFF, 0.35f)));
}

static void tile(int x, const char* label, const Verdict& v) {
  const int y = 30, w = 148, h = 112;
  spr.fillSmoothRoundRect(x, y, w, h, 10, c565(C_TILE));
  spr.drawRoundRect(x, y, w, h, 10, c565(C_LINE));
  bool lit = v.word != W_DAY && v.word != W_NONE;
  uint32_t col = wordColor(v.word);
  led(x + 24, y + 28, col, lit);
  text(label, x + 44, y + 9, fSmall, C_MUTED, top_left, w - 50);
  text(wordText(v.word), x + 43, y + 20, fBig, lit ? col : C_MUTED, top_left, w - 48);
  text(v.l1, x + 12, y + 62, fBodyB, C_FG, top_left, w - 20);
  text(v.l2, x + 12, y + 87, fBody, C_MUTED, top_left, w - 20);
}

static void localTm(time_t t, struct tm& out) { localtime_r(&t, &out); }
static void startTouchTask();

void uiBegin(uint8_t brightness) {
  lcd.init();
  lcd.setRotation(1);  // landscape 480x320
  lcd.setBrightness(brightness);
  lcd.fillScreen(TFT_BLACK);
  spr.setPsram(true);
  spr.setColorDepth(16);
  if (!spr.createSprite(W, H)) Serial.println("[ui] sprite allocation FAILED");
  bool ok = fSmall.load(F_SMALL, sizeof F_SMALL) & fBody.load(F_BODY, sizeof F_BODY) & fBodyB.load(F_BODYB, sizeof F_BODYB)
          & fTitle.load(F_TITLE, sizeof F_TITLE) & fBig.load(F_BIG, sizeof F_BIG) & fHuge.load(F_HUGE, sizeof F_HUGE);
  if (!ok) Serial.println("[ui] font load FAILED");
  startTouchTask();
}

void uiSetBacklight(uint8_t level) { lcd.setBrightness(level); }

static void header(const UiState& s, const char* rightText = nullptr, bool rightWarn = false);
static void header(const UiState& s, const char* rightText, bool rightWarn) {
  char buf[48];
  if (s.nowUtc) {
    struct tm tmv; localTm(s.nowUtc, tmv);
    snprintf(buf, sizeof buf, "%s %d %s   %02d:%02d", DOW[tmv.tm_wday], tmv.tm_mday, MON[tmv.tm_mon], tmv.tm_hour, tmv.tm_min);
  } else strcpy(buf, "Setting clock...");
  text(buf, 12, 4, fBodyB, C_FG);

  uint32_t col = C_MUTED;
  if (rightText) { snprintf(buf, sizeof buf, "%s", rightText); if (rightWarn) col = C_MAYBE; }
  else if (s.problem.length()) { snprintf(buf, sizeof buf, "%s", s.problem.c_str()); col = C_MAYBE; }
  else if (s.lastCheckOk && s.nowUtc) {
    long mins = (long)(s.nowUtc - s.lastCheckOk) / 60;
    if (mins >= STALE_MINUTES) { snprintf(buf, sizeof buf, "Stale: last update %ldh ago", mins / 60); col = C_MAYBE; }
    else { struct tm tmv; localTm(s.lastCheckOk, tmv);
           snprintf(buf, sizeof buf, "Chart checked %02d:%02d", tmv.tm_hour, tmv.tm_min); }
  } else snprintf(buf, sizeof buf, "Clear Sky Chart %s", chartTarget().key.c_str());
  text(buf, W - 12, 7, fBody, col, top_right, 200);

  // Screen dots, between the date and the status text.
  for (int k = 0; k < SCREEN_COUNT; k++) {
    int x = 236 + (k - (SCREEN_COUNT - 1) / 2.0f) * 14, y = 15;
    if (k == s.screen) spr.fillSmoothCircle(x, y, 4, c565(C_FG));
    else spr.fillSmoothCircle(x, y, 3, c565(C_IDLE));
  }
}

// One footer column: small grey label over a white value, aligned with a tile.
// indent leaves room for an icon drawn before the value.
static void footItem(int x, const char* label, const char* value, uint32_t valueCol, int indent = 0) {
  text(label, x + 2, 276, fSmall, C_MUTED, top_left, 146);
  text(value, x + 2 + indent, 294, fBody, valueCol, top_left, 146 - indent);
}

// Moon icon: dark disc with the lit part drawn row by row (lit on the right when waxing, as seen
// from the northern hemisphere).
static void moonIcon(int cx, int cy, int r, float illum, bool waxing) {
  spr.fillSmoothCircle(cx, cy, r, c565(0x2A3245));
  uint16_t lit = c565(0xE8E4D0);
  for (int y = -r; y <= r; y++) {
    float w = sqrtf((float)(r * r - y * y));
    float t = w * (1 - 2 * illum);  // terminator x
    int x1 = (int)lroundf(waxing ? t : -w), x2 = (int)lroundf(waxing ? w : -t);
    if (x2 >= x1) spr.drawFastHLine(cx + x1, cy + y, x2 - x1 + 1, lit);
  }
}

static void fmtTime(char* buf, size_t sz, time_t t) {
  if (!t) { snprintf(buf, sz, "\xE2\x80\x93"); return; }
  struct tm tmv; localTm(t, tmv);
  snprintf(buf, sz, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

static void footerDots(int page) {
  for (int k = 0; k < FOOTER_PAGES; k++) {
    int x = 30 + k * 12, y = 262;
    if (k == page) spr.fillSmoothCircle(x, y, 3, c565(C_FG));
    else spr.fillSmoothCircle(x, y, 2, c565(C_IDLE));
  }
}

static void drawFooter(const UiState& s, const Forecast& f, const Night* tn, const Verdict& vTn) {
  footerDots(s.footerPage);
  char a[40], b[40], c[40];
  const SkyEvents& e = s.sky;
  switch (s.footerPage) {
    case 0: {  // moon
      if (!s.skyValid) { footItem(12, "MOON", "Needs location", C_MUTED); break; }
      snprintf(a, sizeof a, "MOON  %d%%", (int)lroundf(e.moonIllum * 100));
      moonIcon(12 + 11, 294 + 11, 9, e.moonIllum, e.moonWaxing);
      footItem(12, a, moonPhaseName(e.moonIllum, e.moonWaxing), C_FG, 26);
      fmtTime(b, sizeof b, e.moonrise); footItem(166, "MOONRISE", b, C_FG);
      fmtTime(c, sizeof c, e.moonset);  footItem(320, "MOONSET", c, C_FG);
      break;
    }
    case 1: {  // sun
      if (!s.skyValid) { footItem(12, "SUN", "Needs location", C_MUTED); break; }
      char d1[8], d2[8];
      fmtTime(a, sizeof a, e.sunset); footItem(12, "SUNSET", a, C_FG);
      fmtTime(d1, sizeof d1, e.duskAstro); fmtTime(d2, sizeof d2, e.dawnAstro);
      snprintf(b, sizeof b, "%s\xE2\x80\x93%s", d1, d2); footItem(166, "ASTRONOMICAL DARK", b, C_FG);
      fmtTime(c, sizeof c, e.sunrise); footItem(320, "SUNRISE", c, C_FG);
      break;
    }
    default: {  // ground conditions and seeing for tonight, from the chart
      if (!vTn.hasNight) { footItem(12, "TONIGHT", "No night in chart", C_MUTED); break; }
      int humPct = HUM_LO[vTn.hum] + 5;
      bool dew = humPct >= 85;
      snprintf(a, sizeof a, dew ? "%d%%  dew risk" : "%d%%", humPct);
      footItem(12, "PEAK HUMIDITY", a, dew ? C_MAYBE : C_FG);
      footItem(166, "CALMEST WIND", WIND_TXT[vTn.wind], C_FG);
      int sum = 0, n = 0;
      for (int i = tn->s; i <= tn->e; i++) if (f.seeing[i] > 0) { sum += f.seeing[i]; n++; }
      static const char* SEE[] = {"", "Bad", "Poor", "Average", "Good", "Excellent"};
      if (n) { int avg = (int)lroundf((float)sum / n); snprintf(c, sizeof c, "%s  %d/5", SEE[avg], avg); }
      else strcpy(c, "Too cloudy");
      footItem(320, "SEEING TONIGHT", c, C_FG);
      break;
    }
  }
}

void uiDraw(const UiState& s) {
  spr.fillScreen(c565(C_BG));
  header(s);
  const Forecast* f = s.f;
  if (!f || !f->valid) {
    text("Waiting for the first chart...", W / 2, 150, fTitle, C_MUTED, middle_center);
    spr.pushSprite(0, 0);
    return;
  }

  int now = s.nowUtc ? (int)floor((double)(s.nowUtc - f->startUtc) / 3600.0) : 0;
  Night nights[8]; int nn = findNights(*f, nights, 8);
  const Night* tn = nullptr; const Night* tm = nullptr;
  for (int k = 0; k < nn; k++) if (nights[k].e >= now) { tn = &nights[k]; tm = (k + 1 < nn) ? &nights[k + 1] : nullptr; break; }

  Verdict vNow = rateNow(*f, now, nights, nn), vTn = rateNight(*f, tn), vTm = rateNight(*f, tm);
  if (vNow.word == W_DAY && s.skyValid && s.sky.duskAstro) {  // exact time instead of the chart's hour cell
    struct tm d; localTm(s.sky.duskAstro, d);
    snprintf(vNow.l1, sizeof vNow.l1, "Dark at %02d:%02d", d.tm_hour, d.tm_min);
  }
  tile(12, "NOW", vNow);
  tile(166, (tn && tn->s <= now) ? "TONIGHT (NOW)" : "TONIGHT", vTn);
  tile(320, "TOMORROW", vTm);

  // ---- 52-hour strip ----
  const int COLS = 52;
  int a = max(0, min(now - 3, f->n - COLS));
  const float x0 = 74, pitch = (468.0f - x0) / COLS;
  const int cw = max(1, (int)(pitch - 1.2f));
  const int y0 = 154, rh = 11, rp = 14;
  const char* labels[] = {"Score", "RDPS", "ECMWF", "HRRR", "Transp", "Seeing", "Dark"};
  for (int r = 0; r < 7; r++) {
    int y = y0 + r * rp;
    text(labels[r], (int)x0 - 7, y + rh / 2, fSmall, r == 0 ? C_FG : C_MUTED, middle_right);
    for (int c = 0; c < COLS; c++) {
      int i = a + c; if (i >= f->n) break;
      int32_t col = -1;
      switch (r) {
        case 0: { HourEval h = hourEval(*f, i); col = (isDark(*f, i) && h.ok) ? (h.q == 2 ? C_GO : h.q == 1 ? C_MAYBE : C_NO) : C_EMPTY; break; }
        case 1: if (f->rdps[i] >= 0) col = CLOUD_C[f->rdps[i]]; break;
        case 2: if (f->ecmwf[i] >= 0) col = CLOUD_C[f->ecmwf[i]]; break;
        case 3: if (f->hrrr[i] >= 0) col = CLOUD_C[f->hrrr[i]]; break;
        case 4: if (f->trans[i] >= 0) col = FIVE_C[f->trans[i]]; break;
        case 5: if (f->seeing[i] >= 0) col = FIVE_C[f->seeing[i]]; break;
        case 6: col = f->dark[i]; break;
      }
      if (col >= 0) spr.fillRect((int)(x0 + c * pitch), y, cw, rh, c565((uint32_t)col));
    }
  }
  const int yEnd = y0 + 7 * rp;
  for (int c = 0; c < COLS; c++) {
    int i = a + c; if (i >= f->n) break;
    int h = localHour(*f, i), x = (int)(x0 + c * pitch);
    if (h == 0) {
      spr.fillRect(x - 1, y0 - 4, 2, yEnd - y0 + 4, c565(C_MID));
      struct tm tmv; localTm(f->startUtc + (time_t)i * 3600, tmv);
      text(DOW[tmv.tm_wday], x, yEnd + 2, fSmall, C_FG, top_center);
    } else if (h % 6 == 0) {
      char hb[4]; snprintf(hb, sizeof hb, "%02d", h);
      text(hb, x + cw / 2, yEnd + 2, fSmall, C_MUTED, top_center);
    }
  }
  if (now >= a && now < a + COLS) {
    int xn = (int)(x0 + (now - a) * pitch) + cw / 2;
    spr.fillTriangle(xn - 4, y0 - 10, xn + 4, y0 - 10, xn, y0 - 4, TFT_WHITE);
    spr.drawFastVLine(xn, y0 - 4, yEnd - y0 + 1, c565(0x808080));
  }

  drawFooter(s, *f, tn, vTn);
  spr.pushSprite(0, 0);
}

// Status screen: two columns of label/value rows, plus a QR code for the settings page.
static void statusColumn(int x, int w, const StatusItem* items, int n) {
  for (int k = 0; k < n; k++) {
    int y = 44 + k * 34;
    text(items[k].label.c_str(), x, y, fSmall, C_MUTED, top_left, w);
    text(items[k].value.c_str(), x, y + 15, fBody, items[k].warn ? C_MAYBE : C_FG, top_left, w);
  }
}

void uiDrawStatus(const UiState& s, const StatusInfo& st) {
  spr.fillScreen(c565(C_BG));
  header(s);
  statusColumn(16, 200, st.left, st.nLeft);
  statusColumn(232, 130, st.right, st.nRight);
  if (st.qrUrl.length()) {
    const int qx = 372, qy = 50, qw = 96;
    spr.fillRoundRect(qx - 6, qy - 6, qw + 12, qw + 12, 8, TFT_WHITE);
    spr.qrcode(st.qrUrl.c_str(), qx, qy, qw, 3);
    text("Settings", qx + qw / 2, qy + qw + 12, fSmall, C_MUTED, top_center);
  }
  text("Swipe for the other screens", W / 2, H - 22, fSmall, C_IDLE, top_center);
  spr.pushSprite(0, 0);
}

// ---------- weather screen (layout follows mockup/weather-screen.html) ----------
static const uint32_t C_TEMP = 0xFFD27A, C_DEW = 0x6FB6FF, C_CLOUDICON = 0xC9D2E3, C_MOONICON = 0xE8E4D0, C_SUNICON = 0xFFD166;
static const char* MINUS = "\xE2\x80\x93";  // en dash as the minus sign (the fonts have no U+2212)

static void fmtDeg(char* b, size_t n, float v, int dec) {
  if (isnan(v)) { snprintf(b, n, "%s", MINUS); return; }
  char num[16]; snprintf(num, sizeof num, "%.*f", dec, fabsf(v));
  if (dec == 0 && strcmp(num, "0") == 0) { snprintf(b, n, "0\xC2\xB0"); return; }
  snprintf(b, n, "%s%s\xC2\xB0", v < 0 ? MINUS : "", num);
}

static void wxReading(int x, int y, const char* label, const char* value, const char* sub, bool warn, bool subWarn, int* valueW = nullptr) {
  const int CELL = 104;
  text(label, x, y + 12, fSmall, C_MUTED, baseline_left, CELL);
  int w = textFit(value, x, y + 33, fBodyB, fBody, warn ? C_MAYBE : C_FG, baseline_left, CELL);
  if (valueW) *valueW = w;
  if (sub && *sub) text(sub, x, y + 49, fSmall, subWarn ? C_MAYBE : C_MUTED, baseline_left, CELL);
}

static void windArrow(int cx, int cy, int dirDeg) {  // points where the wind blows toward
  float a = (dirDeg + 180) * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a);
  auto P = [&](float x, float y, int& ox, int& oy) { ox = cx + (int)lroundf(x * c - y * s); oy = cy + (int)lroundf(x * s + y * c); };
  int x0, y0, x1, y1, x2, y2, x3, y3;
  P(0, -8, x0, y0); P(5, 5, x1, y1); P(0, 2, x2, y2); P(-5, 5, x3, y3);
  spr.fillTriangle(x0, y0, x1, y1, x2, y2, c565(C_FG));
  spr.fillTriangle(x0, y0, x2, y2, x3, y3, c565(C_FG));
}

static void wxCloud(int x, int y, float s, uint32_t col) {
  uint16_t c = c565(col);
  spr.fillSmoothCircle(x, y, (int)(7 * s), c); spr.fillSmoothCircle(x - (int)(7 * s), y + (int)(3 * s), (int)(5 * s), c);
  spr.fillSmoothCircle(x + (int)(7 * s), y + (int)(3 * s), (int)(5 * s), c); spr.fillSmoothCircle(x + (int)(3 * s), y - (int)(4 * s), (int)(5 * s), c);
  spr.fillRect(x - (int)(7 * s), y + (int)(2 * s), (int)(14 * s), (int)(6 * s), c);
}
static void wxMoon(int x, int y, int r, uint32_t bg) {
  spr.fillSmoothCircle(x, y, r, c565(C_MOONICON));
  spr.fillSmoothCircle(x + r / 2, y - (int)(r * 0.35f), (int)(r * 0.85f), c565(bg));
}
// WU icon codes, simplified to what matters at night.
static void wxIcon(int code, int cx, int cy, uint32_t bg) {
  bool snow = (code >= 5 && code <= 8) || (code >= 13 && code <= 16) || (code >= 41 && code <= 43) || code == 46 || code == 25;
  bool rain = (code >= 9 && code <= 12) || code == 39 || code == 40 || code == 45 || (code >= 3 && code <= 4) || (code >= 37 && code <= 38) || code == 47;
  if (code == 31 || code == 33) wxMoon(cx, cy, 9, bg);                               // clear / fair night
  else if (code == 32 || code == 34 || code == 36) spr.fillSmoothCircle(cx, cy, 8, c565(C_SUNICON));
  else if (code == 29 || code == 27) { wxMoon(cx - 4, cy - 4, 8, bg); wxCloud(cx + 3, cy + 3, 0.85f, C_CLOUDICON); }  // partly / mostly cloudy night
  else if (code == 30 || code == 28) { spr.fillSmoothCircle(cx - 4, cy - 4, 7, c565(C_SUNICON)); wxCloud(cx + 3, cy + 3, 0.85f, C_CLOUDICON); }
  else if (snow || rain) {
    wxCloud(cx, cy - 3, 0.95f, C_CLOUDICON);
    for (int k = -1; k <= 1; k++) {
      if (snow) spr.fillSmoothCircle(cx + k * 6, cy + 11 + (k ? 0 : 2), 2, TFT_WHITE);
      else spr.drawWideLine(cx + k * 6 + 1, cy + 9, cx + k * 6 - 1, cy + 14, 1.5f, c565(C_DEW));
    }
  }
  else if (code >= 0) wxCloud(cx, cy, 1.05f, C_CLOUDICON);
}

static void wxTrace(int x, int y, int w, int h, const Weather& wx) {
  float lo = 1e9f, hi = -1e9f;
  for (int i = 0; i < WX_TRACE; i++) for (float v : {wx.traceT[i], wx.traceD[i]}) if (!isnan(v)) { lo = min(lo, v); hi = max(hi, v); }
  if (lo > hi) { text("No history yet", x, y + h / 2, fSmall, C_IDLE, middle_left); return; }
  lo -= 0.5f; hi += 0.5f;
  auto px = [&](int i) { return x + i * (float)w / (WX_TRACE - 1); };
  auto py = [&](float v) { return y + h - (v - lo) / (hi - lo) * h; };
  if (lo < 0 && hi > 0) for (int xx = x; xx < x + w; xx += 5) spr.drawFastHLine(xx, (int)py(0), 2, c565(0x2A3346));
  // shade the gap between temperature and dew point
  uint16_t gap = c565(blend(C_TILE, C_DEW, 0.12f));
  for (int i = 0; i + 1 < WX_TRACE; i++) {
    if (isnan(wx.traceT[i]) || isnan(wx.traceT[i + 1]) || isnan(wx.traceD[i]) || isnan(wx.traceD[i + 1])) continue;
    for (int xx = (int)px(i); xx <= (int)px(i + 1); xx++) {
      float t = (xx - px(i)) / (px(i + 1) - px(i));
      int yT = (int)py(wx.traceT[i] + (wx.traceT[i + 1] - wx.traceT[i]) * t), yD = (int)py(wx.traceD[i] + (wx.traceD[i + 1] - wx.traceD[i]) * t);
      if (yD > yT) spr.drawFastVLine(xx, yT, yD - yT, gap);
    }
  }
  for (int pass = 0; pass < 2; pass++) {
    const float* a = pass ? wx.traceT : wx.traceD;
    uint16_t col = c565(pass ? C_TEMP : C_DEW);
    for (int i = 0; i + 1 < WX_TRACE; i++)
      if (!isnan(a[i]) && !isnan(a[i + 1])) spr.drawWideLine(px(i), py(a[i]), px(i + 1), py(a[i + 1]), 1.6f, col);
  }
  for (int i = WX_TRACE - 1; i >= 0; i--) if (!isnan(wx.traceT[i])) { spr.fillSmoothCircle(px(i), py(wx.traceT[i]), 3, c565(C_TEMP)); break; }
}

static const char* COMPASS[16] = {"N","NNE","NE","ENE","E","ESE","SE","SSE","S","SSW","SW","WSW","W","WNW","NW","NNW"};

void uiDrawWeather(const UiState& s, const Weather& wx, bool configured, const String& settingsUrl) {
  spr.fillScreen(c565(C_BG));
  char right[48]; bool stale = false;
  if (!configured) strcpy(right, "Weather Underground");
  else if (wx.haveCurrent && s.nowUtc) {
    long mins = max(0L, (long)(s.nowUtc - wx.obsUtc) / 60);
    stale = mins > 30;
    snprintf(right, sizeof right, "%s, %ld min ago", cfg.wuStation.c_str(), mins);
  } else snprintf(right, sizeof right, "%s", cfg.wuStation.c_str());
  header(s, right, stale);

  if (!configured || !wx.haveCurrent) {
    const char* title = configured ? "No weather data yet" : "Weather not set up";
    text(title, W / 2, 110, fTitle, C_FG, top_center);
    if (configured && wx.error.length()) text(wx.error.c_str(), W / 2, 156, fBody, C_MAYBE, top_center, W - 40);
    else text("Add your Weather Underground station ID and API key", W / 2, 156, fBody, C_MUTED, top_center, W - 40);
    text(("on the settings page: " + settingsUrl).c_str(), W / 2, 184, fBody, C_MUTED, top_center, W - 40);
    text("Swipe for the other screens", W / 2, H - 22, fSmall, C_IDLE, top_center);
    spr.pushSprite(0, 0);
    return;
  }

  char b[48], c[48];
  // ---- outside now ----
  spr.fillSmoothRoundRect(12, 32, 198, 158, 10, c565(C_TILE)); spr.drawRoundRect(12, 32, 198, 158, 10, c565(C_LINE));
  text("OUTSIDE NOW", 24, 52, fSmall, C_MUTED, baseline_left);
  fmtDeg(b, sizeof b, wx.temp, 1);
  text(b, 22, 98, fHuge, C_FG, baseline_left, 180);
  fmtDeg(c, sizeof c, wx.feels, 0);
  snprintf(b, sizeof b, "Feels %s", c);
  int fw = textFit(b, 24, 120, fBody, fSmall, C_MUTED, baseline_left, 90);
  if (!isnan(wx.change1h)) {  // trend triangle + change over the last hour
    int tx = 24 + fw + 14, ty = 114;
    if (wx.change1h < 0) spr.fillTriangle(tx - 5, ty - 4, tx + 5, ty - 4, tx, ty + 4, c565(C_MUTED));
    else spr.fillTriangle(tx - 5, ty + 4, tx + 5, ty + 4, tx, ty - 4, c565(C_MUTED));
    snprintf(b, sizeof b, "%.1f\xC2\xB0 in 1 h", fabsf(wx.change1h));
    textFit(b, tx + 9, 120, fBody, fSmall, C_MUTED, baseline_left, 198 - (tx + 9 - 12) - 6);
  }
  if (wx.haveHistory) wxTrace(24, 130, 174, 38, wx);
  else text("24 h trace after first update", 24, 150, fSmall, C_IDLE, baseline_left, 174);
  text("24 h", 24, 184, fSmall, C_IDLE, baseline_left);
  text("temp", 62, 184, fSmall, C_TEMP, baseline_left);
  text("dew point", 98, 184, fSmall, C_DEW, baseline_left);

  // ---- readings ----
  spr.fillSmoothRoundRect(220, 32, 248, 158, 10, c565(C_TILE)); spr.drawRoundRect(220, 32, 248, 158, 10, c565(C_LINE));
  snprintf(b, sizeof b, "%d%%", wx.humidity);
  if (isnan(wx.rainToday)) c[0] = 0; else snprintf(c, sizeof c, "%.1f mm rain", wx.rainToday);  // short enough for the cell
  wxReading(232, 36, "HUMIDITY", wx.humidity >= 0 ? b : MINUS, c, false, false);
  float spread = wx.temp - wx.dew; bool dewWarn = !isnan(spread) && spread < 3.0f;
  fmtDeg(b, sizeof b, wx.dew, 1);
  if (isnan(spread)) c[0] = 0;
  else snprintf(c, sizeof c, dewWarn ? "Dew risk (%.1f\xC2\xB0)" : "%.1f\xC2\xB0 spread", spread);
  wxReading(352, 36, "DEW POINT", b, c, dewWarn, dewWarn);
  bool gustWarn = !isnan(wx.gust) && wx.gust > 30;
  snprintf(b, sizeof b, isnan(wx.wind) ? "%s" : "%.0f km/h", isnan(wx.wind) ? MINUS : "");
  if (!isnan(wx.wind)) snprintf(b, sizeof b, "%.0f km/h", wx.wind);
  const char* dir = wx.windDir >= 0 ? COMPASS[((wx.windDir * 16 + 180) / 360) % 16] : "";
  if (isnan(wx.gust)) snprintf(c, sizeof c, "%s", dir); else snprintf(c, sizeof c, "%s%sgust %.0f", dir, *dir ? ", " : "", wx.gust);
  int vw = 0;
  wxReading(232, 94, "WIND", b, c, false, gustWarn, &vw);
  if (wx.windDir >= 0 && !isnan(wx.wind) && wx.wind >= 1) windArrow(232 + vw + 14, 121, wx.windDir);
  if (isnan(wx.pressure)) strcpy(b, MINUS); else snprintf(b, sizeof b, "%.1f", wx.pressure);
  const char* trend = isnan(wx.pressure3h) ? "" : fabsf(wx.pressure3h) < 0.5f ? ", steady" : wx.pressure3h > 0 ? ", rising" : ", falling";
  snprintf(c, sizeof c, "hPa%s", trend);
  wxReading(352, 94, "PRESSURE", b, c, false, false);
  text("CLOUD TONIGHT (WU)", 232, 161, fSmall, C_MUTED, baseline_left, 180);
  if (wx.cloudTonight >= 0) {
    uint32_t cc = wx.cloudTonight <= 30 ? C_GO : wx.cloudTonight <= 60 ? C_MAYBE : C_NO;
    spr.fillSmoothRoundRect(232, 168, 224, 10, 5, c565(0x1A2234));
    spr.fillSmoothRoundRect(232, 168, max(10, 224 * wx.cloudTonight / 100), 10, 5, c565(cc));
    snprintf(b, sizeof b, "%d%%", wx.cloudTonight);
    text(b, 456, 161, fBody, C_FG, baseline_right);
  } else text(MINUS, 456, 161, fBody, C_MUTED, baseline_right);

  // ---- five-day strip (night cloud and precip, since nights are what matter) ----
  const float x0 = 12, w5 = (468 - x0) / WX_DAYS; const int y0 = 198;
  for (int k = 0; k < WX_DAYS; k++) {
    const WxDay& d = wx.days[k];
    int x = (int)(x0 + k * w5), cx = (int)(x0 + k * w5 + w5 / 2), cw = (int)w5 - 4;
    uint32_t bg = k == 0 ? 0x111A2B : C_TILE;
    spr.fillSmoothRoundRect(x + 2, y0, cw, 114, 9, c565(bg)); spr.drawRoundRect(x + 2, y0, cw, 114, 9, c565(C_LINE));
    if (!wx.haveForecast) { text("Pending", cx, y0 + 57, fSmall, C_IDLE, middle_center, cw - 10); continue; }
    text(d.name, cx, y0 + 20, fSmall, k == 0 ? C_FG : C_MUTED, baseline_center, cw - 10);
    wxIcon(d.icon, cx, y0 + 42, bg);
    char lo[16], hi[16]; fmtDeg(lo, sizeof lo, d.lo, 0); fmtDeg(hi, sizeof hi, d.hi, 0);
    if (isnan(d.hi)) snprintf(b, sizeof b, "Low %s", lo); else snprintf(b, sizeof b, "%s / %s", hi, lo);
    textFit(b, cx, y0 + 76, fBody, fSmall, C_FG, baseline_center, cw - 10);
    if (d.pop >= 0) { snprintf(b, sizeof b, "%d%% precip", d.pop); text(b, cx, y0 + 93, fSmall, d.pop >= 50 ? C_MAYBE : C_MUTED, baseline_center, cw - 10); }
    if (d.cloud >= 0) { snprintf(b, sizeof b, "%d%% cloud", d.cloud); text(b, cx, y0 + 108, fSmall, C_MUTED, baseline_center, cw - 10); }
  }
  spr.pushSprite(0, 0);
}

void uiMessage(const char* title, const char* l1, const char* l2, const char* l3) {
  spr.fillScreen(c565(C_BG));
  text(title, W / 2, 86, fTitle, C_FG, top_center);
  text(l1, W / 2, 140, fBody, C_MUTED, top_center, W - 40);
  text(l2, W / 2, 168, fBody, C_MUTED, top_center, W - 40);
  text(l3, W / 2, 196, fBody, C_MUTED, top_center, W - 40);
  spr.pushSprite(0, 0);
}

// ---------- touch: polled every 10 ms in its own task, so drawing and network calls can't
// swallow a tap or swipe. Events are queued and picked up by the main loop. ----------
static QueueHandle_t touchQ = nullptr;

static TouchEvent pollTouch() {
  static bool wasDown = false, longFired = false, moved = false;
  static uint32_t downMs = 0;
  static int16_t x0 = 0, y0 = 0, xl = 0, yl = 0;  // press start and last position
  static uint8_t upCount = 0;                      // consecutive "not touched" reads
  lgfx::touch_point_t tp;
  bool raw = lcd.getTouch(&tp) > 0;
  // The controller occasionally reports a single missed read mid-press; treat 2 in a row (20 ms) as a release.
  if (raw) upCount = 0; else if (upCount < 255) upCount++;
  bool down = raw || (wasDown && upCount < 2);
  TouchEvent ev = TOUCH_NONE;
  if (down && !wasDown) { downMs = millis(); x0 = xl = tp.x; y0 = yl = tp.y; longFired = false; moved = false; }
  if (raw) {
    xl = tp.x; yl = tp.y;
    if (abs(tp.x - x0) > 25 || abs(tp.y - y0) > 25) moved = true;
  }
  if (down && !longFired && !moved && millis() - downMs >= LONG_PRESS_MS) { longFired = true; ev = TOUCH_LONG; }
  if (!down && wasDown && !longFired) {
    int dx = xl - x0, dy = yl - y0;
    if (!moved) ev = TOUCH_TAP;
    else if (abs(dx) >= 60 && abs(dx) > 2 * abs(dy)) ev = dx < 0 ? TOUCH_SWIPE_LEFT : TOUCH_SWIPE_RIGHT;
  }
  wasDown = down;
  return ev;
}

static void touchTask(void*) {
  for (;;) {
    TouchEvent ev = pollTouch();
    if (ev != TOUCH_NONE) xQueueSend(touchQ, &ev, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void startTouchTask() {
  touchQ = xQueueCreate(8, sizeof(TouchEvent));
  // Same core as the Arduino loop, one priority higher, so it pre-empts drawing briefly every 10 ms.
  xTaskCreatePinnedToCore(touchTask, "touch", 4096, nullptr, 2, nullptr, 1);
}

TouchEvent uiTouchEvent() {
  TouchEvent ev = TOUCH_NONE;
  if (touchQ) xQueueReceive(touchQ, &ev, 0);
  return ev;
}

void uiScreenshot(Stream& out) {
  const uint16_t* buf = (const uint16_t*)spr.getBuffer();
  if (!buf) { out.println("SHOT ERR"); return; }
  out.printf("SHOT %d %d rgb565be\n", W, H);
  const uint8_t* p = (const uint8_t*)buf;
  size_t total = (size_t)W * H * 2, sent = 0;
  while (sent < total) { size_t n = min((size_t)1024, total - sent); sent += out.write(p + sent, n); }
  out.println();
  out.println("SHOT END");
}
