/* roots.h -- the root certificates compiled into Kallisti (core/roots_bundled.c, generated), and loading a
 * trust store from the files on disk. */
#ifndef KALLISTI_ROOTS_H
#define KALLISTI_ROOTS_H

#include "chain.h"

/* Adds the compiled-in roots; returns how many. */
int roots_bundled_load(TrustStore *ts);

/* Adds every certificate found under `dir` (an OpenSSL-style directory): the bundle files cert.pem,
 * certs.pem, ca-bundle.crt and curl-ca-bundle.crt in it and in its certs/ subdirectory, and the individual
 * .pem, .crt and .cer files in certs/.  Returns how many were added (0 if there is no such directory). */
int roots_load_dir(TrustStore *ts, const char *dir);

#define ROOTS_SYSTEM_DIR "/usr/local/ssl"

enum { ROOTS_FROM_NONE = 0, ROOTS_FROM_DIR, ROOTS_FROM_BUNDLED };

/* The standard policy: use the certificates in `dir` (normally ROOTS_SYSTEM_DIR) if it holds any, else the
 * compiled-in set.  Returns where the roots came from; *count gets how many were loaded. */
int roots_load_default(TrustStore *ts, const char *dir, int *count);

#endif
