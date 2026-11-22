#include "ui.h"
#include "config.h"
#include "settings.h"
#include "board.h"
#include "fonts.h"
#if defined(BOARD_INDICATOR)
  #include "lgfx_indicator.h"
  static LGFX_Indicator lcd;
#else
  #include "lgfx_wt32sc01plus.h"
  static LGFX_WT32SC01Plus lcd;
#endif

static LGFX_Sprite spr(&lcd);
static const int W = SCREEN_W, H = SCREEN_H;
static const bool TALL = SCREEN_TALL;
static const int OY = (H - 320) / 2;  // vertical offset that centres the 320-high setup and message layouts

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

// Word-wrapped text, top-left at (x, y), up to maxLines lines of lineH; the last line is shortened with "..".
static void wrapText(const char* s, int x, int y, int w, SmoothFont& f, uint32_t col, int lineH, int maxLines) {
  spr.setFont(&f.font);
  String rest = s; rest.trim();
  for (int line = 0; line < maxLines && rest.length(); line++) {
    int fit = rest.length();
    if (spr.textWidth(rest.c_str()) > w) {  // longest run of whole words that fits
      fit = 0;
      for (int i = 1; i <= (int)rest.length(); i++)
        if ((i == (int)rest.length() || rest[i] == ' ') && spr.textWidth(rest.substring(0, i).c_str()) <= w) fit = i;
      if (!fit) fit = rest.length();  // one long word: text() shortens it
    }
    bool last = line == maxLines - 1;
    text(last ? rest.c_str() : rest.substring(0, fit).c_str(), x, y + line * lineH, f, col, top_left, w);
    rest = rest.substring(fit); rest.trim();
  }
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
#if defined(BOARD_INDICATOR)
  lcd.begin();         // resets panel and touch through the I/O expander, then lcd.init()
  lcd.setRotation(2);  // panel is mounted upside down relative to its native scan direction
#else
  lcd.init();
  lcd.setRotation(1);  // landscape 480x320
#endif
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
  for (int k = 0; k < s.dotCount; k++) {
    int x = 236 + (k - (s.dotCount - 1) / 2.0f) * 14, y = 15;
    if (k == s.dotIndex) spr.fillSmoothCircle(x, y, 4, c565(C_FG));
    else spr.fillSmoothCircle(x, y, 3, c565(C_IDLE));
  }
}

