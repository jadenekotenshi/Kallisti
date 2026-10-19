/* Base64 (RFC 4648, standard alphabet, padded): the WebSocket handshake key and accept value. */
#ifndef KALLISTI_BASE64_H
#define KALLISTI_BASE64_H

#include <stddef.h>

/* Writes the padded encoding of in[0..n) plus a NUL to out, which must hold 4*((n+2)/3)+1 bytes.
 * Returns the encoded length (without the NUL). */
size_t base64_encode(const unsigned char *in, size_t n, char *out);

/* Decodes a padded base64 string (no whitespace tolerated).  out must hold at least strlen(s)/4*3 bytes.
 * Returns 0 and sets *outlen, or -1 for anything malformed. */
int base64_decode(const char *s, unsigned char *out, size_t *outlen);

#endif
