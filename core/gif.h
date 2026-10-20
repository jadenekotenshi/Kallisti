/* gif.h -- a GIF decoder: GIF87a and GIF89a, global and local colour tables, interlacing and a transparent colour.
 * It decodes the FIRST frame (what a still picture of an animation should show) onto the logical screen; the rest of
 * the animation is only counted. The result is 8-bit RGBA (transparent where the picture is), the same as png.h's. */
#ifndef KALLISTI_GIF_H
#define KALLISTI_GIF_H

#include <stddef.h>
#include "png.h"

#define GIF_MAX_PIXELS (6UL * 1024UL * 1024UL)

/* An animation: every frame composited onto the logical screen (so each is a complete picture, RGBA, transparent where
 * nothing has been drawn), with the three disposal methods applied as browsers do (2: the frame's area goes
 * transparent, 3: the screen goes back to how it was before the frame). */
typedef struct {
    unsigned long width, height;
    int nframes;
    unsigned char **rgba;        /* nframes pictures of width * height * 4 bytes, each malloc'd */
    unsigned *delay_ms;          /* how long each frame is shown (browsers' rule: 0 or 10 ms means 100 ms) */
    int total_frames;            /* frames in the file (counting stops at 1000); more than nframes if cut short */
} GifAnim;

/* Decodes up to max_frames frames, stopping early when the frames would together exceed max_pixels pixels (at least
 * the first frame is always decoded). A damaged later frame ends the animation there; a damaged first frame is an
 * error. Returns 0 and fills *a, or -1 with *err set. Free with gif_anim_free(). */
int gif_decode_anim(const unsigned char *data, size_t len, int max_frames, unsigned long max_pixels, GifAnim *a,
                    const char **err);
void gif_anim_free(GifAnim *a);

/* Returns 0 and fills *img (rgba malloc'd, free() it), or -1 with *err set (and img->rgba NULL). If frames is not
 * NULL it receives the number of frames in the file (counting stops at 1000). */
int gif_decode(const unsigned char *data, size_t len, PngImage *img, int *frames, const char **err);

#endif
