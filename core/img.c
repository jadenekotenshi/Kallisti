/* PNG to TIFF for display: see img.h. */
#include <stdlib.h>
#include <string.h>
#include "img.h"
#include "png.h"
#include "jpeg.h"
#include "gif.h"

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* one directory entry: tag, type (3 SHORT, 4 LONG, 5 RATIONAL), count, and the value or its offset */
static unsigned char *entry(unsigned char *p, unsigned tag, unsigned type, unsigned long count, unsigned long value)
{
    put16(p, tag);
    put16(p + 2, type);
    put32(p + 4, count);
    if (type == 3 && count == 1) {
        put16(p + 8, (unsigned)value);                   /* a lone SHORT sits in the first half of the field */
        put16(p + 10, 0);
    } else {
        put32(p + 8, value);
    }
    return p + 12;
}

unsigned char *tiff_encode_rgb(const unsigned char *rgb, unsigned long width, unsigned long height, size_t *len)
{
    enum { NENT = 13, IFD = 8, EXTRA = IFD + 2 + NENT * 12 + 4, DATA = EXTRA + 6 + 8 + 8 };
    size_t pixels;
    unsigned char *out, *p;

    if (width == 0 || height == 0 || width > 0xffffUL * 4 || height > 0xffffUL * 4 || width * height > 0x3fffffffUL / 3) {
        return NULL;
    }
    pixels = (size_t)(width * height * 3UL);
    out = (unsigned char *)malloc(DATA + pixels);
    if (out == NULL) {
        return NULL;
    }
    memset(out, 0, DATA);
    out[0] = 'M';
    out[1] = 'M';
    put16(out + 2, 42);
    put32(out + 4, IFD);
    put16(out + IFD, NENT);
    p = out + IFD + 2;
    p = entry(p, 256, 4, 1, width);
    p = entry(p, 257, 4, 1, height);
    p = entry(p, 258, 3, 3, EXTRA);                       /* BitsPerSample: 8,8,8 at EXTRA */
    p = entry(p, 259, 3, 1, 1);                           /* no compression */
    p = entry(p, 262, 3, 1, 2);                           /* RGB */
    p = entry(p, 273, 4, 1, DATA);                        /* StripOffsets */
    p = entry(p, 277, 3, 1, 3);
    p = entry(p, 278, 4, 1, height);                      /* RowsPerStrip: all of it */
    p = entry(p, 279, 4, 1, pixels);
    p = entry(p, 282, 5, 1, EXTRA + 6);                   /* XResolution 72/1 */
    p = entry(p, 283, 5, 1, EXTRA + 14);
    p = entry(p, 284, 3, 1, 1);                           /* chunky */
    p = entry(p, 296, 3, 1, 2);                           /* inches */
    put32(p, 0);                                          /* no next directory */
    put16(out + EXTRA, 8);
    put16(out + EXTRA + 2, 8);
    put16(out + EXTRA + 4, 8);
    put32(out + EXTRA + 6, 72);
    put32(out + EXTRA + 10, 1);
    put32(out + EXTRA + 14, 72);
    put32(out + EXTRA + 18, 1);
    memcpy(out + DATA, rgb, pixels);
    *len = DATA + pixels;
    return out;
}

