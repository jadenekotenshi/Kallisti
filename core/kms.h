/* kms.h -- the millisecond clock type shared by the sans-I/O state machines (gateway, REST). */
#ifndef KALLISTI_KMS_H
#define KALLISTI_KMS_H

/* Milliseconds from any monotonic clock, 32 bits on every platform (OPENSTEP's long is 32 bits, the host's
 * is not): differences are taken modulo 2^32, so a wrapping clock is fine. */
typedef unsigned int KMs;

/* later - earlier as a signed number of milliseconds, correct across wraparound. */
#define KMS_SINCE(later, earlier) ((long)(int)((KMs)(later) - (KMs)(earlier)))

#endif
