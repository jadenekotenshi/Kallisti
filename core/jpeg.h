/* jpeg.h -- a JPEG decoder: baseline, extended-sequential and progressive Huffman JPEGs (8-bit, 1 or 3
 * components, any sampling factors, restart intervals), with libjpeg's "fancy" chroma upsampling and its
 * accurate integer IDCT, so the pixels match libjpeg's.  Arithmetic coding, 12-bit samples, lossless and CMYK
 * JPEGs are refused.  The result is 8-bit RGBA (opaque), the same as png.h's. */
#ifndef KALLISTI_JPEG_H
#define KALLISTI_JPEG_H

#include <stddef.h>
#include "png.h"

#define JPEG_MAX_PIXELS (6UL * 1024UL * 1024UL)

/* Returns 0 and fills *img (rgba malloc'd, free() it), or -1 with *err set (and img->rgba NULL). */
int jpeg_decode(const unsigned char *data, size_t len, PngImage *img, const char **err);

#endif
