/* img.h -- getting a picture on screen without any decoder AppKit might not have: PNG in (png.h), a flattened and
 * possibly shrunken RGB image, and an uncompressed TIFF out, which every OPENSTEP NSImage can read. */
#ifndef KALLISTI_IMG_H
#define KALLISTI_IMG_H

#include <stddef.h>

/* Encodes 8-bit RGB (width*height*3 bytes) as a big-endian, uncompressed, single-strip TIFF. Returns a malloc'd
 * buffer (length in *len) or NULL. */
unsigned char *tiff_encode_rgb(const unsigned char *rgb, unsigned long width, unsigned long height, size_t *len);

/* Decodes a PNG, lays transparent parts over white, shrinks it by a whole factor until it fits max_w x max_h
 * (0: no limit), and encodes a TIFF. *tiff is malloc'd; *w and *h are the resulting size. Returns 0 or -1 with
 * *err set. */
int img_png_to_tiff(const unsigned char *png, size_t n, unsigned long max_w, unsigned long max_h,
                    unsigned char **tiff, size_t *tiff_len, unsigned long *w, unsigned long *h, const char **err);

#endif
