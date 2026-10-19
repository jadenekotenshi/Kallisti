/* ktime.h -- turning Discord's ISO 8601 timestamps ("2026-10-19T10:00:00.123000+00:00") into the local time
 * to show next to a message. */
#ifndef KALLISTI_KTIME_H
#define KALLISTI_KTIME_H

#include <stddef.h>

/* Fills hhmm ("HH:MM", at least 6 bytes) and day ("YYYY-MM-DD", at least 11 bytes) in the machine's local time
 * zone. Returns 0, or -1 for text that is not a timestamp (hhmm becomes "--:--", day ""). Times beyond the
 * year 2037, which a 32-bit time_t cannot hold, are shown in UTC. */
int ktime_local(const char *iso, char *hhmm, char *day);

/* Seconds since 1970-01-01 UTC as an unsigned 32-bit-safe number, or -1 on failure; *secs gets it. */
int ktime_parse(const char *iso, unsigned long *secs);

#endif
