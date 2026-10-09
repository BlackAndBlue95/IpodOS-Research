#ifndef SUN_H
#define SUN_H
/* local sunrise and sunset in minutes after midnight for the date, lat/lon in degrees (east
   positive), tz_min = local offset from UTC in minutes including DST. -1: no rise/set today. */
int sun_times(int year, int month, int day, double lat, double lon, int tz_min, int *rise, int *set);
#endif
