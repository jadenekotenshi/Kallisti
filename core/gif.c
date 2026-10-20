/* GIF decoding: see gif.h. */
#include <stdlib.h>
#include <string.h>
#include "gif.h"

#define MAX_FRAMES_COUNTED 1000

static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

/* Gathers the data sub-blocks starting at d[*pos] into one malloc'd buffer; advances *pos past the terminator.
 * Returns NULL (and sets *err) if the blocks run off the end. */
static unsigned char *join_blocks(const unsigned char *d, size_t n, size_t *pos, size_t *outlen, const char **err)
{
    size_t p = *pos, total = 0, q;
    unsigned char *buf, *o;

    for (q = p; ; ) {
        size_t len;

        if (q >= n) {
            *err = "the picture is cut short";
            return NULL;
        }
        len = d[q++];
        if (len == 0) {
            break;
        }
        if (q + len > n) {
            *err = "the picture is cut short";
            return NULL;
        }
        total += len;
        q += len;
    }
    buf = (unsigned char *)malloc(total > 0 ? total : 1);
    if (buf == NULL) {
        *err = "out of memory";
        return NULL;
    }
    o = buf;
    for (;;) {
        size_t len = d[p++];

        if (len == 0) {
            break;
        }
        memcpy(o, d + p, len);
        o += len;
        p += len;
    }
    *pos = p;
    *outlen = total;
    return buf;
}

/* Skips the data sub-blocks at d[*pos]; returns 0, or -1 if they run off the end. */
static int skip_blocks(const unsigned char *d, size_t n, size_t *pos)
{
    size_t p = *pos;

    for (;;) {
        size_t len;

        if (p >= n) {
            return -1;
        }
        len = d[p++];
        if (len == 0) {
            break;
        }
        if (p + len > n) {
            return -1;
        }
        p += len;
    }
    *pos = p;
    return 0;
}

/* LZW-decodes `count` colour indices from the packed codes; returns 0, or -1 with *err. */
static int lzw(const unsigned char *src, size_t srclen, int min_code, unsigned char *out, size_t count, const char **err)
{
    static unsigned short prefix[4096];
    static unsigned char suffix[4096], stack[4097];
    int clear = 1 << min_code, eoi = clear + 1, next = clear + 2, size = min_code + 1, prev = -1, code, i;
    unsigned long bitbuf = 0;
    int bitcnt = 0;
    size_t pos = 0, o = 0;

    for (i = 0; i < clear; i++) {
        suffix[i] = (unsigned char)i;
        prefix[i] = 0;
    }
    while (o < count) {
        int sp = 0, first;

        while (bitcnt < size) {
            if (pos >= srclen) {
                *err = "the picture's data ends too soon";
                return -1;
            }
            bitbuf |= (unsigned long)src[pos++] << bitcnt;
            bitcnt += 8;
        }
        code = (int)(bitbuf & ((1UL << size) - 1));
        bitbuf >>= size;
        bitcnt -= size;
        if (code == clear) {
            next = clear + 2;
            size = min_code + 1;
            prev = -1;
            continue;
        }
        if (code == eoi) {
            *err = "the picture's data ends too soon";
            return -1;
        }
        if (prev < 0) {                                  /* the first code after a clear is a literal */
            if (code >= clear) {
                *err = "bad code in the picture";
                return -1;
            }
            out[o++] = (unsigned char)code;
            prev = code;
            continue;
        }
        if (code > next || (code == next && next >= 4096)) {
            *err = "bad code in the picture";
            return -1;
        }
        if (code == next) {                              /* the KwKwK case: previous string plus its own first byte */
            int c = prev;

            while (c >= clear) c = prefix[c];
            first = suffix[c];
            stack[sp++] = (unsigned char)first;
            c = prev;
            while (c >= clear) {
                stack[sp++] = suffix[c];
                c = prefix[c];
            }
            stack[sp++] = suffix[c];
        } else {
            int c = code;

            while (c >= clear) {
                stack[sp++] = suffix[c];
                c = prefix[c];
            }
            stack[sp++] = suffix[c];
            first = suffix[c];
        }
        if (next < 4096) {
            prefix[next] = (unsigned short)prev;
            suffix[next] = (unsigned char)first;
            next++;
            if (next == (1 << size) && size < 12) {
                size++;
            }
        }
        while (sp > 0) {
            if (o >= count) {
                break;                                   /* more data than the picture needs: ignore the rest */
            }
            out[o++] = stack[--sp];
        }
        prev = code;
    }
    return 0;
}

void gif_anim_free(GifAnim *a)
{
    int i;

    for (i = 0; i < a->nframes; i++) {
        free(a->rgba[i]);
    }
    free(a->rgba);
    free(a->delay_ms);
    memset(a, 0, sizeof(*a));
}

