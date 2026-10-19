/* PNG decoding: see png.h. */
#include <stdlib.h>
#include <string.h>
#include "png.h"
#include "inflate.h"

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

typedef struct {
    unsigned long w, h;
    int depth, ctype, interlace;
    int channels;                /* samples per pixel in the file */
    unsigned char pal[256][3];
    unsigned char pal_alpha[256];
    int npal;
    int has_key;                 /* tRNS colour key for grey / RGB */
    unsigned key[3];
} Hdr;

/* bytes in one scanline of `width` pixels, without the filter byte */
static size_t row_bytes(const Hdr *h, unsigned long width)
{
    return (size_t)((width * (unsigned long)h->channels * (unsigned long)h->depth + 7UL) / 8UL);
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Undoes the filter of `rows` scanlines at p (each 1 + rb bytes); returns 0 or -1. */
static int unfilter(unsigned char *p, unsigned long rows, size_t rb, size_t bpp)
{
    unsigned long y;
    size_t i;
    unsigned char *cur, *prev = NULL;

    for (y = 0; y < rows; y++) {
        int f = p[0];

        cur = p + 1;
        switch (f) {
        case 0:
            break;
        case 1:
            for (i = bpp; i < rb; i++) cur[i] = (unsigned char)(cur[i] + cur[i - bpp]);
            break;
        case 2:
            if (prev) for (i = 0; i < rb; i++) cur[i] = (unsigned char)(cur[i] + prev[i]);
            break;
        case 3:
            for (i = 0; i < rb; i++) {
                int left = i >= bpp ? cur[i - bpp] : 0, up = prev ? prev[i] : 0;

                cur[i] = (unsigned char)(cur[i] + ((left + up) >> 1));
            }
            break;
        case 4:
            for (i = 0; i < rb; i++) {
                int left = i >= bpp ? cur[i - bpp] : 0, up = prev ? prev[i] : 0, ul = (prev && i >= bpp) ? prev[i - bpp] : 0;

                cur[i] = (unsigned char)(cur[i] + paeth(left, up, ul));
            }
            break;
        default:
            return -1;
        }
        prev = cur;
        p += 1 + rb;
    }
    return 0;
}

/* One sample (0 .. 2^depth-1 for depth <= 8, the full 16 bits for 16) of pixel x, channel c, in an unfiltered row. */
static unsigned sample(const Hdr *h, const unsigned char *row, unsigned long x, int c)
{
    unsigned long idx = x * (unsigned long)h->channels + (unsigned long)c;

    switch (h->depth) {
    case 8:  return row[idx];
    case 16: return ((unsigned)row[idx * 2] << 8) | row[idx * 2 + 1];
    case 4:  return (row[idx >> 1] >> (4 - 4 * (int)(idx & 1))) & 0x0f;
    case 2:  return (row[idx >> 2] >> (6 - 2 * (int)(idx & 3))) & 0x03;
    default: return (row[idx >> 3] >> (7 - (int)(idx & 7))) & 0x01;
    }
}

static unsigned char scale8(const Hdr *h, unsigned v)
{
    switch (h->depth) {
    case 16: return (unsigned char)(v >> 8);
    case 8:  return (unsigned char)v;
    case 4:  return (unsigned char)(v * 17);
    case 2:  return (unsigned char)(v * 85);
    default: return (unsigned char)(v * 255);
    }
}

/* Converts pixel x of an unfiltered row to RGBA at out. Returns -1 for a palette index out of range. */
static int pixel(const Hdr *h, const unsigned char *row, unsigned long x, unsigned char *out)
{
    unsigned v, r, g, b;

    switch (h->ctype) {
    case 0:
        v = sample(h, row, x, 0);
        out[0] = out[1] = out[2] = scale8(h, v);
        out[3] = (h->has_key && v == h->key[0]) ? 0 : 255;
        return 0;
    case 2:
        r = sample(h, row, x, 0);
        g = sample(h, row, x, 1);
        b = sample(h, row, x, 2);
        out[0] = scale8(h, r);
        out[1] = scale8(h, g);
        out[2] = scale8(h, b);
        out[3] = (h->has_key && r == h->key[0] && g == h->key[1] && b == h->key[2]) ? 0 : 255;
        return 0;
    case 3:
        v = sample(h, row, x, 0);
        if ((int)v >= h->npal) return -1;
        out[0] = h->pal[v][0];
        out[1] = h->pal[v][1];
        out[2] = h->pal[v][2];
        out[3] = h->pal_alpha[v];
        return 0;
    case 4:
        v = sample(h, row, x, 0);
        out[0] = out[1] = out[2] = scale8(h, v);
        out[3] = scale8(h, sample(h, row, x, 1));
        return 0;
    default:
        out[0] = scale8(h, sample(h, row, x, 0));
        out[1] = scale8(h, sample(h, row, x, 1));
        out[2] = scale8(h, sample(h, row, x, 2));
        out[3] = scale8(h, sample(h, row, x, 3));
        return 0;
    }
}

static const int ADAM_X0[7] = { 0, 4, 0, 2, 0, 1, 0 };
static const int ADAM_Y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const int ADAM_DX[7] = { 8, 8, 4, 4, 2, 2, 1 };
static const int ADAM_DY[7] = { 8, 8, 8, 4, 4, 2, 2 };

int png_decode(const unsigned char *data, size_t len, PngImage *img, const char **err)
{
    static const unsigned char SIG[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    Hdr h;
    size_t pos = 8, idat_len = 0, idat_cap = 0, raw_len, rb, bpp;
    unsigned char *idat = NULL, *raw = NULL;
    int seen_ihdr = 0, seen_idat = 0, seen_iend = 0, ok = 0, pass, npass;
    const unsigned char *d;
    unsigned long clen, y, x;

    memset(&h, 0, sizeof(h));
    memset(img, 0, sizeof(*img));
    *err = "not a PNG picture";
    if (len < 8 + 12 || memcmp(data, SIG, 8) != 0) {
        return -1;
    }
    memset(h.pal_alpha, 255, sizeof(h.pal_alpha));

    while (pos + 12 <= len && !seen_iend) {
        const unsigned char *type;

        clen = be32(data + pos);
        type = data + pos + 4;
        d = data + pos + 8;
        if (clen > 0x7fffffffUL || pos + 12 + (size_t)clen > len) {
            *err = "the picture is cut short";
            goto done;
        }
        if (crc32_sum(0, type, 4 + (size_t)clen) != be32(d + clen)) {
            *err = "the picture is damaged (chunk checksum)";
            goto done;
        }
        if (memcmp(type, "IHDR", 4) == 0) {
            if (seen_ihdr || clen != 13) {
                *err = "bad PNG header";
                goto done;
            }
            seen_ihdr = 1;
            h.w = be32(d);
            h.h = be32(d + 4);
            h.depth = d[8];
            h.ctype = d[9];
            h.interlace = d[12];
            if (d[10] != 0 || d[11] != 0 || h.interlace > 1) {
                *err = "unsupported PNG method";
                goto done;
            }
            switch (h.ctype) {
            case 0: h.channels = 1; ok = h.depth == 1 || h.depth == 2 || h.depth == 4 || h.depth == 8 || h.depth == 16; break;
            case 2: h.channels = 3; ok = h.depth == 8 || h.depth == 16; break;
            case 3: h.channels = 1; ok = h.depth == 1 || h.depth == 2 || h.depth == 4 || h.depth == 8; break;
            case 4: h.channels = 2; ok = h.depth == 8 || h.depth == 16; break;
            case 6: h.channels = 4; ok = h.depth == 8 || h.depth == 16; break;
            default: ok = 0;
            }
            if (!ok) {
                *err = "unsupported PNG colour type or depth";
                goto done;
            }
            ok = 0;
            if (h.w == 0 || h.h == 0 || h.w > PNG_MAX_SIDE || h.h > PNG_MAX_SIDE || h.w * h.h > PNG_MAX_PIXELS) {
                *err = "the picture is too large";
                goto done;
            }
        } else if (!seen_ihdr) {
            *err = "bad PNG header";
            goto done;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            unsigned long i;

            if (clen % 3 != 0 || clen == 0 || clen > 768 || seen_idat || h.ctype == 0 || h.ctype == 4) {
                *err = "bad PNG palette";
                goto done;
            }
            h.npal = (int)(clen / 3);
            for (i = 0; i < clen / 3; i++) memcpy(h.pal[i], d + i * 3, 3);
        } else if (memcmp(type, "tRNS", 4) == 0) {
            unsigned long i;

            if (h.ctype == 3) {
                if (clen > 256) {
                    *err = "bad PNG transparency";
                    goto done;
                }
                for (i = 0; i < clen; i++) h.pal_alpha[i] = d[i];
            } else if (h.ctype == 0 && clen == 2) {
                h.has_key = 1;
                h.key[0] = ((unsigned)d[0] << 8) | d[1];
            } else if (h.ctype == 2 && clen == 6) {
                h.has_key = 1;
                h.key[0] = ((unsigned)d[0] << 8) | d[1];
                h.key[1] = ((unsigned)d[2] << 8) | d[3];
                h.key[2] = ((unsigned)d[4] << 8) | d[5];
            }                    /* (with an alpha channel tRNS is not allowed; ignore it) */
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_len + clen > idat_cap) {
                size_t ncap = idat_cap ? idat_cap : 65536;
                unsigned char *nb;

                while (ncap < idat_len + clen) ncap *= 2;
                nb = (unsigned char *)realloc(idat, ncap);
                if (nb == NULL) {
                    *err = "out of memory";
                    goto done;
                }
                idat = nb;
                idat_cap = ncap;
            }
            memcpy(idat + idat_len, d, clen);
            idat_len += clen;
            seen_idat = 1;
        } else if (memcmp(type, "IEND", 4) == 0) {
            seen_iend = 1;
        } else if ((type[0] & 0x20) == 0) {
            *err = "the picture uses a feature this reader does not know";
            goto done;
        }
        pos += 12 + (size_t)clen;
    }
    if (!seen_ihdr || !seen_idat || !seen_iend) {
        *err = "the picture is incomplete";
        goto done;
    }
    if (h.ctype == 3 && h.npal == 0) {
        *err = "the picture needs a palette it does not have";
        goto done;
    }

    /* the size of the unfiltered data: one filter byte per scanline of every pass */
    npass = h.interlace ? 7 : 1;
    raw_len = 0;
    for (pass = 0; pass < npass; pass++) {
        unsigned long pw = h.interlace ? (h.w - (unsigned long)ADAM_X0[pass] + (unsigned long)ADAM_DX[pass] - 1) / (unsigned long)ADAM_DX[pass] : h.w;
        unsigned long ph = h.interlace ? (h.h - (unsigned long)ADAM_Y0[pass] + (unsigned long)ADAM_DY[pass] - 1) / (unsigned long)ADAM_DY[pass] : h.h;

        if (h.interlace && (h.w <= (unsigned long)ADAM_X0[pass] || h.h <= (unsigned long)ADAM_Y0[pass])) {
            continue;
        }
        raw_len += (size_t)ph * (1 + row_bytes(&h, pw));
    }
    raw = (unsigned char *)malloc(raw_len > 0 ? raw_len : 1);
    img->rgba = (unsigned char *)malloc((size_t)(h.w * h.h * 4UL));
    if (raw == NULL || img->rgba == NULL) {
        *err = "out of memory";
        goto done;
    }
    if (inflate_zlib(idat, idat_len, raw, raw_len, err) != 0) {
        goto done;
    }

    bpp = (size_t)((h.channels * h.depth + 7) / 8);
    {
        unsigned char *p = raw;

        for (pass = 0; pass < npass; pass++) {
            unsigned long pw, ph, px, py;

            if (h.interlace) {
                if (h.w <= (unsigned long)ADAM_X0[pass] || h.h <= (unsigned long)ADAM_Y0[pass]) continue;
                pw = (h.w - (unsigned long)ADAM_X0[pass] + (unsigned long)ADAM_DX[pass] - 1) / (unsigned long)ADAM_DX[pass];
                ph = (h.h - (unsigned long)ADAM_Y0[pass] + (unsigned long)ADAM_DY[pass] - 1) / (unsigned long)ADAM_DY[pass];
            } else {
                pw = h.w;
                ph = h.h;
            }
            rb = row_bytes(&h, pw);
            if (unfilter(p, ph, rb, bpp) != 0) {
                *err = "bad PNG filter";
                goto done;
            }
            for (py = 0; py < ph; py++) {
                const unsigned char *row = p + py * (1 + rb) + 1;

                y = h.interlace ? (unsigned long)ADAM_Y0[pass] + py * (unsigned long)ADAM_DY[pass] : py;
                for (px = 0; px < pw; px++) {
                    x = h.interlace ? (unsigned long)ADAM_X0[pass] + px * (unsigned long)ADAM_DX[pass] : px;
                    if (pixel(&h, row, px, img->rgba + (y * h.w + x) * 4) != 0) {
                        *err = "a palette index is out of range";
                        goto done;
                    }
                }
            }
            p += ph * (1 + rb);
        }
    }
    img->width = h.w;
    img->height = h.h;
    *err = "ok";
    free(raw);
    free(idat);
    return 0;
done:
    free(raw);
    free(idat);
    free(img->rgba);
    memset(img, 0, sizeof(*img));
    return -1;
}
