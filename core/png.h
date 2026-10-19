/* png.h -- a PNG decoder: every colour type and bit depth, palettes and tRNS transparency, Adam7 interlacing,
 * chunk CRCs and the zlib checksum checked.  The result is 8-bit RGBA.  Gamma, colour profiles and animation
 * are ignored.  Pictures larger than PNG_MAX_PIXELS are refused. */
#ifndef KALLISTI_PNG_H
#define KALLISTI_PNG_H

#include <stddef.h>

#define PNG_MAX_PIXELS (6UL * 1024UL * 1024UL)
#define PNG_MAX_SIDE 16384UL

typedef struct {
    unsigned long width, height;
    unsigned char *rgba;         /* width * height * 4 bytes, malloc'd; free() it */
} PngImage;

/* Returns 0 and fills *img, or -1 with *err set to a short reason (and img->rgba NULL). */
int png_decode(const unsigned char *data, size_t len, PngImage *img, const char **err);

#endif
