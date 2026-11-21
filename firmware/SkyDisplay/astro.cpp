#include "astro.h"
#include <math.h>

static const double DEG = M_PI / 180.0;
static double jd(double t) { return t / 86400.0 + 2440587.5; }
static double norm360(double a) { a = fmod(a, 360.0); return a < 0 ? a + 360.0 : a; }

static void sunRaDec(double J, double& ra, double& dec, double& lamDeg) {
  double n = J - 2451545.0;
  double L = norm360(280.460 + 0.9856474 * n);
  double g = norm360(357.528 + 0.9856003 * n) * DEG;
  double lam = (L + 1.915 * sin(g) + 0.020 * sin(2 * g)) * DEG;
  double eps = (23.439 - 0.0000004 * n) * DEG;
  ra = atan2(cos(eps) * sin(lam), cos(lam));
  dec = asin(sin(eps) * sin(lam));
  lamDeg = norm360(lam / DEG);
}

static void moonRaDec(double J, double& ra, double& dec, double& lamDeg, double& parDeg) {
  double T = (J - 2451545.0) / 36525.0;
  auto s = [T](double a, double b) { return sin((a + b * T) * DEG); };
  auto c = [T](double a, double b) { return cos((a + b * T) * DEG); };
  double lam = 218.32 + 481267.881 * T + 6.29 * s(135.0, 477198.87) - 1.27 * s(259.3, -413335.36) + 0.66 * s(235.7, 890534.22)
             + 0.21 * s(269.9, 954397.74) - 0.19 * s(357.5, 35999.05) - 0.11 * s(186.5, 966404.03);
  double bet = 5.13 * s(93.3, 483202.02) + 0.28 * s(228.2, 960400.89) - 0.28 * s(318.3, 6003.15) - 0.17 * s(217.6, -407332.21);
  parDeg = 0.9508 + 0.0518 * c(135.0, 477198.87) + 0.0095 * c(259.3, -413335.36) + 0.0078 * c(235.7, 890534.22)
         + 0.0028 * c(269.9, 954397.74);
  double eps = (23.439 - 0.0000004 * (J - 2451545.0)) * DEG;
  double l = lam * DEG, b = bet * DEG;
  double x = cos(b) * cos(l);
  double y = cos(eps) * cos(b) * sin(l) - sin(eps) * sin(b);
  double z = sin(eps) * cos(b) * sin(l) + cos(eps) * sin(b);
  ra = atan2(y, x); dec = asin(z); lamDeg = norm360(lam);
}

static double altitude(double ra, double dec, double J, double lat, double lon) {
  double gmst = norm360(280.46061837 + 360.98564736629 * (J - 2451545.0));
  double H = (gmst + lon) * DEG - ra;
  return asin(sin(lat * DEG) * sin(dec) + cos(lat * DEG) * cos(dec) * cos(H)) / DEG;
}

struct Ctx { double lat, lon; };
static double sunAlt(double t, const Ctx& c) { double ra, dec, l; sunRaDec(jd(t), ra, dec, l); return altitude(ra, dec, jd(t), c.lat, c.lon); }
static double moonAlt(double t, const Ctx& c) {  // topocentric
  double ra, dec, l, par; moonRaDec(jd(t), ra, dec, l, par);
  double a = altitude(ra, dec, jd(t), c.lat, c.lon);
  return a - par * cos(a * DEG);
}

// Next time f crosses target (rising or setting) within 36 h, by 10-minute scan + bisection.
static time_t nextCross(double (*f)(double, const Ctx&), const Ctx& c, double t0, double target, bool rising) {
  const double step = 600, end = t0 + 36 * 3600.0;
  double t = t0, prev = f(t0, c) - target;
  while (t < end) {
    double t2 = t + step, cur = f(t2, c) - target;
    if ((rising && prev < 0 && cur >= 0) || (!rising && prev >= 0 && cur < 0)) {
      double a = t, b = t2;
      for (int i = 0; i < 16; i++) {
        double m = (a + b) / 2, v = f(m, c) - target;
        if ((v >= 0) == rising) b = m; else a = m;
      }
      return (time_t)((a + b) / 2);
    }
    t = t2; prev = cur;
  }
  return 0;
}

SkyEvents computeSkyEvents(time_t from, double lat, double lon) {
  SkyEvents e;
  Ctx c{lat, lon};
  double t0 = (double)from;
  e.sunset    = nextCross(sunAlt, c, t0, -0.833, false);
  e.sunrise   = nextCross(sunAlt, c, t0, -0.833, true);
  e.duskAstro = nextCross(sunAlt, c, t0, -18.0, false);
  e.dawnAstro = nextCross(sunAlt, c, t0, -18.0, true);
  e.moonrise  = nextCross(moonAlt, c, t0, -0.833, true);
  e.moonset   = nextCross(moonAlt, c, t0, -0.833, false);
  double ra, dec, sl, ml, par;
  sunRaDec(jd(t0), ra, dec, sl);
  moonRaDec(jd(t0), ra, dec, ml, par);
  double elong = norm360(ml - sl);
  e.moonIllum = (float)((1 - cos(elong * DEG)) / 2);
  e.moonWaxing = elong < 180;
  return e;
}

const char* moonPhaseName(float illum, bool waxing) {
  if (illum < 0.03f) return "New moon";
  if (illum > 0.97f) return "Full moon";
  if (illum >= 0.45f && illum <= 0.55f) return waxing ? "First quarter" : "Last quarter";
  if (illum < 0.5f) return waxing ? "Waxing crescent" : "Waning crescent";
  return waxing ? "Waxing gibbous" : "Waning gibbous";
}