/* Lays RGBA over white, shrinks it by a whole factor until it fits the box, and encodes the TIFF. Does not free rgba. */
static int rgba_to_tiff(const unsigned char *rgba, unsigned long width, unsigned long height, unsigned long max_w,
                        unsigned long max_h, unsigned char **tiff, size_t *tiff_len, unsigned long *w, unsigned long *h,
                        const char **err)
{
    unsigned long k = 1, nw, nh, x, y, i, j;
    unsigned char *rgb;

    *tiff = NULL;
    if (max_w > 0 && width > max_w) {
        k = (width + max_w - 1) / max_w;
    }
    if (max_h > 0 && height > max_h && (height + max_h - 1) / max_h > k) {
        k = (height + max_h - 1) / max_h;
    }
    nw = (width + k - 1) / k;
    nh = (height + k - 1) / k;
    rgb = (unsigned char *)malloc((size_t)(nw * nh * 3UL));
    if (rgb == NULL) {
        *err = "out of memory";
        return -1;
    }
    if (k == 1) {                                         /* no shrinking: lay each pixel over white directly */
        size_t total = (size_t)(width * height), t;

        for (t = 0; t < total; t++) {
            const unsigned char *s = rgba + t * 4;
            unsigned long a = s[3];

            if (a == 255) {
                rgb[t * 3] = s[0];
                rgb[t * 3 + 1] = s[1];
                rgb[t * 3 + 2] = s[2];
            } else {
                rgb[t * 3] = (unsigned char)((s[0] * a + 255UL * (255UL - a)) / 255UL);
                rgb[t * 3 + 1] = (unsigned char)((s[1] * a + 255UL * (255UL - a)) / 255UL);
                rgb[t * 3 + 2] = (unsigned char)((s[2] * a + 255UL * (255UL - a)) / 255UL);
            }
        }
    } else {
        for (y = 0; y < nh; y++) {
            for (x = 0; x < nw; x++) {
                unsigned long sum[3] = { 0, 0, 0 }, count = 0;

                for (j = y * k; j < (y + 1) * k && j < height; j++) {
                    for (i = x * k; i < (x + 1) * k && i < width; i++) {
                        const unsigned char *s = rgba + (j * width + i) * 4;
                        unsigned long a = s[3];

                        sum[0] += (s[0] * a + 255UL * (255UL - a)) / 255UL;     /* over white */
                        sum[1] += (s[1] * a + 255UL * (255UL - a)) / 255UL;
                        sum[2] += (s[2] * a + 255UL * (255UL - a)) / 255UL;
                        count++;
                    }
                }
                rgb[(y * nw + x) * 3] = (unsigned char)(sum[0] / count);
                rgb[(y * nw + x) * 3 + 1] = (unsigned char)(sum[1] / count);
                rgb[(y * nw + x) * 3 + 2] = (unsigned char)(sum[2] / count);
            }
        }
    }
    *tiff = tiff_encode_rgb(rgb, nw, nh, tiff_len);
    free(rgb);
    if (*tiff == NULL) {
        *err = "out of memory";
        return -1;
    }
    *w = nw;
    *h = nh;
    *err = "ok";
    return 0;
}

int img_to_tiff(const unsigned char *data, size_t n, unsigned long max_w, unsigned long max_h,
                unsigned char **tiff, size_t *tiff_len, unsigned long *w, unsigned long *h, const char **err)
{
    PngImage im;
    int rc;

    *tiff = NULL;
    if (n >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        if (png_decode(data, n, &im, err) != 0) {
            return -1;
        }
    } else if (n >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) {
        if (jpeg_decode(data, n, &im, err) != 0) {
            return -1;
        }
    } else if (n >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0)) {
        if (gif_decode(data, n, &im, NULL, err) != 0) {
            return -1;
        }
    } else {
        *err = "the server did not send a PNG, JPEG or GIF picture";
        return -1;
    }
    rc = rgba_to_tiff(im.rgba, im.width, im.height, max_w, max_h, tiff, tiff_len, w, h, err);
    free(im.rgba);
    return rc;
}

void img_anim_free(ImgAnim *a)
{
    int i;

    for (i = 0; i < a->n; i++) {
        free(a->tiff[i]);
    }
    free(a->tiff);
    free(a->len);
    free(a->delay_ms);
    memset(a, 0, sizeof(*a));
}

int img_gif_to_anim(const unsigned char *data, size_t n, unsigned long max_w, unsigned long max_h, int max_frames,
                    unsigned long max_pixels, ImgAnim *out, const char **err)
{
    GifAnim g;
    int i;

    memset(out, 0, sizeof(*out));
    if (n < 6 || (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0)) {
        *err = "not a GIF picture";
        return -1;
    }
    if (gif_decode_anim(data, n, max_frames, max_pixels, &g, err) != 0) {
        return -1;
    }
    out->tiff = (unsigned char **)calloc((size_t)g.nframes, sizeof(unsigned char *));
    out->len = (size_t *)calloc((size_t)g.nframes, sizeof(size_t));
    out->delay_ms = (unsigned *)calloc((size_t)g.nframes, sizeof(unsigned));
    if (out->tiff == NULL || out->len == NULL || out->delay_ms == NULL) {
        gif_anim_free(&g);
        img_anim_free(out);
        *err = "out of memory";
        return -1;
    }
    for (i = 0; i < g.nframes; i++) {
        unsigned long w, h;

        if (rgba_to_tiff(g.rgba[i], g.width, g.height, max_w, max_h, &out->tiff[i], &out->len[i], &w, &h, err) != 0) {
            gif_anim_free(&g);
            img_anim_free(out);
            return -1;
        }
        out->delay_ms[i] = g.delay_ms[i];
        out->w = w;
        out->h = h;
        out->n = i + 1;
    }
    out->total_frames = g.total_frames;
    gif_anim_free(&g);
    *err = "ok";
    return 0;
}
