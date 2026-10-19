/* inflate.h -- zlib/DEFLATE decompression (RFC 1950/1951), enough for PNG: stored, fixed-Huffman and
 * dynamic-Huffman blocks, with the zlib header and Adler-32 checked.  The caller says how large the result
 * must be; anything else is an error, which also bounds memory. */
#ifndef KALLISTI_INFLATE_H
#define KALLISTI_INFLATE_H

#include <stddef.h>

/* Decompresses the zlib stream in[0..n) into out[0..outlen), which must come out to exactly outlen bytes.
 * Returns 0, or -1 with *err set to a short reason. */
int inflate_zlib(const unsigned char *in, size_t n, unsigned char *out, size_t outlen, const char **err);

unsigned long adler32_sum(const unsigned char *p, size_t n);
unsigned long crc32_sum(unsigned long crc, const unsigned char *p, size_t n);

#endif
