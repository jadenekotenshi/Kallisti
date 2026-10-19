/* ISO 8601 timestamps to local time: see ktime.h. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "ktime.h"

static int digits(const char *s, int n, int *out)
{
    int i, v = 0;

    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 1;
}

/* days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's algorithm) */
static long days_from_civil(int y, int m, int d)
{
    long era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + doe - 719468L;
}

int ktime_parse(const char *iso, unsigned long *secs)
{
    int y, mo, d, h, mi, s, off_h = 0, off_m = 0, sign = 1;
    const char *p;
    long days;
    unsigned long base, off;

    if (iso == NULL || strlen(iso) < 19) {
        return -1;
    }
    if (!digits(iso, 4, &y) || iso[4] != '-' || !digits(iso + 5, 2, &mo) || iso[7] != '-' ||
        !digits(iso + 8, 2, &d) || (iso[10] != 'T' && iso[10] != ' ') || !digits(iso + 11, 2, &h) ||
        iso[13] != ':' || !digits(iso + 14, 2, &mi) || iso[16] != ':' || !digits(iso + 17, 2, &s)) {
        return -1;
    }
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) {
        return -1;
    }
    p = iso + 19;
    if (*p == '.') {                                     /* fractional seconds */
        p++;
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }
    if (*p == 'Z' || *p == '\0') {
        ;
    } else if (*p == '+' || *p == '-') {
        sign = *p == '-' ? -1 : 1;
        if (!digits(p + 1, 2, &off_h) || p[3] != ':' || !digits(p + 4, 2, &off_m)) {
            return -1;
        }
    } else {
        return -1;
    }
    days = days_from_civil(y, mo, d);
    if (days > 49709L) {                                 /* past 2106: does not fit 32 unsigned bits */
        return -1;
    }
    /* unsigned throughout: OPENSTEP's long is 32 bits, and dates after 2038 overflow a signed one */
    base = (unsigned long)days * 86400UL + (unsigned long)h * 3600UL + (unsigned long)mi * 60UL + (unsigned long)s;
    off = (unsigned long)off_h * 3600UL + (unsigned long)off_m * 60UL;
    if (sign > 0) {
        if (off > base) {
            return -1;
        }
        base -= off;
    } else {
        if (base + off < base) {
            return -1;
        }
        base += off;
    }
    *secs = base;
    return 0;
}

int ktime_local(const char *iso, char *hhmm, char *day)
{
    unsigned long secs;
    time_t t;
    struct tm *tm;

    hhmm[0] = day[0] = '\0';
    if (ktime_parse(iso, &secs) != 0) {
        strcpy(hhmm, "--:--");
        return -1;
    }
    if (secs > 0x7fffffffUL) {                           /* beyond a 32-bit time_t: read the text as it is */
        sprintf(hhmm, "%.2s:%.2s", iso + 11, iso + 14);
        sprintf(day, "%.10s", iso);
        return 0;
    }
    t = (time_t)secs;
    tm = localtime(&t);
    if (tm == NULL) {
        strcpy(hhmm, "--:--");
        return -1;
    }
    sprintf(hhmm, "%02d:%02d", tm->tm_hour, tm->tm_min);
    sprintf(day, "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return 0;
}