// One footer column: small grey label over a white value, aligned with a tile.
// indent leaves room for an icon drawn before the value.
static int footY = 276;  // label row of the footer being drawn; the value sits 18 px below
static void footItem(int x, const char* label, const char* value, uint32_t valueCol, int indent = 0) {
  text(label, x + 2, footY, fSmall, C_MUTED, top_left, 146);
  text(value, x + 2 + indent, footY + 18, fBody, valueCol, top_left, 146 - indent);
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

static void drawFooter(const UiState& s, const Forecast& f, const Night* tn, const Verdict& vTn, int page, int y) {
  footY = y;
  char a[40], b[40], c[40];
  const SkyEvents& e = s.sky;
  switch (page) {
    case 0: {  // moon
      if (!s.skyValid) { footItem(12, "MOON", "Needs location", C_MUTED); break; }
      snprintf(a, sizeof a, "MOON  %d%%", (int)lroundf(e.moonIllum * 100));
      moonIcon(12 + 11, footY + 18 + 11, 9, e.moonIllum, e.moonWaxing);
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
  if (s.needsSetup) { uiFirstRunScreen(s); return; }
  spr.fillScreen(c565(C_BG));
  header(s);
  const Forecast* f = s.f;
  if (!f || !f->valid) {
    text("Waiting for the first chart...", W / 2, H / 2 - 10, fTitle, C_MUTED, middle_center);
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
  const int y0 = TALL ? 162 : 154, rh = TALL ? 18 : 11, rp = TALL ? 22 : 14;
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

  if (TALL) {  // room for all footer pages at once, no rotation
    for (int p = 0; p < FOOTER_PAGES; p++) {
      int y = 344 + p * 44;
      if (p) spr.drawFastHLine(12, y - 4, W - 24, c565(C_LINE));
      drawFooter(s, *f, tn, vTn, p, y);
    }
  } else {
    footerDots(s.footerPage);
    drawFooter(s, *f, tn, vTn, s.footerPage, 276);
  }
  spr.pushSprite(0, 0);
}

// Status screen: two columns of label/value rows, plus a QR code for the settings page.
static void statusColumn(int x, int w, const StatusItem* items, int n) {
  for (int k = 0; k < n; k++) {
    int y = TALL ? 46 + k * 42 : 44 + k * 34;
    text(items[k].label.c_str(), x, y, fSmall, C_MUTED, top_left, w);
    text(items[k].value.c_str(), x, y + 15, fBody, items[k].warn ? C_MAYBE : C_FG, top_left, w);
  }
}

void uiDrawStatus(const UiState& s, const StatusInfo& st) {
  spr.fillScreen(c565(C_BG));
  header(s);
  statusColumn(16, TALL ? 220 : 200, st.left, st.nLeft);
  statusColumn(TALL ? 252 : 232, TALL ? 216 : 130, st.right, st.nRight);
  if (TALL) {  // square screen: columns get the full width, QR code along the bottom
    spr.drawFastHLine(12, 362, W - 24, c565(C_LINE));
    if (st.qrUrl.length()) {
      const int qw = 92, qx = W - 18 - qw, qy = 374;
      spr.fillRoundRect(qx - 5, qy - 5, qw + 10, qw + 10, 8, TFT_WHITE);
      spr.qrcode(st.qrUrl.c_str(), qx, qy, qw, 3);
      text("Scan for the settings page", 16, 396, fBody, C_FG, top_left, qx - 40);
      text(st.qrUrl.c_str(), 16, 422, fBody, C_MUTED, top_left, qx - 40);
    }
  } else {
    if (st.qrUrl.length()) {
      const int qw = 96, qx = W - 12 - qw, qy = 50;
      spr.fillRoundRect(qx - 6, qy - 6, qw + 12, qw + 12, 8, TFT_WHITE);
      spr.qrcode(st.qrUrl.c_str(), qx, qy, qw, 3);
      text("Settings", qx + qw / 2, qy + qw + 12, fSmall, C_MUTED, top_center);
    }
    text("Swipe for the other screens", W / 2, H - 22, fSmall, C_IDLE, top_center);
  }
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

// Number with a small icon in front (raindrop = precip chance, cloud = cloud cover), centred on cx, text baseline y.
static void miniStat(const char* num, int cx, int y, bool cloud, uint32_t col, uint32_t bg) {
  spr.setFont(&fSmall.font);
  int tw = spr.textWidth(num), iw = 12, x = cx - (iw + 4 + tw) / 2, iy = y - 5;
  if (cloud) wxCloud(x + 6, iy, 0.45f, C_MUTED);
  else {
    uint16_t c = c565(C_DEW);
    spr.fillSmoothCircle(x + 6, iy + 2, 4, c);
    spr.fillTriangle(x + 2, iy + 1, x + 10, iy + 1, x + 6, iy - 7, c);
  }
  (void)bg;
  text(num, x + iw + 4, y, fSmall, col, baseline_left);
}

static const char* COMPASS[16] = {"N","NNE","NE","ENE","E","ESE","SE","SSE","S","SSW","SW","WSW","W","WNW","NW","NNW"};

void uiDrawWeather(const UiState& s, const Weather& wx, bool configured, const String& settingsUrl) {
  spr.fillScreen(c565(C_BG));
  char right[48]; bool stale = false;
  String src = wx.source.length() ? wx.source : (cfg.ecowittHost.length() ? String("EcoWitt") : cfg.wuStation);
  if (!configured) strcpy(right, "Weather");
  else if (wx.haveCurrent && s.nowUtc) {
    long mins = max(0L, (long)(s.nowUtc - wx.obsUtc) / 60);
    stale = mins > 30;
    snprintf(right, sizeof right, "%s, %ld min ago", src.c_str(), mins);
  } else snprintf(right, sizeof right, "%s", src.c_str());
  header(s, right, stale);

  if (!configured || !wx.haveCurrent) {
    const char* title = configured ? "No weather data yet" : "Weather not set up";
    text(title, W / 2, OY + 110, fTitle, C_FG, top_center);
    if (configured && wx.error.length()) text(wx.error.c_str(), W / 2, OY + 156, fBody, C_MAYBE, top_center, W - 40);
    else text("Add your Weather Underground station ID and API key", W / 2, OY + 156, fBody, C_MUTED, top_center, W - 40);
    text(("on the settings page: " + settingsUrl).c_str(), W / 2, OY + 184, fBody, C_MUTED, top_center, W - 40);
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

  // ---- text forecast (square screen only) ----
  if (TALL) {
    const int ty = 198, th = 148;
    spr.fillSmoothRoundRect(12, ty, 456, th, 10, c565(C_TILE)); spr.drawRoundRect(12, ty, 456, th, 10, c565(C_LINE));
    String title = wx.forecastTitle.length() ? wx.forecastTitle : String("FORECAST");
    text((title + " (WU)").c_str(), 24, ty + 20, fSmall, C_MUTED, baseline_left, 200);
    if (wx.today.valid) {  // today's headline numbers, top right
      char hi[16]; fmtDeg(hi, sizeof hi, wx.today.hi, 0);
      if (wx.today.pop >= 0) snprintf(b, sizeof b, "High %s, %d%% precip", hi, wx.today.pop); else snprintf(b, sizeof b, "High %s", hi);
      text(b, 456, ty + 20, fBody, C_FG, baseline_right, 220);
    }
    if (!wx.haveForecast && wx.wuError.length()) wrapText(("Forecast unavailable: " + wx.wuError).c_str(), 24, ty + 34, 432, fBody, C_MAYBE, 25, 4);
    else if (!wx.haveForecast) text(weatherHasForecast() ? "Pending" : "Needs a Weather Underground API key", 24, ty + 52, fBody, C_IDLE, baseline_left, 432);
    else if (!wx.forecastText.length()) text("No text forecast", 24, ty + 52, fBody, C_IDLE, baseline_left);
    else wrapText(wx.forecastText.c_str(), 24, ty + 32, 432, fTitle, C_FG, 28, 4);
  }

  // ---- day strip: today's daytime (while it lasts), then tonight and four more nights ----
  const WxDay* cells[WX_DAYS + 1]; int nc = 0;
  if (wx.today.valid) cells[nc++] = &wx.today;
  for (int k = 0; k < WX_DAYS; k++) cells[nc++] = &wx.days[k];
  const float x0 = 12, w5 = (468 - x0) / nc; const int y0 = TALL ? 356 : 198;
  for (int k = 0; k < nc; k++) {
    const WxDay& d = *cells[k];
    int x = (int)(x0 + k * w5), cx = (int)(x0 + k * w5 + w5 / 2), cw = (int)w5 - 4;
    bool tonight = &d == &wx.days[0], today = &d == &wx.today;
    uint32_t bg = tonight ? 0x111A2B : C_TILE;
    spr.fillSmoothRoundRect(x + 2, y0, cw, 114, 9, c565(bg)); spr.drawRoundRect(x + 2, y0, cw, 114, 9, c565(C_LINE));
    if (!wx.haveForecast) { if (TALL || !wx.wuError.length()) text("Pending", cx, y0 + 57, fSmall, C_IDLE, middle_center, cw - 10); continue; }
    text(d.name, cx, y0 + 20, fSmall, tonight || today ? C_FG : C_MUTED, baseline_center, cw - 8);
    wxIcon(d.icon, cx, y0 + 42, bg);
    char lo[16], hi[16]; fmtDeg(lo, sizeof lo, d.lo, 0); fmtDeg(hi, sizeof hi, d.hi, 0);
    if (isnan(d.hi)) snprintf(b, sizeof b, "Low %s", lo);
    else if (isnan(d.lo)) snprintf(b, sizeof b, "High %s", hi);
    else snprintf(b, sizeof b, "%s / %s", hi, lo);
    textFit(b, cx, y0 + 76, fBody, fSmall, C_FG, baseline_center, cw - 8);
    if (cw >= 84) {  // wide cells: words
      if (d.pop >= 0) { snprintf(b, sizeof b, "%d%% precip", d.pop); text(b, cx, y0 + 93, fSmall, d.pop >= 50 ? C_MAYBE : C_MUTED, baseline_center, cw - 8); }
      if (d.cloud >= 0) { snprintf(b, sizeof b, "%d%% cloud", d.cloud); text(b, cx, y0 + 108, fSmall, C_MUTED, baseline_center, cw - 8); }
    } else {         // six narrow cells: a small raindrop or cloud before the number
      if (d.pop >= 0) { snprintf(b, sizeof b, "%d%%", d.pop); miniStat(b, cx, y0 + 93, false, d.pop >= 50 ? C_MAYBE : C_MUTED, bg); }
      if (d.cloud >= 0) { snprintf(b, sizeof b, "%d%%", d.cloud); miniStat(b, cx, y0 + 108, true, C_MUTED, bg); }
    }
  }
  if (!TALL && !wx.haveForecast && wx.wuError.length())  // no text panel on the small screen: say why across the strip
    text(("Forecast unavailable: " + wx.wuError).c_str(), W / 2, y0 + 57, fBody, C_MAYBE, middle_center, W - 40);
  spr.pushSprite(0, 0);
}

// ---------- observatory screen (layout follows mockup/observatory-screen.html) ----------
static const uint32_t C_BLUE = 0x6FB6FF, C_TRACK = 0x1A2234;

static void hm(char* b, size_t n, time_t t) { struct tm tmv; localTm(t, tmv); snprintf(b, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min); }

// Status pill: tinted fill, outline and text in col. Returns its width.
static int pill(int x, int y, const char* label, uint32_t col, uint32_t bg = C_TILE) {
  spr.setFont(&fSmall.font);
  int w = spr.textWidth(label) + 14, h = 21;
  spr.fillSmoothRoundRect(x, y, w, h, h / 2, c565(blend(bg, col, 0.16f)));
  spr.drawRoundRect(x, y, w, h, h / 2, c565(blend(bg, col, 0.55f)));
  text(label, x + 7, y + 16, fSmall, col, baseline_left);
  return w;
}
static int pillWidth(const char* label) { spr.setFont(&fSmall.font); return spr.textWidth(label) + 14; }

static void glowDot(int x, int y, int r, uint32_t col, bool glow) {
  if (glow) for (int rr = r * 2; rr > r; rr--) { float t = 1.0f - (float)(rr - r) / r; spr.fillCircle(x, y, rr, c565(blend(C_TILE, col, 0.5f * t * t))); }
  spr.fillSmoothCircle(x, y, r, c565(col));
}

static void barH(int x, int y, int w, int h, float frac, uint32_t col) {
  spr.fillSmoothRoundRect(x, y, w, h, h / 2, c565(C_TRACK));
  if (frac > 0) spr.fillSmoothRoundRect(x, y, max(h, (int)(w * min(frac, 1.0f))), h, h / 2, c565(col));
}

// Small building with a roll-off roof: closed, part way, or rolled off onto its rails.
static void roofIcon(int x, int y, float s, int shutter, uint32_t col) {
  auto X = [&](float v) { return x + (int)lroundf(v * s); };
  auto Y = [&](float v) { return y + (int)lroundf(v * s); };
  spr.fillRect(X(0), Y(14), X(36) - X(0), Y(32) - Y(14), c565(0x2A3245));
  spr.fillRect(X(36), Y(14), X(54) - X(36), max(2, (int)(2 * s)), c565(0x3A4358));
  float off = shutter == SHUTTER_OPEN ? 18 : (shutter == SHUTTER_OPENING || shutter == SHUTTER_CLOSING) ? 9 : 0;
  spr.fillTriangle(X(-2 + off), Y(15), X(18 + off), Y(5), X(38 + off), Y(15), c565(col));
  if (shutter == SHUTTER_ERROR) text("!", X(18), Y(30), fBodyB, C_NO, baseline_center);
}

// Largest of the given fonts that fits maxW (the last one is shortened if even it doesn't fit).
static void fitWord(const char* s, int x, int y, uint32_t col, int maxW, SmoothFont& a, SmoothFont& b, SmoothFont& c) {
  SmoothFont* fs[] = {&a, &b, &c};
  for (SmoothFont* f : fs) { spr.setFont(&f->font); if (spr.textWidth(s) <= maxW || f == &c) { text(s, x, y, *f, col, baseline_left, maxW); return; } }
}

static void fmtLux(char* b, size_t n, float v) {
  if (isnan(v)) snprintf(b, n, "%s", MINUS);
  else if (v < 0.01f) snprintf(b, n, "%.4f", v);
  else if (v < 10) snprintf(b, n, "%.2f", v);
  else if (v < 10000) snprintf(b, n, "%.0f", v);
  else snprintf(b, n, "%.0f %03.0f", floorf(v / 1000), fmodf(v, 1000));
}

struct RigText { const char* word; uint32_t col; bool glow; String l1, l2, l3, right; };
static RigText rigText(const ObsRig& r, time_t now) {
  RigText t{"", C_IDLE, false, "", "", "", ""};
  char b[24];
  String what = r.target;
  if (r.filter.length()) what += (what.length() ? "  \xC2\xB7  " : "") + r.filter;
  if (!isnan(r.exposure)) what += (what.length() ? "  \xC2\xB7  " : "") + String((int)lroundf(r.exposure)) + " s";
  String frames = String(r.frames) + (r.frames == 1 ? " frame" : " frames") + " tonight";
  String lastImg = r.lastImage && now ? "Last image " + String(max(0L, (long)(now - r.lastImage) / 60)) + " min ago" : String("No images yet tonight");
  if (!isnan(r.rms) || !isnan(r.hfr)) {
    String rms = isnan(r.rms) ? String(MINUS) : String(r.rms, 2) + "\xE2\x80\xB3";
    String hfr = isnan(r.hfr) ? String(MINUS) : String(r.hfr, 2);
    t.right = "RMS " + rms + "   HFR " + hfr;
  }
  switch (r.state) {
    case RIG_IMAGING: t.word = "IMAGING"; t.col = C_GO; t.glow = true;
      t.l1 = what.length() ? what : String("Sequence running"); t.l2 = frames; t.l3 = lastImg;
      if (r.guider == "LostLock") { t.l2 = "Guiding: lost the star"; }
      break;
    case RIG_WAITING: t.word = "WAITING"; t.col = C_MAYBE;
      t.l1 = what.length() ? what : String("Sequence running"); t.l2 = r.frames ? frames : String("Sequence running, not exposing"); t.l3 = lastImg; break;
    case RIG_FINISHED: t.word = "FINISHED"; t.col = C_GO;
      t.l1 = (r.target.length() ? r.target + "  \xC2\xB7  " : String("")) + String(r.frames) + " frames";
      if (r.endedAt) { hm(b, sizeof b, r.endedAt); t.l2 = String("Finished ") + b + (r.online ? (r.parked ? ", mount parked" : ", mount not parked") : ""); }
      break;
    case RIG_LOST: t.word = "LOST"; t.col = C_NO;
      t.l1 = (what.length() ? what + "  \xC2\xB7  " : String("")) + "frame " + String(r.frames);
      if (r.endedAt) { hm(b, sizeof b, r.endedAt); t.l2 = String("NINA stopped answering at ") + b; }
      break;
    case RIG_ERROR: t.word = "ERROR"; t.col = C_NO;
      t.l1 = r.error.length() ? r.error : String("NINA reports an error"); t.l2 = frames; break;
    case RIG_IDLE: t.word = "IDLE"; t.col = C_IDLE;
      t.l1 = "NINA running, no sequence"; t.l2 = r.parked ? "Mount parked" : r.tracking ? "Mount tracking" : "Mount not parked";
      t.l3 = r.lastRun; t.right = ""; break;
    case RIG_INACTIVE: t.word = "INACTIVE"; t.col = C_IDLE;
      t.l1 = "Not running"; t.l2 = r.lastRun;
      if (r.lastSeen) { hm(b, sizeof b, r.lastSeen); t.l3 = String("NINA offline since ") + b; }
      t.right = ""; break;
    default: t.word = "CHECKING"; t.col = C_IDLE; t.l1 = "Waiting for the first answer"; t.right = ""; break;
  }
  return t;
}

void uiDrawObservatory(const UiState& s, const Observatory& o, const String& settingsUrl) {
  spr.fillScreen(c565(C_BG));
  header(s, "Observatory");
  bool any = o.roof.configured || o.safety.configured || o.sky.configured || o.rain.configured || o.nRigs || o.nPark;
  if (!any) {
    text("Observatory not set up", W / 2, OY + 110, fTitle, C_FG, top_center);
    text("Add your roof, safety monitor, sky sensor and rigs", W / 2, OY + 156, fBody, C_MUTED, top_center, W - 40);
    text(("on the settings page: " + settingsUrl).c_str(), W / 2, OY + 184, fBody, C_MUTED, top_center, W - 40);
    spr.pushSprite(0, 0);
    return;
  }
  char b[64], c[64];
  time_t now = s.nowUtc;

  // ---- roof ----
  const char* rw = MINUS; uint32_t rcol = C_IDLE; String rsub = "Not set up";
  int sh = o.roof.shutter;
  if (o.roof.configured) {
    if (!o.roof.online) { rw = "NO ANSWER"; rcol = C_MAYBE; rsub = o.roof.name.length() ? o.roof.name : String("Roof controller"); sh = -1; }
    else {
      switch (sh) {
        case SHUTTER_OPEN: rw = "OPEN"; rcol = C_GO; break;
        case SHUTTER_CLOSED: rw = "CLOSED"; rcol = C_MUTED; break;
        case SHUTTER_OPENING: rw = "OPENING"; rcol = C_MAYBE; break;
        case SHUTTER_CLOSING: rw = "CLOSING"; rcol = C_MAYBE; break;
        default: rw = "ERROR"; rcol = C_NO; break;
      }
      if (sh == SHUTTER_ERROR) rsub = o.roof.error;
      else if (o.roof.since) { hm(b, sizeof b, o.roof.since);
        rsub = (sh == SHUTTER_OPEN ? "Open since " : sh == SHUTTER_CLOSED ? "Closed since " : "Started ") + String(b); }
      else rsub = o.roof.name.length() ? o.roof.name : String("Roll-off roof");
    }
  }
  // ---- safety ----
  const char* sw = MINUS; uint32_t scol = C_IDLE;
  if (o.safety.configured) { if (!o.safety.online && !o.safety.reason.length()) { sw = "NO DATA"; scol = C_MAYBE; } else if (o.safety.safe) { sw = "SAFE"; scol = C_GO; } else { sw = "UNSAFE"; scol = C_NO; } }
  String rain; uint32_t rainCol = C_MUTED;
  if (o.rain.configured) { rain = String("Rain sensor: ") + (!o.rain.online ? "no data" : o.rain.wet ? "WET" : "dry"); rainCol = !o.rain.online ? C_MAYBE : o.rain.wet ? C_NO : C_MUTED; }
  String reason = (o.safety.configured && !o.safety.safe) ? o.safety.reason : String("");

  // ---- sky ----
  bool skyOk = o.sky.configured && o.sky.online;
  float cl = skyOk ? o.sky.cloud : NAN;
  uint32_t ccol = isnan(cl) ? C_IDLE : cl <= 30 ? C_GO : cl <= 60 ? C_MAYBE : C_NO;
  char cloudTxt[12]; if (isnan(cl)) snprintf(cloudTxt, sizeof cloudTxt, "%s", MINUS); else snprintf(cloudTxt, sizeof cloudTxt, "%d%%", (int)lroundf(cl));
  char sqm[16], lux[24], skyT[16], amb[24];
  if (skyOk && !isnan(o.sky.sqm)) snprintf(sqm, sizeof sqm, "%.2f", o.sky.sqm); else snprintf(sqm, sizeof sqm, "%s", MINUS);
  fmtLux(lux, sizeof lux, skyOk ? o.sky.lux : NAN);
  fmtDeg(skyT, sizeof skyT, skyOk ? o.sky.skyT : NAN, 1);
  if (skyOk && !isnan(o.sky.ambient)) { fmtDeg(c, sizeof c, o.sky.ambient, 1); snprintf(amb, sizeof amb, "air %s", c); } else amb[0] = 0;

  // ---- rigs on this page ----
  int pages = max(1, (o.nRigs + OBS_RIGS_PER_PAGE - 1) / OBS_RIGS_PER_PAGE), page = s.rigPage % pages;
  int r0 = page * OBS_RIGS_PER_PAGE, r1 = min(o.nRigs, r0 + OBS_RIGS_PER_PAGE);
  auto pageDots = [&](int x, int y) { if (pages < 2) return; for (int k = 0; k < pages; k++) spr.fillSmoothCircle(x + k * 12, y, k == page ? 3 : 2, c565(k == page ? C_FG : C_IDLE)); };
  auto parkCol = [](const ObsPark& p) { return !p.online ? C_NO : p.parked ? C_GO : C_BLUE; };
  auto parkWord = [](const ObsPark& p) { return !p.online ? "OFFLINE" : p.parked ? "PARKED" : "UNPARKED"; };

  if (TALL) {
    // roof + safety tiles
    spr.fillSmoothRoundRect(12, 32, 224, 104, 10, c565(C_TILE)); spr.drawRoundRect(12, 32, 224, 104, 10, c565(C_LINE));
    text("ROOF", 24, 52, fSmall, C_MUTED, baseline_left);
    fitWord(rw, 24, 92, rcol, 134, fBig, fTitle, fBodyB);
    text(rsub.c_str(), 24, 122, fBody, sh == SHUTTER_ERROR ? C_NO : C_MUTED, baseline_left, 200);
    if (o.roof.configured) roofIcon(164, 52, 1.1f, sh, rcol);
    spr.fillSmoothRoundRect(244, 32, 224, 104, 10, c565(C_TILE)); spr.drawRoundRect(244, 32, 224, 104, 10, c565(C_LINE));
    text("SAFETY", 256, 52, fSmall, C_MUTED, baseline_left);
    glowDot(268, 79, 9, scol, o.safety.configured);
    fitWord(sw, 286, 92, scol, 170, fBig, fTitle, fBodyB);
    if (reason.length()) {
      if (rain.length()) text(rain.c_str(), 256, 113, fSmall, rainCol, baseline_left, 204);
      text(reason.c_str(), 256, rain.length() ? 130 : 122, fSmall, C_NO, baseline_left, 204);
    } else if (rain.length()) text(rain.c_str(), 256, 122, fBody, rainCol, baseline_left, 204);

    // sky
    spr.fillSmoothRoundRect(12, 144, 456, 78, 10, c565(C_TILE)); spr.drawRoundRect(12, 144, 456, 78, 10, c565(C_LINE));
    text("CLOUD COVER", 24, 164, fSmall, C_MUTED, baseline_left);
    text(cloudTxt, 24, 202, fBig, ccol, baseline_left);
    barH(104, 188, 104, 9, isnan(cl) ? 0 : cl / 100, ccol);
    struct { const char* l; const char* v; const char* u; } cols[] = {{"SQM", sqm, "mag/arcsec\xC2\xB2"}, {"BRIGHTNESS", lux, "lux"}, {"SKY TEMP", skyT, amb}};
    for (int k = 0; k < 3; k++) {
      int x = 224 + k * 82;
      text(cols[k].l, x, 164, fSmall, C_MUTED, baseline_left, 80);
      text(cols[k].v, x, 192, fBodyB, strcmp(cols[k].v, MINUS) ? C_FG : C_IDLE, baseline_left, 80);
      if (strcmp(cols[k].v, MINUS)) text(cols[k].u, x, 211, fSmall, C_IDLE, baseline_left, 80);
    }

    // park sensors: pills when they fit, else coloured dots
    text("MOUNT PARK SENSORS", 12, 244, fSmall, C_MUTED, baseline_left);
    if (!o.nPark) text("None heard yet", 12, 268, fBody, C_IDLE, baseline_left);
    else {
      int need = 0;
      for (int i = 0; i < o.nPark; i++) { spr.setFont(&fBody.font); need += spr.textWidth(o.park[i].name.c_str()) + 6 + pillWidth(parkWord(o.park[i])) + 14; }
      int px = 12;
      for (int i = 0; i < o.nPark; i++) {
        const ObsPark& p = o.park[i];
        if (need <= 456) { text(p.name.c_str(), px, 268, fBody, C_FG, baseline_left); spr.setFont(&fBody.font); px += spr.textWidth(p.name.c_str()) + 6; px += pill(px, 252, parkWord(p), parkCol(p), C_BG) + 14; }
        else { spr.fillSmoothCircle(px + 5, 262, 5, c565(parkCol(p))); text(p.name.c_str(), px + 15, 268, fBody, C_FG, baseline_left); spr.setFont(&fBody.font); px += spr.textWidth(p.name.c_str()) + 30; }
      }
    }

    // rigs
    text("RIGS", 12, 296, fSmall, C_MUTED, baseline_left); pageDots(54, 291);
    if (!o.nRigs) text("No rigs set up", 12, 322, fBody, C_IDLE, baseline_left);
    for (int i = r0; i < r1; i++) {
      const ObsRig& r = o.rigs[i]; RigText t = rigText(r, now);
      int y = 304 + (i - r0) * 84;
      spr.fillSmoothRoundRect(12, y, 456, 76, 10, c565(C_TILE)); spr.drawRoundRect(12, y, 456, 76, 10, c565(C_LINE));
      glowDot(28, y + 19, 6, t.col, t.glow);
      text(r.name.c_str(), 42, y + 26, fBodyB, C_FG, baseline_left, 106);
      pill(152, y + 9, t.word, t.col);
      text(t.l1.c_str(), 252, y + 25, fBody, C_FG, baseline_left, 204);
      text(t.l2.c_str(), 42, y + 48, fBody, t.word[0] == 'L' ? C_NO : C_MUTED, baseline_left, 250);
      if (t.l3.length()) text(t.l3.c_str(), 42, y + 68, fSmall, C_IDLE, baseline_left, 250);
      if (t.right.length()) text(t.right.c_str(), 456, y + 68, fSmall, C_MUTED, baseline_right, 180);
    }
  } else {
    // roof, safety, cloud tiles
    spr.fillSmoothRoundRect(12, 32, 148, 70, 10, c565(C_TILE)); spr.drawRoundRect(12, 32, 148, 70, 10, c565(C_LINE));
    text("ROOF", 22, 50, fSmall, C_MUTED, baseline_left);
    fitWord(rw, 22, 86, rcol, 80, fTitle, fBodyB, fBody);
    if (o.roof.configured) roofIcon(104, 50, 0.8f, sh, rcol);
    spr.fillSmoothRoundRect(166, 32, 148, 70, 10, c565(C_TILE)); spr.drawRoundRect(166, 32, 148, 70, 10, c565(C_LINE));
    text("SAFETY", 176, 50, fSmall, C_MUTED, baseline_left);
    fitWord(sw, 176, 86, scol, 128, fTitle, fBodyB, fBody);
    spr.fillSmoothRoundRect(320, 32, 148, 70, 10, c565(C_TILE)); spr.drawRoundRect(320, 32, 148, 70, 10, c565(C_LINE));
    text("CLOUD", 330, 50, fSmall, C_MUTED, baseline_left);
    text(cloudTxt, 330, 86, fTitle, ccol, baseline_left);
    barH(392, 73, 64, 8, isnan(cl) ? 0 : cl / 100, ccol);
    // one line: why it's unsafe, or the sky numbers
    String line;
    if (reason.length()) line = "Unsafe: " + reason + (rain.length() ? "   \xC2\xB7   " + rain : String(""));
    else {
      if (o.rain.configured) line = String("Rain ") + (!o.rain.online ? "no data" : o.rain.wet ? "WET" : "dry") + "   \xC2\xB7   ";
      if (skyOk) line += String("SQM ") + sqm + "   \xC2\xB7   " + lux + " lux   \xC2\xB7   sky " + skyT + "   \xC2\xB7   ";
      line += rsub;
    }
    text(line.c_str(), 12, 124, fSmall, reason.length() ? C_NO : C_MUTED, baseline_left, 456);
    // park sensors
    text("PARK", 12, 150, fSmall, C_MUTED, baseline_left);
    if (!o.nPark) text("None heard yet", 56, 150, fSmall, C_IDLE, baseline_left);
    int px = 56;
    for (int i = 0; i < o.nPark && px < 430; i++) {
      spr.fillSmoothCircle(px + 4, 145, 4, c565(parkCol(o.park[i])));
      text(o.park[i].name.c_str(), px + 13, 150, fSmall, C_FG, baseline_left, 140);
      spr.setFont(&fSmall.font); px += min(140, (int)spr.textWidth(o.park[i].name.c_str())) + 24;
    }
    pageDots(452, 146);
    if (!o.nRigs) text("No rigs set up", 12, 186, fBody, C_IDLE, baseline_left);
    for (int i = r0; i < r1; i++) {
      const ObsRig& r = o.rigs[i]; RigText t = rigText(r, now);
      int y = 162 + (i - r0) * 76;
      spr.fillSmoothRoundRect(12, y, 456, 68, 10, c565(C_TILE)); spr.drawRoundRect(12, y, 456, 68, 10, c565(C_LINE));
      glowDot(26, y + 16, 5, t.col, t.glow);
      text(r.name.c_str(), 38, y + 22, fBody, C_FG, baseline_left, 92);
      pill(134, y + 6, t.word, t.col);
      text(t.l1.c_str(), 236, y + 21, fSmall, C_FG, baseline_left, 222);
      text(t.l2.c_str(), 38, y + 42, fSmall, t.word[0] == 'L' ? C_NO : C_MUTED, baseline_left, 270);
      if (t.l3.length()) text(t.l3.c_str(), 38, y + 61, fSmall, C_IDLE, baseline_left, 270);
      if (t.right.length()) text(t.right.c_str(), 458, y + 42, fSmall, C_MUTED, baseline_right, 150);
    }
  }
  spr.pushSprite(0, 0);
}

// ---------- setup screens ----------
static void qrPanel(int x, int y, int size, const char* data) {
  spr.fillRoundRect(x - 8, y - 8, size + 16, size + 16, 10, TFT_WHITE);
  spr.qrcode(data, x, y, size, 3);
}

// Escape ; , : \ and " for the WIFI: QR format.
static String wifiQrEscape(const char* s) {
  String o;
  for (; *s; s++) { if (strchr(";,:\\\"", *s)) o += '\\'; o += *s; }
  return o;
}

void uiWifiSetupScreen(const char* apName, const String& setupUrl) {
  spr.fillScreen(c565(C_BG));
  text("Wi-Fi setup", W / 2, OY + 10, fTitle, C_FG, top_center);
  const int qs = 132, y = OY + 62;
  const int x1 = 120 - qs / 2, x2 = 360 - qs / 2;
  String wifi = "WIFI:T:nopass;S:" + wifiQrEscape(apName) + ";;";
  qrPanel(x1, y, qs, wifi.c_str());
  qrPanel(x2, y, qs, setupUrl.c_str());
  text("1. Join the setup network", 120, OY + 214, fBodyB, C_FG, top_center, 220);
  text(apName, 120, OY + 240, fBody, C_MUTED, top_center, 220);
  text("2. Open the setup page", 360, OY + 214, fBodyB, C_FG, top_center, 220);
  text(setupUrl.c_str(), 360, OY + 240, fBody, C_MUTED, top_center, 220);
  text("Scan with your phone camera. The page often opens after step 1.", W / 2, OY + 288, fSmall, C_IDLE, top_center, W - 24);
  spr.pushSprite(0, 0);
}

void uiFirstRunScreen(const UiState& s) {
  spr.fillScreen(c565(C_BG));
  header(s);
  const int qs = 150, qx = W - 24 - qs, qy = OY + 70;
  qrPanel(qx, qy, qs, s.settingsUrl.c_str());
  text("Almost there", 24, OY + 64, fTitle, C_FG, top_left);
  text("Wi-Fi is connected. Scan the code", 24, OY + 112, fBody, C_MUTED, top_left, qx - 40);
  text("to open the settings page, then", 24, OY + 138, fBody, C_MUTED, top_left, qx - 40);
  text("add your Clear Sky Chart link", 24, OY + 164, fBody, C_MUTED, top_left, qx - 40);
  text("and your time zone.", 24, OY + 190, fBody, C_MUTED, top_left, qx - 40);
  text(s.settingsUrl.c_str(), 24, OY + 232, fBodyB, C_FG, top_left, qx - 40);
  text("Settings", qx + qs / 2, qy + qs + 16, fSmall, C_MUTED, top_center);
  spr.pushSprite(0, 0);
}

void uiMessage(const char* title, const char* l1, const char* l2, const char* l3) {
  spr.fillScreen(c565(C_BG));
  text(title, W / 2, OY + 86, fTitle, C_FG, top_center);
  text(l1, W / 2, OY + 140, fBody, C_MUTED, top_center, W - 40);
  text(l2, W / 2, OY + 168, fBody, C_MUTED, top_center, W - 40);
  text(l3, W / 2, OY + 196, fBody, C_MUTED, top_center, W - 40);
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