int gif_decode_anim(const unsigned char *d, size_t n, int max_frames, unsigned long max_pixels, GifAnim *a, const char **err)
{
    unsigned long sw, sh, x, y;
    int ngct = 0, have_screen = 0;
    const unsigned char *gct = NULL;
    size_t pos, canvas_bytes = 0;
    unsigned char *canvas = NULL, *saved = NULL, *idx = NULL, *packed = NULL;
    int ok = 0, transparent = -1, disposal = 0, cap = 0, ended = 0;
    unsigned delay_cs = 0;
    /* what the frame before this one asks to be done to the screen before the next is drawn */
    int prev_disposal = 0;
    unsigned long prev_x = 0, prev_y = 0, prev_w = 0, prev_h = 0;

    memset(a, 0, sizeof(*a));
    *err = "not a GIF picture";
    if (n < 13 || (memcmp(d, "GIF87a", 6) != 0 && memcmp(d, "GIF89a", 6) != 0)) {
        return -1;
    }
    if (max_frames < 1) {
        max_frames = 1;
    }
    sw = le16(d + 6);
    sh = le16(d + 8);
    pos = 13;
    if (d[10] & 0x80) {
        ngct = 2 << (d[10] & 7);
        if (pos + (size_t)ngct * 3 > n) {
            *err = "the picture is cut short";
            return -1;
        }
        gct = d + pos;
        pos += (size_t)ngct * 3;
    }
    while (pos < n && !ended) {
        int b = d[pos++];

        if (b == 0x3b) {                                 /* trailer */
            break;
        }
        if (b == 0x21) {                                 /* an extension */
            int label;

            if (pos >= n) {
                break;
            }
            label = d[pos++];
            if (label == 0xf9 && pos + 5 < n && d[pos] == 4) {         /* graphic control: disposal, delay, transparency */
                disposal = (d[pos + 1] >> 2) & 7;
                delay_cs = le16(d + pos + 2);
                transparent = (d[pos + 1] & 1) ? d[pos + 4] : -1;
            }
            if (skip_blocks(d, n, &pos) != 0) {
                break;
            }
            continue;
        }
        if (b != 0x2c) {
            break;                                       /* something this reader does not know: stop here */
        }
        /* an image descriptor */
        {
            unsigned long fx, fy, fw, fh, row;
            int interlace, nlct = 0, min_code, npal, c;
            const unsigned char *lct = NULL, *pal;
            size_t packed_len = 0, i;
            int decoded_ok = 0;

            if (a->nframes >= max_frames || (a->nframes > 0 && (unsigned long)(a->nframes + 1) * sw * sh > max_pixels)) {
                /* no room for more: count the rest of the frames without decoding them */
                int cnt = a->total_frames;

                if (a->total_frames == 0) {
                    a->total_frames = a->nframes;
                    cnt = a->total_frames;
                }
                while (pos + 9 <= n && cnt < MAX_FRAMES_COUNTED) {
                    int flags = d[pos + 8];

                    cnt++;
                    pos += 9;
                    if (flags & 0x80) {
                        pos += (size_t)(2 << (flags & 7)) * 3;
                    }
                    pos++;
                    if (pos > n || skip_blocks(d, n, &pos) != 0) {
                        break;
                    }
                    /* look for the next image descriptor, skipping extensions */
                    while (pos < n && d[pos] == 0x21) {
                        pos += 2;
                        if (pos > n || skip_blocks(d, n, &pos) != 0) {
                            pos = n;
                            break;
                        }
                    }
                    if (pos >= n || d[pos] != 0x2c) {
                        break;
                    }
                    pos++;
                }
                a->total_frames = cnt;
                ended = 1;
                continue;
            }
            if (pos + 9 > n) {
                *err = "the picture is cut short";
                break;
            }
            fx = le16(d + pos);
            fy = le16(d + pos + 2);
            fw = le16(d + pos + 4);
            fh = le16(d + pos + 6);
            interlace = (d[pos + 8] & 0x40) != 0;
            if (d[pos + 8] & 0x80) {
                nlct = 2 << (d[pos + 8] & 7);
                if (pos + 9 + (size_t)nlct * 3 > n) {
                    *err = "the picture is cut short";
                    break;
                }
                lct = d + pos + 9;
                pos += 9 + (size_t)nlct * 3;
            } else {
                pos += 9;
            }
            if (fw == 0 || fh == 0 || fw * fh > GIF_MAX_PIXELS) {
                *err = "the picture is too large or has no size";
                break;
            }
            if (pos >= n) {
                *err = "the picture is cut short";
                break;
            }
            min_code = d[pos++];
            if (min_code < 2 || min_code > 8) {
                *err = "bad GIF code size";
                break;
            }
            free(packed);
            packed = join_blocks(d, n, &pos, &packed_len, err);
            if (packed == NULL) {
                break;
            }
            free(idx);
            idx = (unsigned char *)malloc((size_t)(fw * fh));
            if (idx == NULL) {
                *err = "out of memory";
                break;
            }
            if (lzw(packed, packed_len, min_code, idx, (size_t)(fw * fh), err) != 0) {
                break;
            }
            if (lct == NULL && gct == NULL) {
                *err = "the picture has no colour table";
                break;
            }
            pal = lct != NULL ? lct : gct;
            npal = lct != NULL ? nlct : ngct;
            if (!have_screen) {                          /* set up the screen from the first frame */
                if (sw == 0 || sh == 0) {
                    sw = fx + fw;
                    sh = fy + fh;
                }
                if (sw * sh > GIF_MAX_PIXELS || sw > PNG_MAX_SIDE || sh > PNG_MAX_SIDE) {
                    *err = "the picture is too large";
                    break;
                }
                canvas_bytes = (size_t)(sw * sh * 4UL);
                canvas = (unsigned char *)calloc(canvas_bytes, 1);
                saved = (unsigned char *)malloc(canvas_bytes);
                if (canvas == NULL || saved == NULL) {
                    *err = "out of memory";
                    break;
                }
                have_screen = 1;
            }
            /* what the frame before this asked for happens first */
            if (prev_disposal == 2) {
                for (y = prev_y; y < prev_y + prev_h && y < sh; y++) {
                    for (x = prev_x; x < prev_x + prev_w && x < sw; x++) {
                        memset(canvas + (y * sw + x) * 4, 0, 4);
                    }
                }
            } else if (prev_disposal == 3) {
                memcpy(canvas, saved, canvas_bytes);
            }
            if (disposal == 3) {
                memcpy(saved, canvas, canvas_bytes);
            }
            for (i = 0; i < (size_t)fh; i++) {
                if (interlace) {                         /* rows arrive in four passes */
                    unsigned long p1 = (fh + 7) / 8, p2 = (fh + 3) / 8, p3 = (fh + 1) / 4;

                    if (i < p1) row = i * 8;
                    else if (i < p1 + p2) row = 4 + (i - p1) * 8;
                    else if (i < p1 + p2 + p3) row = 2 + (i - p1 - p2) * 4;
                    else row = 1 + (i - p1 - p2 - p3) * 2;
                } else {
                    row = i;
                }
                y = fy + row;
                if (y >= sh) {
                    continue;                            /* the frame sticks out of the screen: clip it */
                }
                for (x = 0; x < fw; x++) {
                    unsigned long ox = fx + x;
                    unsigned char *o;

                    c = idx[i * fw + x];
                    if (ox >= sw || c == transparent) {
                        continue;                        /* off the screen, or see-through: leave what is there */
                    }
                    o = canvas + (y * sw + ox) * 4;
                    if (c < npal) {
                        o[0] = pal[c * 3];
                        o[1] = pal[c * 3 + 1];
                        o[2] = pal[c * 3 + 2];
                    } else {
                        o[0] = o[1] = o[2] = 0;          /* an index past the table is black */
                    }
                    o[3] = 255;
                }
            }
            decoded_ok = 1;
            /* snapshot the screen as this frame */
            if (a->nframes >= cap) {
                int ncap = cap ? cap * 2 : 4;
                unsigned char **nr = (unsigned char **)realloc(a->rgba, (size_t)ncap * sizeof(unsigned char *));
                unsigned *nd;

                if (nr == NULL) {
                    *err = "out of memory";
                    decoded_ok = 0;
                } else {
                    a->rgba = nr;
                    nd = (unsigned *)realloc(a->delay_ms, (size_t)ncap * sizeof(unsigned));
                    if (nd == NULL) {
                        *err = "out of memory";
                        decoded_ok = 0;
                    } else {
                        a->delay_ms = nd;
                        cap = ncap;
                    }
                }
            }
            if (decoded_ok) {
                a->rgba[a->nframes] = (unsigned char *)malloc(canvas_bytes);
                if (a->rgba[a->nframes] == NULL) {
                    *err = "out of memory";
                    decoded_ok = 0;
                }
            }
            if (!decoded_ok) {
                break;
            }
            memcpy(a->rgba[a->nframes], canvas, canvas_bytes);
            a->delay_ms[a->nframes] = delay_cs <= 1 ? 100U : delay_cs * 10U;
            a->nframes++;
            prev_disposal = disposal;
            prev_x = fx;
            prev_y = fy;
            prev_w = fw;
            prev_h = fh;
            disposal = 0;                                /* a graphic control block applies to the next frame only */
            transparent = -1;
            delay_cs = 0;
        }
    }
    if (a->nframes == 0) {
        if (strcmp(*err, "not a GIF picture") == 0) {
            *err = "the picture has no image";
        }
        goto done;
    }
    a->width = sw;
    a->height = sh;
    if (a->total_frames < a->nframes) {
        a->total_frames = a->nframes;
    }
    *err = "ok";
    ok = 1;
done:
    free(canvas);
    free(saved);
    free(idx);
    free(packed);
    if (!ok) {
        gif_anim_free(a);
        return -1;
    }
    return 0;
}

int gif_decode(const unsigned char *d, size_t n, PngImage *img, int *frames, const char **err)
{
    GifAnim a;

    memset(img, 0, sizeof(*img));
    if (frames != NULL) {
        *frames = 0;
    }
    if (gif_decode_anim(d, n, 1, GIF_MAX_PIXELS, &a, err) != 0) {
        return -1;
    }
    img->width = a.width;
    img->height = a.height;
    img->rgba = a.rgba[0];
    a.rgba[0] = NULL;
    if (frames != NULL) {
        *frames = a.total_frames;
    }
    gif_anim_free(&a);
    return 0;
}
