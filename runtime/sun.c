/* Sunrise and sunset from NOAA's solar calculator equations. Uses its own trig, since region E
 * has no libm (libgcc supplies the soft-float arithmetic). Accurate to about a minute. */
#include "sun.h"

#define PI 3.14159265358979323846
#define RAD(d) ((d) * PI / 180.0)
#define DEG(r) ((r) * 180.0 / PI)

static double s_fmod(double a, double m) { double q = a / m; long i = (long)q; if (q < 0 && i != q) i--; return a - m * i; }
static double s_sqrt(double x)
{
    double r = x > 1 ? x : 1, i;
    if (x <= 0) return 0;
    for (i = 0; i < 40; i++) r = 0.5 * (r + x / r);
    return r;
}
static double s_sin(double x)
{
    double x2, t, s;
    int i;
    x = s_fmod(x + PI, 2 * PI) - PI;              /* -pi..pi */
    x2 = x * x; t = x; s = x;
    for (i = 1; i < 10; i++) { t *= -x2 / ((2 * i) * (2 * i + 1)); s += t; }
    return s;
}
static double s_cos(double x) { return s_sin(x + PI / 2); }
static double s_tan(double x) { return s_sin(x) / s_cos(x); }
static double s_atan(double x)
{
    double x2, t, s, r;
    int i, inv = 0, half = 0;
    if (x < 0) return -s_atan(-x);
    if (x > 1) { x = 1 / x; inv = 1; }
    if (x > 0.5) { x = (s_sqrt(1 + x * x) - 1) / x; half = 1; }   /* atan(x) = 2 atan(...) */
    x2 = x * x; t = x; s = x;
    for (i = 1; i < 30; i++) { t *= -x2; s += t / (2 * i + 1); }
    r = half ? 2 * s : s;
    return inv ? PI / 2 - r : r;
}
static double s_atan2(double y, double x)
{
    if (x > 0) return s_atan(y / x);
    if (x < 0) return y >= 0 ? s_atan(y / x) + PI : s_atan(y / x) - PI;
    return y > 0 ? PI / 2 : y < 0 ? -PI / 2 : 0;
}
static double s_asin(double x) { if (x >= 1) return PI / 2; if (x <= -1) return -PI / 2; return s_atan2(x, s_sqrt(1 - x * x)); }
static double s_acos(double x) { return PI / 2 - s_asin(x); }

static double julian_day(int y, int m, int d)
{
    int a, b;
    if (m <= 2) { y--; m += 12; }
    a = y / 100; b = 2 - a + a / 4;
    return (long)(365.25 * (y + 4716)) + (long)(30.6001 * (m + 1)) + d + b - 1524.5;
}

int sun_times(int year, int month, int day, double lat, double lon, int tz_min, int *rise, int *set)
{
    double jd = julian_day(year, month, day), t = (jd - 2451545.0) / 36525.0;
    double L0 = s_fmod(280.46646 + t * (36000.76983 + 0.0003032 * t), 360.0);
    double M = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    double C = s_sin(RAD(M)) * (1.914602 - t * (0.004817 + 0.000014 * t)) + s_sin(RAD(2 * M)) * (0.019993 - 0.000101 * t) + s_sin(RAD(3 * M)) * 0.000289;
    double tl = L0 + C, omega = 125.04 - 1934.136 * t;
    double lambda = tl - 0.00569 - 0.00478 * s_sin(RAD(omega));
    double eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    double eps = eps0 + 0.00256 * s_cos(RAD(omega));
    double decl = s_asin(s_sin(RAD(eps)) * s_sin(RAD(lambda)));
    double y = s_tan(RAD(eps / 2)); y *= y;
    double eqt = 4.0 * DEG(y * s_sin(2 * RAD(L0)) - 2 * e * s_sin(RAD(M)) + 4 * e * y * s_sin(RAD(M)) * s_cos(2 * RAD(L0))
                           - 0.5 * y * y * s_sin(4 * RAD(L0)) - 1.25 * e * e * s_sin(2 * RAD(M)));
    double cosha = s_cos(RAD(90.833)) / (s_cos(RAD(lat)) * s_cos(decl)) - s_tan(RAD(lat)) * s_tan(decl);
    double ha, r, s;
    if (cosha > 1 || cosha < -1) return -1;        /* polar day or night */
    ha = DEG(s_acos(cosha));
    r = 720 - 4 * (lon + ha) - eqt + tz_min;
    s = 720 - 4 * (lon - ha) - eqt + tz_min;
    *rise = (int)(r + 0.5); *set = (int)(s + 0.5);
    while (*rise < 0) *rise += 1440; while (*rise >= 1440) *rise -= 1440;
    while (*set < 0) *set += 1440; while (*set >= 1440) *set -= 1440;
    return 0;
}
