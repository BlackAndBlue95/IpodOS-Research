/* Checks sun.c against published sunrise/sunset times (NOAA calculator, to the minute). */
#include <stdio.h>
#include <stdlib.h>
#include "../sun.c"

struct ref { const char *city; double lat, lon; int tz, y, m, d, rise, set; };
static const struct ref refs[] = {
    { "London 2026-06-21 BST", 51.5074, -0.1278, 60, 2026, 6, 21, 4 * 60 + 43, 21 * 60 + 21 },
    { "London 2026-12-21 GMT", 51.5074, -0.1278, 0, 2026, 12, 21, 8 * 60 + 4, 15 * 60 + 53 },
    { "New York 2026-12-21 EST", 40.7128, -74.0060, -300, 2026, 12, 21, 7 * 60 + 16, 16 * 60 + 32 },
    { "New York 2026-06-21 EDT", 40.7128, -74.0060, -240, 2026, 6, 21, 5 * 60 + 25, 20 * 60 + 31 },
    { "Sydney 2026-06-21 AEST", -33.8688, 151.2093, 600, 2026, 6, 21, 7 * 60 + 0, 16 * 60 + 54 },
    { "Tokyo 2026-03-20 JST", 35.6762, 139.6503, 540, 2026, 3, 20, 5 * 60 + 45, 17 * 60 + 53 },
    { "Los Angeles 2026-10-07 PDT", 34.0522, -118.2437, -420, 2026, 10, 7, 6 * 60 + 53, 18 * 60 + 29 },
};

int main(void)
{
    int bad = 0;
    for (unsigned i = 0; i < sizeof refs / sizeof refs[0]; i++) {
        const struct ref *r = &refs[i];
        int rise, set, rc = sun_times(r->y, r->m, r->d, r->lat, r->lon, r->tz, &rise, &set);
        int dr = abs(rise - r->rise), ds = abs(set - r->set), ok = !rc && dr <= 4 && ds <= 4;
        printf("  %s %-28s rise %02d:%02d (ref %02d:%02d) set %02d:%02d (ref %02d:%02d)\n", ok ? "ok  " : "FAIL", r->city,
               rise / 60, rise % 60, r->rise / 60, r->rise % 60, set / 60, set % 60, r->set / 60, r->set % 60);
        bad += !ok;
    }
    printf(bad ? "%d FAILED\n" : "ALL OK\n", bad);
    return bad != 0;
}
