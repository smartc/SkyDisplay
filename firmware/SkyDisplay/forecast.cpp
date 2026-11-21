#include "forecast.h"
#include "config.h"
#include "settings.h"
#include "src/gif/AnimatedGIF.h"

// ---------- chart geometry (measured from the Dark Dragons chart.gif) ----------
// Each row is sampled at one y, 5 px into each cell.
static const int Y_RDPS = 82, Y_ECMWF = 98, Y_HRRR = 114, Y_TRANS = 130,
                 Y_SEEING = 146, Y_DARK = 162, Y_WIND = 226, Y_HUM = 242;
static const int ROW_YS[] = {Y_RDPS, Y_ECMWF, Y_HRRR, Y_TRANS, Y_SEEING, Y_DARK, Y_WIND, Y_HUM};
static const int NROWS = sizeof(ROW_YS) / sizeof(ROW_YS[0]);
static const int GRID_X0 = 131;          // search for cells right of the row labels
static const int FIRST_CELL_X = 134;     // expected x of column 0 (layout check)

static void markNights(Forecast& f);

// ---------- colour keys (cleardarksky.com/t/*_forecast_scale.txt) ----------
static const uint32_t CLOUD_KEY[11] = {0xFAFAFA,0xE9E9E9,0xC1C1C1,0xADEDED,0x99D9D9,0x76B6F6,
                                       0x62A2E2,0x4E8ECE,0x2666A6,0x135393,0x003E7E};
static const uint32_t FIVE_KEY[6]   = {0xF9F9F9,0xC7C7C7,0x95D5D5,0x63A3E3,0x2C6CAC,0x003F7F};
static const uint32_t HUM_KEY[16]   = {0x08035D,0x0D4D8D,0x3070B0,0x4E8ECE,0x71B1F1,0x80C0C0,0x09FEED,0x55FAAD,
                                       0x94FE6A,0xEAFB16,0xFEC600,0xFC8602,0xFE3401,0xEA0000,0xB70000,0xE10000};

