#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../core/ktime.h"
#include "test.h"

int main(void)
{
    unsigned long s;
    char hm[8], day[16];

    CHECK(ktime_parse("1970-01-01T00:00:00+00:00", &s) == 0 && s == 0);
    CHECK(ktime_parse("2000-03-01T00:00:00Z", &s) == 0 && s == 951868800UL);
    CHECK(ktime_parse("2026-10-19T10:00:00.123000+00:00", &s) == 0 && s == 1792404000UL);
    CHECK(ktime_parse("2026-10-19T12:00:00+02:00", &s) == 0 && s == 1792404000UL);          /* offsets */
    CHECK(ktime_parse("2026-10-19T05:30:00-04:30", &s) == 0 && s == 1792404000UL);
    CHECK(ktime_parse("2024-02-29T23:59:59Z", &s) == 0 && s == 1709251199UL);               /* leap day */
    CHECK(ktime_parse("2038-01-19T03:14:07Z", &s) == 0 && s == 2147483647UL);
    CHECK(ktime_parse("2100-01-01T00:00:00Z", &s) == 0 && s > 0x7fffffffUL);
    CHECK(ktime_parse("2026-10-19 10:00:00Z", &s) == 0);
    CHECK(ktime_parse("", &s) == -1 && ktime_parse(NULL, &s) == -1 && ktime_parse("garbage", &s) == -1);
    CHECK(ktime_parse("2026-13-19T10:00:00Z", &s) == -1 && ktime_parse("2026-10-32T10:00:00Z", &s) == -1);
    CHECK(ktime_parse("2026-10-19T25:00:00Z", &s) == -1 && ktime_parse("1969-12-31T23:59:59Z", &s) == -1);
    CHECK(ktime_parse("2026-10-19T10:00:00+0200", &s) == -1 && ktime_parse("2026-10-19T10:00:00X", &s) == -1);

#ifndef OPENSTEP                                         /* no tzset() on OPENSTEP 4.2: the zone is whatever the machine has */
    putenv("TZ=UTC");
    tzset();
    CHECK(ktime_local("2026-10-19T10:05:00.5+00:00", hm, day) == 0 && strcmp(hm, "10:05") == 0 && strcmp(day, "2026-10-19") == 0);
    putenv("TZ=EST5EDT");
    tzset();
    CHECK(ktime_local("2026-10-19T10:05:00+00:00", hm, day) == 0 && strcmp(hm, "06:05") == 0 && strcmp(day, "2026-10-19") == 0);   /* EDT */
    CHECK(ktime_local("2026-12-25T02:30:00Z", hm, day) == 0 && strcmp(hm, "21:30") == 0 && strcmp(day, "2026-12-24") == 0);        /* EST, previous day */
    CHECK(ktime_local("2100-01-01T04:05:06Z", hm, day) == 0 && strcmp(hm, "04:05") == 0 && strcmp(day, "2100-01-01") == 0);      /* beyond 2038: as written */
#endif
    CHECK(ktime_local("nope", hm, day) == -1 && strcmp(hm, "--:--") == 0 && day[0] == '\0');
    TEST_DONE("ktime");
}
