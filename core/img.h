/* img.h -- getting a picture on screen without any decoder AppKit might not have: PNG, JPEG or GIF in (png.h, jpeg.h, gif.h), a flattened and
 * possibly shrunken RGB image, and an uncompressed TIFF out, which every OPENSTEP NSImage can read. */
#ifndef KALLISTI_IMG_H
#define KALLISTI_IMG_H

#include <stddef.h>

/* Encodes 8-bit RGB (width*height*3 bytes) as a big-endian, uncompressed, single-strip TIFF. Returns a malloc'd
 * buffer (length in *len) or NULL. */
unsigned char *tiff_encode_rgb(const unsigned char *rgb, unsigned long width, unsigned long height, size_t *len);

/* Decodes a PNG, a JPEG or a GIF (told apart by its first bytes; a GIF's first frame), lays transparent parts over white, shrinks it by a whole factor until it fits max_w x max_h
 * (0: no limit), and encodes a TIFF. *tiff is malloc'd; *w and *h are the resulting size. Returns 0 or -1 with
 * *err set. */
int img_to_tiff(const unsigned char *data, size_t n, unsigned long max_w, unsigned long max_h,
                    unsigned char **tiff, size_t *tiff_len, unsigned long *w, unsigned long *h, const char **err);

/* An animated GIF as one TIFF per frame (each flattened onto white and shrunk like img_to_tiff does), with how long
 * each is shown. */
typedef struct {
    int n;                       /* frames */
    unsigned char **tiff;        /* n TIFFs, each malloc'd */
    size_t *len;
    unsigned *delay_ms;
    unsigned long w, h;          /* the size of every frame */
    int total_frames;            /* frames in the file, which can be more than n */
} ImgAnim;

/* Decodes up to max_frames frames (within max_pixels pixels in all; at least one) of a GIF. Returns 0, or -1 with *err
 * set. Free with img_anim_free(). */
int img_gif_to_anim(const unsigned char *data, size_t n, unsigned long max_w, unsigned long max_h, int max_frames,
                    unsigned long max_pixels, ImgAnim *out, const char **err);
void img_anim_free(ImgAnim *a);

#endif