static int nearest(uint32_t c, const uint32_t* key, int n) {
  if (c == 0) return -1;  // black = no data
  int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
  int best = 0; long bestD = LONG_MAX;
  for (int i = 0; i < n; i++) {
    int dr = r - ((key[i] >> 16) & 255), dg = g - ((key[i] >> 8) & 255), db = b - (key[i] & 255);
    long d = (long)dr * dr + (long)dg * dg + (long)db * db;
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

// ---------- GIF decode: keep only the rows we sample ----------
static AnimatedGIF gif;
static uint8_t* rowBuf = nullptr;   // NROWS * width * 3 bytes (RGB888), in PSRAM
static int gifW = 0;

static void onGifLine(GIFDRAW* d) {
  int y = d->iY + d->y;
  for (int r = 0; r < NROWS; r++) {
    if (ROW_YS[r] != y) continue;
    const uint8_t* pal = d->pPalette24;
    uint8_t* out = rowBuf + (size_t)r * gifW * 3;
    for (int x = 0; x < d->iWidth; x++) {
      int X = d->iX + x;
      if (X < 0 || X >= gifW) continue;
      const uint8_t* p = pal + d->pPixels[x] * 3;
      out[X * 3] = p[0]; out[X * 3 + 1] = p[1]; out[X * 3 + 2] = p[2];
    }
  }
}

static inline uint32_t px(int row, int x) {
  const uint8_t* p = rowBuf + ((size_t)row * gifW + x) * 3;
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

String decodeChartGif(const uint8_t* data, int len, Forecast& f) {
  f.valid = false;
  gif.begin(GIF_PALETTE_RGB888);
  if (!gif.open((uint8_t*)data, len, onGifLine)) return "GIF open failed (" + String(gif.getLastError()) + ")";
  gifW = gif.getCanvasWidth();
  int h = gif.getCanvasHeight();
  if (gifW < 600 || h < 250) { gif.close(); return "unexpected size " + String(gifW) + "x" + String(h); }
  free(rowBuf);
  rowBuf = (uint8_t*)ps_calloc((size_t)NROWS * gifW * 3, 1);
  if (!rowBuf) { gif.close(); return "out of memory"; }
  gif.playFrame(false, nullptr);
  gif.close();

  // Find cell starts on the RDPS row: a non-black pixel after a black one.
  int starts[MAX_COLS]; int n = 0;
  for (int x = GRID_X0; x < gifW && n < MAX_COLS; x++)
    if (px(0, x) != 0 && px(0, x - 1) == 0) starts[n++] = x;

  // Layout check: first cell where we expect it, sensible count and spacing.
  if (n < 48 || abs(starts[0] - FIRST_CELL_X) > 2) return "chart layout changed (n=" + String(n) + ", x0=" + String(n ? starts[0] : -1) + ")";
  for (int i = 1; i < n; i++) {
    int p = starts[i] - starts[i - 1];
    if (p < 12 || p > 16) return "chart layout changed (pitch " + String(p) + " at col " + String(i) + ")";
  }

  for (int i = 0; i < n; i++) {
    int x = starts[i] + 5;
    f.rdps[i]   = nearest(px(0, x), CLOUD_KEY, 11);
    f.ecmwf[i]  = nearest(px(1, x), CLOUD_KEY, 11);
    f.hrrr[i]   = nearest(px(2, x), CLOUD_KEY, 11);
    f.trans[i]  = nearest(px(3, x), FIVE_KEY, 6);
    f.seeing[i] = nearest(px(4, x), FIVE_KEY, 6);
    f.dark[i]   = px(5, x);
    f.wind[i]   = nearest(px(6, x), FIVE_KEY, 6);
    f.hum[i]    = nearest(px(7, x), HUM_KEY, 16);
  }
  f.n = n;
  markNights(f);
  f.valid = true;
  return "";
}

// ---------- time ----------
static long daysFromCivil(int y, unsigned m, unsigned d) {  // Howard Hinnant's algorithm
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

time_t parseIsoUtc(const char* s) {
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  return (time_t)(daysFromCivil(Y, M, D) * 86400L + h * 3600L + m * 60L + sec);
}

int localHour(const Forecast& f, int i) {  // uses the configured time zone (TZ env)
  time_t t = f.startUtc + (time_t)i * 3600;
  struct tm tmv; localtime_r(&t, &tmv);
  return tmv.tm_hour;
}

// ---------- scoring (same rules as the mockups) ----------
// Darkness row colours: cream/orange (red > 0) = daylight; blues = sun down, where
// navy (green < 0x20) is dark with the moon down, medium blue is moonlit, and bright
// cyan is twilight. Near full moon, moonlit cells (~0x0096FF) are almost the same
// colour as twilight, so twilight is identified by position instead: the bright
// cells at each end of a sun-down run, next to daylight.
static const int TWILIGHT_GREEN = 0x90;

static void markNights(Forecast& f) {
  int i = 0;
  while (i < f.n) {
    if (((f.dark[i] >> 16) & 255) != 0) { f.night[i++] = false; continue; }
    int s = i; while (i < f.n && ((f.dark[i] >> 16) & 255) == 0) i++;
    int e = i - 1;
    for (int k = s; k <= e; k++) f.night[k] = true;
    // Trim twilight off a run's ends, but not off a run cut by the chart's edge.
    if (s > 0) for (int k = s; k <= e && ((f.dark[k] >> 8) & 255) >= TWILIGHT_GREEN; k++) f.night[k] = false;
    if (i < f.n) for (int k = e; k >= s && ((f.dark[k] >> 8) & 255) >= TWILIGHT_GREEN; k--) f.night[k] = false;
  }
}

bool isDark(const Forecast& f, int i) { return i >= 0 && i < f.n && f.night[i]; }

bool isMoonlit(const Forecast& f, int i) {
  return isDark(f, i) && ((f.dark[i] >> 8) & 255) >= 0x20;
}

HourEval hourEval(const Forecast& f, int i) {
  HourEval h;
  if (i < 0 || i >= f.n) return h;
  int v[3] = {f.rdps[i], f.ecmwf[i], f.hrrr[i]};
  int cnt = 0, sum = 0, mn = 10, mx = 0;
  for (int k = 0; k < 3; k++) if (v[k] >= 0) { cnt++; sum += v[k]; mn = min(mn, v[k]); mx = max(mx, v[k]); }
  if (!cnt) return h;
  h.ok = true;
  h.mean = (float)sum / cnt;
  h.cover = (int)lroundf(100 - h.mean * 10);
  h.spread = mx - mn;
  h.t = max(0, (int)f.trans[i]);
  h.q = (h.cover <= cfg.goodMaxCloud && h.t >= cfg.goodMinTrans) ? 2
      : (h.cover <= FAIR_MAX_CLOUD && h.t >= FAIR_MIN_TRANS) ? 1 : 0;
  return h;
}

int findNights(const Forecast& f, Night* out, int maxN) {
  int cnt = 0, s = -1;
  for (int i = 0; i <= f.n && cnt < maxN; i++) {
    bool d = i < f.n && isDark(f, i);
    if (d && s < 0) s = i;
    if (!d && s >= 0) { out[cnt++] = {s, i - 1, i == f.n || s == 0}; s = -1; }
  }
  return cnt;
}

static void fmtHour(char* buf, size_t sz, int h) { snprintf(buf, sz, "%02d", (h % 24 + 24) % 24); }

Verdict rateNight(const Forecast& f, const Night* n) {
  Verdict v;
  if (!n) { v.word = W_NONE; strcpy(v.l1, "Beyond forecast"); return v; }
  v.hasNight = true;
  int best = 0, bestS = n->s, run = 0, runS = n->s, good = 0, fair = 0, agree = 0, tot = 0;
  v.hum = 0; v.wind = 5;
  for (int i = n->s; i <= n->e; i++) {
    HourEval h = hourEval(f, i); if (!h.ok) continue; tot++;
    if (h.spread <= AGREE_MAX_SPREAD) agree++;
    if (h.q == 2) { good++; if (run == 0) runS = i; run++; if (run > best) { best = run; bestS = runS; } } else run = 0;
    if (h.q == 1) fair++;
    if (f.hum[i] >= 0) v.hum = max(v.hum, (int)f.hum[i]);
    if (f.wind[i] >= 0) v.wind = min(v.wind, (int)f.wind[i]);
  }
  for (int i = n->s; i <= n->e; i++) if (isMoonlit(f, i)) {
    if (v.moonFrom < 0) v.moonFrom = i;
    v.moonTo = i; v.moonHours++;
  }
  v.word = best >= cfg.goMinHours ? W_GO : (best >= MAYBE_MIN_RUN || good + fair >= MAYBE_MIN_TOTAL) ? W_MAYBE : W_NO;
  char a[4], b[4];
  if (best >= MAYBE_MIN_RUN) {
    fmtHour(a, sizeof a, localHour(f, bestS)); fmtHour(b, sizeof b, localHour(f, bestS + best));
    snprintf(v.l1, sizeof v.l1, "Clear %s–%s  (%dh)", a, b, best);
  } else {
    fmtHour(a, sizeof a, localHour(f, n->s)); fmtHour(b, sizeof b, localHour(f, n->e + 1));
    snprintf(v.l1, sizeof v.l1, "Cloudy %s–%s", a, b);
  }
  if (n->partial) strcpy(v.l2, "Partial forecast");
  else strcpy(v.l2, (tot && (float)agree / tot >= AGREE_MIN_SHARE) ? "Models agree" : "Models disagree");
  return v;
}

static const char* TRANS_NAME[] = {"too cloudy", "poor", "below avg", "average", "above avg", "transparent"};

Verdict rateNow(const Forecast& f, int i, const Night* nights, int nn) {
  Verdict v;
  if (i < 0 || i >= f.n) { v.word = W_NONE; strcpy(v.l1, "Outside chart"); return v; }
  HourEval h = hourEval(f, i);
  if (isDark(f, i)) {
    if (!h.ok) { v.word = W_NONE; strcpy(v.l1, "No data"); return v; }
    v.word = h.q == 2 ? W_CLEAR : h.q == 1 ? W_FAIR : W_NO;
    snprintf(v.l1, sizeof v.l1, "%d%% cloud", h.cover);
    if (h.t == 0) strcpy(v.l2, "Too cloudy");
    else snprintf(v.l2, sizeof v.l2, "Transp. %s", TRANS_NAME[h.t]);
    return v;
  }
  v.word = W_DAY;
  const Night* next = nullptr;
  for (int k = 0; k < nn; k++) if (nights[k].s > i) { next = &nights[k]; break; }
  if (next) snprintf(v.l1, sizeof v.l1, "Dark at %02d:00", localHour(f, next->s));
  else strcpy(v.l1, "Daylight");
  if (h.ok) snprintf(v.l2, sizeof v.l2, "%d%% cloud now", h.cover);
  return v;
}
