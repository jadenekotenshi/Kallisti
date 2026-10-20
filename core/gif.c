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

int gif_decode(const unsigned char *d, size_t n, PngImage *img, int *frames, const char **err)
{
    unsigned long sw, sh, fw = 0, fh = 0, fx = 0, fy = 0, x, y;
    int ngct = 0, nlct = 0, interlace = 0, transparent = -1, have_frame = 0, nframes = 0, min_code = 0;
    const unsigned char *gct = NULL, *lct = NULL;
    unsigned char *idx = NULL, *packed = NULL;
    size_t pos, packed_len = 0, i;
    int ok = 0;

    memset(img, 0, sizeof(*img));
    if (frames != NULL) {
        *frames = 0;
    }
    *err = "not a GIF picture";
    if (n < 13 || (memcmp(d, "GIF87a", 6) != 0 && memcmp(d, "GIF89a", 6) != 0)) {
        return -1;
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
    while (pos < n) {
        int b = d[pos++];

        if (b == 0x3b) {                                 /* trailer */
            break;
        }
        if (b == 0x21) {                                 /* an extension */
            int label;

            if (pos >= n) {
                *err = "the picture is cut short";
                goto done;
            }
            label = d[pos++];
            if (label == 0xf9 && pos + 5 < n && d[pos] == 4 && !have_frame) {
                transparent = (d[pos + 1] & 1) ? d[pos + 4] : -1;
            }
            if (skip_blocks(d, n, &pos) != 0) {
                *err = "the picture is cut short";
                goto done;
            }
            continue;
        }
        if (b != 0x2c) {
            *err = "the picture has a block this reader does not know";
            goto done;
        }
        /* an image descriptor */
        if (have_frame) {                                /* a later frame: count it and move on */
            if (pos + 9 > n) {
                break;
            }
            {
                int flags = d[pos + 8];

                pos += 9;
                if (flags & 0x80) {
                    pos += (size_t)(2 << (flags & 7)) * 3;
                }
                pos++;                                   /* the minimum code size */
            }
            if (pos > n || skip_blocks(d, n, &pos) != 0) {
                break;
            }
            if (++nframes >= MAX_FRAMES_COUNTED) {
                break;
            }
            continue;
        }
        if (pos + 9 > n) {
            *err = "the picture is cut short";
            goto done;
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
                goto done;
            }
            lct = d + pos + 9;
            pos += 9 + (size_t)nlct * 3;
        } else {
            pos += 9;
        }
        if (fw == 0 || fh == 0 || fw * fh > GIF_MAX_PIXELS) {
            *err = "the picture is too large or has no size";
            goto done;
        }
        if (pos >= n) {
            *err = "the picture is cut short";
            goto done;
        }
        min_code = d[pos++];
        if (min_code < 2 || min_code > 8) {
            *err = "bad GIF code size";
            goto done;
        }
        packed = join_blocks(d, n, &pos, &packed_len, err);
        if (packed == NULL) {
            goto done;
        }
        idx = (unsigned char *)malloc((size_t)(fw * fh));
        if (idx == NULL) {
            *err = "out of memory";
            goto done;
        }
        if (lzw(packed, packed_len, min_code, idx, (size_t)(fw * fh), err) != 0) {
            goto done;
        }
        have_frame = 1;
        nframes = 1;
    }
    if (!have_frame) {
        *err = "the picture has no image";
        goto done;
    }
    if (lct == NULL && gct == NULL) {
        *err = "the picture has no colour table";
        goto done;
    }
    if (sw == 0 || sh == 0) {                            /* a screen size of 0: take the frame's */
        sw = fx + fw;
        sh = fy + fh;
    }
    if (sw * sh > GIF_MAX_PIXELS || sw > PNG_MAX_SIDE || sh > PNG_MAX_SIDE) {
        *err = "the picture is too large";
        goto done;
    }
    img->rgba = (unsigned char *)calloc((size_t)(sw * sh * 4UL), 1);        /* transparent black */
    if (img->rgba == NULL) {
        *err = "out of memory";
        goto done;
    }
    {
        const unsigned char *pal = lct != NULL ? lct : gct;
        int npal = lct != NULL ? nlct : ngct;
        unsigned long row;

        for (i = 0; i < (size_t)fh; i++) {
            if (interlace) {                             /* rows arrive in four passes */
                unsigned long hh = fh;
                unsigned long p1 = (hh + 7) / 8, p2 = (hh + 3) / 8, p3 = (hh + 1) / 4;

                if (i < p1) row = i * 8;
                else if (i < p1 + p2) row = 4 + (i - p1) * 8;
                else if (i < p1 + p2 + p3) row = 2 + (i - p1 - p2) * 4;
                else row = 1 + (i - p1 - p2 - p3) * 2;
            } else {
                row = i;
            }
            y = fy + row;
            if (y >= sh) {
                continue;                                /* the frame sticks out of the screen: clip it */
            }
            for (x = 0; x < fw; x++) {
                unsigned long ox = fx + x;
                int c = idx[i * fw + x];
                unsigned char *o;

                if (ox >= sw) {
                    continue;
                }
                o = img->rgba + (y * sw + ox) * 4;
                if (c == transparent) {
                    continue;                            /* stays transparent */
                }
                if (c < npal) {
                    o[0] = pal[c * 3];
                    o[1] = pal[c * 3 + 1];
                    o[2] = pal[c * 3 + 2];
                }                                        /* (an index past the table is black) */
                o[3] = 255;
            }
        }
    }
    img->width = sw;
    img->height = sh;
    if (frames != NULL) {
        *frames = nframes;
    }
    *err = "ok";
    ok = 1;
done:
    free(idx);
    free(packed);
    if (!ok) {
        free(img->rgba);
        memset(img, 0, sizeof(*img));
        return -1;
    }
    return 0;
}
