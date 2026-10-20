/* gif.h -- a GIF decoder: GIF87a and GIF89a, global and local colour tables, interlacing and a transparent colour.
 * It decodes the FIRST frame (what a still picture of an animation should show) onto the logical screen; the rest of
 * the animation is only counted. The result is 8-bit RGBA (transparent where the picture is), the same as png.h's. */
#ifndef KALLISTI_GIF_H
#define KALLISTI_GIF_H

#include <stddef.h>
#include "png.h"

#define GIF_MAX_PIXELS (6UL * 1024UL * 1024UL)

/* Returns 0 and fills *img (rgba malloc'd, free() it), or -1 with *err set (and img->rgba NULL). If frames is not
 * NULL it receives the number of frames in the file (counting stops at 1000). */
int gif_decode(const unsigned char *data, size_t len, PngImage *img, int *frames, const char **err);

#endif
