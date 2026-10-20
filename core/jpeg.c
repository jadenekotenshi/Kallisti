/* JPEG decoding: see jpeg.h.  Structure: parse the markers; entropy-decode every scan into a coefficient buffer
 * per component (baseline and progressive alike); then dequantise + IDCT every block, upsample the chroma planes
 * and convert to RGBA.  The IDCT, the "fancy" upsampling and the colour conversion follow libjpeg's own
 * arithmetic so the output is bit-for-bit what libjpeg makes. */
#include <stdlib.h>
#include <string.h>
#include "jpeg.h"

#define MAX_COMP 3
#define FASTBITS 9
#define MAX_COEF_BYTES (96UL * 1024UL * 1024UL)

static const unsigned char ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct {
    unsigned char bits[17];
    unsigned char vals[256];
    int maxcode[18];             /* the largest code of each length, -1 if there is none; [17] is a sentinel */
    int valptr[17];
    int mincode[17];
    unsigned short fast[1 << FASTBITS];     /* (length << 8) | symbol for codes of up to FASTBITS bits, else 0 */
    int present;
} HuffTable;

typedef struct {
    int id, h, v, tq;
    unsigned long bw, bh;        /* blocks across and down, padded to whole MCUs */
    unsigned long w, ht;         /* the component's real (downsampled) size in samples */
    short *coef;                 /* bw * bh * 64 */
    unsigned char *plane;        /* bw*8 by bh*8 samples after the IDCT */
    int dc_pred;
    int dc_tbl, ac_tbl;
} Comp;

typedef struct {
    const unsigned char *d;
    size_t n, pos;
    unsigned long bitbuf;
    int bitcnt;
    int marker;                  /* a marker met while reading entropy data, or 0 */
    int starved;                 /* zero bytes fed because the data ran out (a few are normal read-ahead) */

    unsigned short qt[4][64];    /* in natural order */
    HuffTable dc[4], ac[4];
    Comp comp[MAX_COMP];
    int ncomp, progressive;
    unsigned long width, height;
    int hmax, vmax;
    unsigned long mcux, mcuy;
    int restart_interval;
    int adobe_transform;         /* -1: no Adobe marker */
    int saw_sof, saw_sos;
    int eobrun;
    const char *err;
} Dec;

static int fail(Dec *s, const char *msg)
{
    if (s->err == NULL) {
        s->err = msg;
    }
    return -1;
}

/* ---------------------------------------------------------------- Huffman tables */

static int build_huff(Dec *s, HuffTable *t)
{
    int i, j, k = 0, code = 0, len;

    for (len = 1; len <= 16; len++) {
        t->valptr[len] = k;
        t->mincode[len] = code;
        k += t->bits[len];
        code += t->bits[len];
        if (code > (1 << len)) {
            return fail(s, "bad Huffman table");
        }
        t->maxcode[len] = t->bits[len] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7fffffff;
    memset(t->fast, 0, sizeof(t->fast));
    k = 0;
    code = 0;
    for (len = 1; len <= FASTBITS; len++) {
        for (i = 0; i < t->bits[len]; i++, k++, code++) {
            int first = code << (FASTBITS - len);

            for (j = 0; j < (1 << (FASTBITS - len)); j++) {
                t->fast[first + j] = (unsigned short)((len << 8) | t->vals[k]);
            }
        }
        code <<= 1;
    }
    t->present = 1;
    return 0;
}

/* ---------------------------------------------------------------- the bit reader */

/* Tops the buffer up to more than 24 bits.  A marker (or the end of the data) stops the supply: zeros follow. */
static void fill(Dec *s)
{
    while (s->bitcnt <= 24) {
        int b = 0;

        if (s->marker == 0 && s->pos < s->n) {
            b = s->d[s->pos++];
            if (b == 0xff) {
                int m;

                do {
                    m = s->pos < s->n ? s->d[s->pos++] : -1;
                } while (m == 0xff);
                if (m == 0) {
                    b = 0xff;                                  /* a stuffed 0xFF data byte */
                } else {
                    s->marker = m < 0 ? 0xd9 : m;              /* (a file that ends after an 0xFF is treated as EOI) */
                    b = 0;
                    s->starved++;
                }
            }
        } else {
            s->starved++;
        }
        s->bitbuf |= (unsigned long)b << (24 - s->bitcnt);
        s->bitcnt += 8;
    }
}

/* the next n (0..16) bits, most significant first */
static int getbits(Dec *s, int n)
{
    int v;

    if (n == 0) {
        return 0;
    }
    if (s->bitcnt < n) {
        fill(s);
    }
    v = (int)((s->bitbuf >> (32 - n)) & ((1UL << n) - 1));
    s->bitbuf = (s->bitbuf << n) & 0xffffffffUL;
    s->bitcnt -= n;
    return v;
}

static int huff_decode(Dec *s, const HuffTable *t)
{
    int l, code;
    unsigned e;

    if (s->bitcnt < 16) {
        fill(s);
    }
    e = t->fast[(s->bitbuf >> (32 - FASTBITS)) & ((1U << FASTBITS) - 1)];
    if (e != 0) {
        int len = (int)(e >> 8);

        s->bitbuf = (s->bitbuf << len) & 0xffffffffUL;
        s->bitcnt -= len;
        return (int)(e & 0xff);
    }
    for (l = FASTBITS + 1; l <= 16; l++) {
        code = (int)(s->bitbuf >> (32 - l));
        if (code <= t->maxcode[l]) {
            s->bitbuf = (s->bitbuf << l) & 0xffffffffUL;
            s->bitcnt -= l;
            return t->vals[t->valptr[l] + code - t->mincode[l]];
        }
    }
    fail(s, "bad Huffman code in the picture");
    return -1;
}

/* the value of an s-bit magnitude: the sign convention of JPEG's "extend" */
static int extend(int v, int s)
{
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

/* ---------------------------------------------------------------- the blocks of a scan */

static int decode_block_baseline(Dec *s, Comp *c, short *blk)
{
    int t, diff, k, rs, r, sz;
    const HuffTable *dc = &s->dc[c->dc_tbl], *ac = &s->ac[c->ac_tbl];

    t = huff_decode(s, dc);
    if (t < 0 || t > 15) {
        return fail(s, "bad Huffman code in the picture");
    }
    diff = t ? extend(getbits(s, t), t) : 0;
    c->dc_pred += diff;
    blk[0] = (short)c->dc_pred;
    for (k = 1; k < 64;) {
        rs = huff_decode(s, ac);
        if (rs < 0) {
            return -1;
        }
        sz = rs & 15;
        r = rs >> 4;
        if (sz == 0) {
            if (r != 15) {
                break;
            }
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) {
            return fail(s, "bad coefficient in the picture");
        }
        blk[ZIGZAG[k]] = (short)extend(getbits(s, sz), sz);
        k++;
    }
    return 0;
}

static int decode_dc_first(Dec *s, Comp *c, short *blk, int al)
{
    int t = huff_decode(s, &s->dc[c->dc_tbl]), diff;

    if (t < 0 || t > 15) {
        return fail(s, "bad Huffman code in the picture");
    }
    diff = t ? extend(getbits(s, t), t) : 0;
    c->dc_pred += diff;
    blk[0] = (short)(c->dc_pred * (1 << al));
    return 0;
}

static int decode_ac_first(Dec *s, Comp *c, short *blk, int ss, int se, int al)
{
    int k, rs, r, sz;

    if (s->eobrun > 0) {
        s->eobrun--;
        return 0;
    }
    for (k = ss; k <= se;) {
        rs = huff_decode(s, &s->ac[c->ac_tbl]);
        if (rs < 0) {
            return -1;
        }
        sz = rs & 15;
        r = rs >> 4;
        if (sz == 0) {
            if (r < 15) {
                s->eobrun = (1 << r) - 1;
                if (r) {
                    s->eobrun += getbits(s, r);
                }
                break;
            }
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) {
            return fail(s, "bad coefficient in the picture");
        }
        blk[ZIGZAG[k]] = (short)(extend(getbits(s, sz), sz) * (1 << al));
        k++;
    }
    return 0;
}

static void refine_nonzero(Dec *s, short *p, int p1, int m1)
{
    if (getbits(s, 1)) {
        if ((*p & p1) == 0) {
            *p = (short)(*p >= 0 ? *p + p1 : *p + m1);
        }
    }
}

static int decode_ac_refine(Dec *s, Comp *c, short *blk, int ss, int se, int al)
{
    int p1 = 1 << al, m1 = -(1 << al), k = ss, rs, r, sz;
    short *p;

    if (s->eobrun == 0) {
        for (; k <= se; k++) {
            rs = huff_decode(s, &s->ac[c->ac_tbl]);
            if (rs < 0) {
                return -1;
            }
            r = rs >> 4;
            sz = rs & 15;
            if (sz) {
                if (sz != 1) {
                    return fail(s, "bad coefficient in the picture");
                }
                sz = getbits(s, 1) ? p1 : m1;
            } else if (r != 15) {
                s->eobrun = 1 << r;
                if (r) {
                    s->eobrun += getbits(s, r);
                }
                break;
            }
            /* advance over already-nonzero coefficients and r still-zero ones, refining the nonzero ones on the way */
            do {
                p = &blk[ZIGZAG[k]];
                if (*p != 0) {
                    refine_nonzero(s, p, p1, m1);
                } else if (--r < 0) {
                    break;
                }
                k++;
            } while (k <= se);
            if (sz && k <= 63) {
                blk[ZIGZAG[k]] = (short)sz;
            }
        }
    }
    if (s->eobrun > 0) {
        for (; k <= se; k++) {
            p = &blk[ZIGZAG[k]];
            if (*p != 0) {
                refine_nonzero(s, p, p1, m1);
            }
        }
        s->eobrun--;
    }
    return 0;
}

/* ---------------------------------------------------------------- scans */

typedef struct {
    int ncomp;
    int idx[MAX_COMP];           /* indices into Dec.comp */
    int ss, se, ah, al;
} Scan;

static int decode_one(Dec *s, const Scan *sc, Comp *c, short *blk)
{
    if (!s->progressive) {
        return decode_block_baseline(s, c, blk);
    }
    if (sc->ss == 0) {
        if (sc->ah == 0) {
            return decode_dc_first(s, c, blk, sc->al);
        }
        if (getbits(s, 1)) {
            blk[0] = (short)(blk[0] | (1 << sc->al));
        }
        return 0;
    }
    return sc->ah == 0 ? decode_ac_first(s, c, blk, sc->ss, sc->se, sc->al)
                       : decode_ac_refine(s, c, blk, sc->ss, sc->se, sc->al);
}

/* Between restart intervals: drop the partial byte, find the RSTn marker, reset the predictors. */
static int do_restart(Dec *s)
{
    int i;

    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->marker >= 0xd0 && s->marker <= 0xd7) {
        s->marker = 0;
    } else if (s->marker == 0) {
        while (s->pos + 1 < s->n) {                          /* skip anything up to the next RSTn */
            if (s->d[s->pos] == 0xff && s->d[s->pos + 1] >= 0xd0 && s->d[s->pos + 1] <= 0xd7) {
                s->pos += 2;
                break;
            }
            s->pos++;
        }
    } else {
        return fail(s, "a restart marker is missing in the picture");
    }
    s->starved = 0;
    s->eobrun = 0;
    for (i = 0; i < s->ncomp; i++) {
        s->comp[i].dc_pred = 0;
    }
    return 0;
}

static int decode_scan(Dec *s, const Scan *sc)
{
    unsigned long mcus, i, count = 0;
    int ci, i2;

    s->bitbuf = 0;
    s->bitcnt = 0;
    s->marker = 0;
    s->starved = 0;
    s->eobrun = 0;
    for (i2 = 0; i2 < s->ncomp; i2++) {
        s->comp[i2].dc_pred = 0;
    }
    if (sc->ncomp == 1) {
        Comp *c = &s->comp[sc->idx[0]];
        unsigned long rw = (c->w + 7) / 8, rh = (c->ht + 7) / 8;

        mcus = rw * rh;
        for (i = 0; i < mcus; i++) {
            unsigned long bx = i % rw, by = i / rw;

            if (s->restart_interval && count > 0 && count % (unsigned long)s->restart_interval == 0 && do_restart(s) != 0) {
                return -1;
            }
            if (decode_one(s, sc, c, c->coef + (by * c->bw + bx) * 64) != 0) {
                return -1;
            }
            count++;
        }
    } else {
        mcus = s->mcux * s->mcuy;
        for (i = 0; i < mcus; i++) {
            unsigned long mx = i % s->mcux, my = i / s->mcux;

            if (s->restart_interval && count > 0 && count % (unsigned long)s->restart_interval == 0 && do_restart(s) != 0) {
                return -1;
            }
            for (ci = 0; ci < sc->ncomp; ci++) {
                Comp *c = &s->comp[sc->idx[ci]];
                int bv, bh;

                for (bv = 0; bv < c->v; bv++) {
                    for (bh = 0; bh < c->h; bh++) {
                        unsigned long bx = mx * (unsigned long)c->h + (unsigned long)bh;
                        unsigned long by = my * (unsigned long)c->v + (unsigned long)bv;

                        if (decode_one(s, sc, c, c->coef + (by * c->bw + bx) * 64) != 0) {
                            return -1;
                        }
                    }
                }
            }
            count++;
        }
    }
    if (s->starved > 6) {
        return fail(s, "the picture is cut short or damaged");
    }
    return 0;
}

/* ---------------------------------------------------------------- marker segments */

static unsigned seg16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

static int parse_sof(Dec *s, const unsigned char *p, size_t len, int progressive)
{
    int i;
    unsigned long total = 0;

    if (s->saw_sof) {
        return fail(s, "two frame headers in the picture");
    }
    if (len < 6 || p[0] != 8) {
        return fail(s, "only 8-bit JPEG pictures are supported");
    }
    s->height = seg16(p + 1);
    s->width = seg16(p + 3);
    s->ncomp = p[5];
    if (s->width == 0 || s->height == 0 || s->width * s->height > JPEG_MAX_PIXELS) {
        return fail(s, "the picture is too large or has no size");
    }
    if ((s->ncomp != 1 && s->ncomp != 3) || len < 6 + (size_t)s->ncomp * 3) {
        return fail(s, "only grey and colour JPEG pictures are supported");
    }
    s->progressive = progressive;
    s->hmax = s->vmax = 1;
    for (i = 0; i < s->ncomp; i++) {
        Comp *c = &s->comp[i];

        c->id = p[6 + i * 3];
        c->h = p[7 + i * 3] >> 4;
        c->v = p[7 + i * 3] & 15;
        c->tq = p[8 + i * 3];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) {
            return fail(s, "bad JPEG component description");
        }
        if (c->h > s->hmax) s->hmax = c->h;
        if (c->v > s->vmax) s->vmax = c->v;
    }
    if (s->ncomp == 1) {
        s->comp[0].h = s->comp[0].v = 1;                      /* a single component is never subsampled */
        s->hmax = s->vmax = 1;
    }
    s->mcux = (s->width + 8UL * (unsigned long)s->hmax - 1) / (8UL * (unsigned long)s->hmax);
    s->mcuy = (s->height + 8UL * (unsigned long)s->vmax - 1) / (8UL * (unsigned long)s->vmax);
    for (i = 0; i < s->ncomp; i++) {
        Comp *c = &s->comp[i];

        c->w = (s->width * (unsigned long)c->h + (unsigned long)s->hmax - 1) / (unsigned long)s->hmax;
        c->ht = (s->height * (unsigned long)c->v + (unsigned long)s->vmax - 1) / (unsigned long)s->vmax;
        c->bw = s->mcux * (unsigned long)c->h;
        c->bh = s->mcuy * (unsigned long)c->v;
        total += c->bw * c->bh * 64UL * 2UL;
        if (total > MAX_COEF_BYTES) {
            return fail(s, "the picture is too large");
        }
    }
    for (i = 0; i < s->ncomp; i++) {
        Comp *c = &s->comp[i];

        c->coef = (short *)calloc((size_t)(c->bw * c->bh * 64UL), sizeof(short));
        if (c->coef == NULL) {
            return fail(s, "out of memory");
        }
    }
    s->saw_sof = 1;
    return 0;
}

static int parse_dht(Dec *s, const unsigned char *p, size_t len)
{
    while (len >= 17) {
        int tc = p[0] >> 4, th = p[0] & 15, i, total = 0;
        HuffTable *t;

        if (tc > 1 || th > 3) {
            return fail(s, "bad Huffman table header");
        }
        t = tc == 0 ? &s->dc[th] : &s->ac[th];
        memset(t, 0, sizeof(*t));
        for (i = 1; i <= 16; i++) {
            t->bits[i] = p[i];
            total += p[i];
        }
        if (total > 256 || (size_t)total + 17 > len) {
            return fail(s, "bad Huffman table");
        }
        memcpy(t->vals, p + 17, (size_t)total);
        if (build_huff(s, t) != 0) {
            return -1;
        }
        p += 17 + total;
        len -= 17 + (size_t)total;
    }
    return len == 0 ? 0 : fail(s, "bad Huffman table");
}

static int parse_dqt(Dec *s, const unsigned char *p, size_t len)
{
    while (len >= 1) {
        int pq = p[0] >> 4, tq = p[0] & 15, i;
        size_t need = 1 + (pq ? 128 : 64);

        if (pq > 1 || tq > 3 || len < need) {
            return fail(s, "bad quantisation table");
        }
        for (i = 0; i < 64; i++) {
            s->qt[tq][ZIGZAG[i]] = pq ? (unsigned short)seg16(p + 1 + i * 2) : p[1 + i];
        }
        p += need;
        len -= need;
    }
    return 0;
}

static int parse_sos(Dec *s, const unsigned char *p, size_t len, Scan *sc)
{
    int i, j;

    if (!s->saw_sof) {
        return fail(s, "scan before the frame header");
    }
    if (len < 1 || p[0] < 1 || p[0] > s->ncomp || len < 1 + (size_t)p[0] * 2 + 3) {
        return fail(s, "bad scan header");
    }
    memset(sc, 0, sizeof(*sc));
    sc->ncomp = p[0];
    for (i = 0; i < sc->ncomp; i++) {
        int id = p[1 + i * 2], td = p[2 + i * 2] >> 4, ta = p[2 + i * 2] & 15;

        for (j = 0; j < s->ncomp && s->comp[j].id != id; j++) {
            ;
        }
        if (j == s->ncomp || td > 3 || ta > 3) {
            return fail(s, "bad scan header");
        }
        sc->idx[i] = j;
        s->comp[j].dc_tbl = td;
        s->comp[j].ac_tbl = ta;
    }
    sc->ss = p[1 + sc->ncomp * 2];
    sc->se = p[2 + sc->ncomp * 2];
    sc->ah = p[3 + sc->ncomp * 2] >> 4;
    sc->al = p[3 + sc->ncomp * 2] & 15;
    if (!s->progressive) {
        sc->ss = 0;
        sc->se = 63;
        sc->ah = sc->al = 0;
    } else if (sc->ss > sc->se || sc->se > 63 || sc->al > 13 || (sc->ss == 0 && sc->se != 0) ||
               (sc->ss != 0 && sc->ncomp != 1)) {
        return fail(s, "bad progressive scan");
    }
    for (i = 0; i < sc->ncomp; i++) {
        Comp *c = &s->comp[sc->idx[i]];
        int need_dc = !s->progressive || (sc->ss == 0 && sc->ah == 0);
        int need_ac = !s->progressive || sc->ss > 0;

        if ((need_dc && !s->dc[c->dc_tbl].present) || (need_ac && !s->ac[c->ac_tbl].present)) {
            return fail(s, "a Huffman table is missing");
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- IDCT, upsampling, colour */

#define CONST_BITS 13
#define PASS1_BITS 2
#define FIX_0_298631336 2446L
#define FIX_0_390180644 3196L
#define FIX_0_541196100 4433L
#define FIX_0_765366865 6270L
#define FIX_0_899976223 7373L
#define FIX_1_175875602 9633L
#define FIX_1_501321110 12299L
#define FIX_1_847759065 15137L
#define FIX_1_961570560 16069L
#define FIX_2_053119869 16819L
#define FIX_2_562915447 20995L
#define FIX_3_072711026 25172L
#define DESCALE(x, n) (((x) + (1L << ((n) - 1))) >> (n))

static unsigned char clamp8(long v)
{
    return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* One 8-point inverse transform (libjpeg's jidctint "islow"): in[0..7] with the given stride, out[0..7] with
 * the same stride, results scaled by 2^CONST_BITS before the final descale `shift`. */
static void idct_1d(const long in[8], long out[8], int shift)
{
    long tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13, z1, z2, z3, z4, z5;

    z2 = in[2];
    z3 = in[6];
    z1 = (z2 + z3) * FIX_0_541196100;
    tmp2 = z1 + z3 * (-FIX_1_847759065);
    tmp3 = z1 + z2 * FIX_0_765366865;
    z2 = in[0];
    z3 = in[4];
    tmp0 = (z2 + z3) * (1L << CONST_BITS);
    tmp1 = (z2 - z3) * (1L << CONST_BITS);
    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;
    tmp0 = in[7];
    tmp1 = in[5];
    tmp2 = in[3];
    tmp3 = in[1];
    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    z4 = tmp1 + tmp3;
    z5 = (z3 + z4) * FIX_1_175875602;
    tmp0 *= FIX_0_298631336;
    tmp1 *= FIX_2_053119869;
    tmp2 *= FIX_3_072711026;
    tmp3 *= FIX_1_501321110;
    z1 *= -FIX_0_899976223;
    z2 *= -FIX_2_562915447;
    z3 *= -FIX_1_961570560;
    z4 *= -FIX_0_390180644;
    z3 += z5;
    z4 += z5;
    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;
    out[0] = DESCALE(tmp10 + tmp3, shift);
    out[7] = DESCALE(tmp10 - tmp3, shift);
    out[1] = DESCALE(tmp11 + tmp2, shift);
    out[6] = DESCALE(tmp11 - tmp2, shift);
    out[2] = DESCALE(tmp12 + tmp1, shift);
    out[5] = DESCALE(tmp12 - tmp1, shift);
    out[3] = DESCALE(tmp13 + tmp0, shift);
    out[4] = DESCALE(tmp13 - tmp0, shift);
}

static void idct_block(const short *coef, const unsigned short *qt, unsigned char *out, size_t stride)
{
    long ws[64], col[8], res[8];
    int x, y, ac;

    for (x = 0; x < 8; x++) {
        ac = 0;
        for (y = 1; y < 8; y++) {
            if (coef[y * 8 + x] != 0) {
                ac = 1;
                break;
            }
        }
        if (!ac) {                                           /* a column with only a DC term */
            long dc = (long)coef[x] * qt[x] * (1L << PASS1_BITS);

            for (y = 0; y < 8; y++) ws[y * 8 + x] = dc;
            continue;
        }
        for (y = 0; y < 8; y++) col[y] = (long)coef[y * 8 + x] * qt[y * 8 + x];
        idct_1d(col, res, CONST_BITS - PASS1_BITS);
        for (y = 0; y < 8; y++) ws[y * 8 + x] = res[y];
    }
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) col[x] = ws[y * 8 + x];
        idct_1d(col, res, CONST_BITS + PASS1_BITS + 3);
        for (x = 0; x < 8; x++) out[y * stride + (size_t)x] = clamp8(res[x] + 128);
    }
}

/* One component's samples at the full image size. `src` has stride bw*8. */
static int upsample(const Dec *s, const Comp *c, unsigned char *out)
{
    unsigned long W = s->width, H = s->height, x, y;
    size_t stride = (size_t)(c->bw * 8);
    int hf = s->hmax / c->h, vf = s->vmax / c->v;
    const unsigned char *src = c->plane;

    if (s->hmax % c->h != 0 || s->vmax % c->v != 0) {
        return -1;
    }
    if (hf == 1 && vf == 1) {
        for (y = 0; y < H; y++) memcpy(out + y * W, src + y * stride, (size_t)W);
        return 0;
    }
    if (hf == 2 && vf == 1) {                                /* libjpeg's h2v1 "fancy" (triangle) upsampling */
        unsigned char *row = (unsigned char *)malloc((size_t)(c->w * 2 + 2));

        if (row == NULL) return -1;
        for (y = 0; y < H; y++) {
            const unsigned char *in = src + y * stride;

            if (c->w == 1) {
                row[0] = row[1] = in[0];
            } else {
                row[0] = in[0];
                row[1] = (unsigned char)((in[0] * 3 + in[1] + 2) >> 2);
                for (x = 1; x + 1 < c->w; x++) {
                    row[2 * x] = (unsigned char)((in[x] * 3 + in[x - 1] + 1) >> 2);
                    row[2 * x + 1] = (unsigned char)((in[x] * 3 + in[x + 1] + 2) >> 2);
                }
                row[2 * (c->w - 1)] = (unsigned char)((in[c->w - 1] * 3 + in[c->w - 2] + 1) >> 2);
                row[2 * (c->w - 1) + 1] = in[c->w - 1];
            }
            memcpy(out + y * W, row, (size_t)W);
        }
        free(row);
        return 0;
    }
    if (hf == 2 && vf == 2) {                                /* h2v2 fancy: 3/4 the near input row, 1/4 the far one */
        unsigned char *row = (unsigned char *)malloc((size_t)(c->w * 2 + 2));

        if (row == NULL) return -1;
        for (y = 0; y < H; y++) {
            unsigned long iy = y / 2, ny;
            const unsigned char *in0 = src + iy * stride, *in1;
            long this_sum, last_sum, next_sum;

            if (y % 2 == 0) {
                ny = iy == 0 ? 0 : iy - 1;
            } else {
                ny = iy + 1 >= c->ht ? c->ht - 1 : iy + 1;
            }
            in1 = src + ny * stride;
            if (c->w == 1) {
                row[0] = row[1] = (unsigned char)((in0[0] * 3 + in1[0] + 2) >> 2);   /* (4*sum+8)>>4 and (4*sum+7)>>4 */
                row[0] = (unsigned char)(((in0[0] * 3 + in1[0]) * 4 + 8) >> 4);
                row[1] = (unsigned char)(((in0[0] * 3 + in1[0]) * 4 + 7) >> 4);
            } else {
                this_sum = in0[0] * 3 + in1[0];
                next_sum = in0[1] * 3 + in1[1];
                row[0] = (unsigned char)((this_sum * 4 + 8) >> 4);
                row[1] = (unsigned char)((this_sum * 3 + next_sum + 7) >> 4);
                last_sum = this_sum;
                this_sum = next_sum;
                for (x = 1; x + 1 < c->w; x++) {
                    next_sum = in0[x + 1] * 3 + in1[x + 1];
                    row[2 * x] = (unsigned char)((this_sum * 3 + last_sum + 8) >> 4);
                    row[2 * x + 1] = (unsigned char)((this_sum * 3 + next_sum + 7) >> 4);
                    last_sum = this_sum;
                    this_sum = next_sum;
                }
                row[2 * (c->w - 1)] = (unsigned char)((this_sum * 3 + last_sum + 8) >> 4);
                row[2 * (c->w - 1) + 1] = (unsigned char)((this_sum * 4 + 7) >> 4);
            }
            memcpy(out + y * W, row, (size_t)W);
        }
        free(row);
        return 0;
    }
    for (y = 0; y < H; y++) {                                /* any other ratio: repeat the nearest sample */
        const unsigned char *in = src + (y / (unsigned long)vf) * stride;

        for (x = 0; x < W; x++) out[y * W + x] = in[x / (unsigned long)hf];
    }
    return 0;
}

/* ---------------------------------------------------------------- the whole picture */

static void free_dec(Dec *s)
{
    int i;

    for (i = 0; i < MAX_COMP; i++) {
        free(s->comp[i].coef);
        free(s->comp[i].plane);
    }
}

#define SCALEBITS 16
#define ONE_HALF (1L << (SCALEBITS - 1))
#define FIX(x) ((long)((x) * (1L << SCALEBITS) + 0.5))

int jpeg_decode(const unsigned char *data, size_t len, PngImage *img, const char **err)
{
    Dec *s;
    Scan sc;
    size_t pos;
    int m, i, rc = -1;
    unsigned char *full[MAX_COMP];
    unsigned long x, y, W, H;
    int rgb_direct;

    memset(img, 0, sizeof(*img));
    memset(full, 0, sizeof(full));
    *err = "not a JPEG picture";
    if (len < 4 || data[0] != 0xff || data[1] != 0xd8) {
        return -1;
    }
    s = (Dec *)calloc(1, sizeof(Dec));
    if (s == NULL) {
        *err = "out of memory";
        return -1;
    }
    s->d = data;
    s->n = len;
    s->adobe_transform = -1;
    pos = 2;
    m = 0;
    for (;;) {
        if (m == 0) {                                        /* find the next marker */
            while (pos < len && data[pos] != 0xff) pos++;
            while (pos < len && data[pos] == 0xff) pos++;
            if (pos >= len) {
                fail(s, "the picture is cut short");
                goto done;
            }
            m = data[pos++];
        }
        if (m == 0xd9) {
            break;
        }
        if (m == 0xd8 || m == 0x01 || (m >= 0xd0 && m <= 0xd7) || m == 0) {
            m = 0;                                           /* standalone markers carry no data */
            continue;
        }
        {
            size_t seglen, end;
            const unsigned char *p;
            int cur = m;

            if (pos + 2 > len) {
                fail(s, "the picture is cut short");
                goto done;
            }
            seglen = seg16(data + pos);
            if (seglen < 2 || pos + seglen > len) {
                fail(s, "the picture is cut short");
                goto done;
            }
            p = data + pos + 2;
            end = pos + seglen;
            m = 0;
            if (cur == 0xc0 || cur == 0xc1 || cur == 0xc2) {
                if (parse_sof(s, p, seglen - 2, cur == 0xc2) != 0) goto done;
            } else if (cur == 0xc3 || (cur >= 0xc5 && cur <= 0xc7) || (cur >= 0xc9 && cur <= 0xcf)) {
                fail(s, "this kind of JPEG (arithmetic, lossless or hierarchical) is not supported");
                goto done;
            } else if (cur == 0xc4) {
                if (parse_dht(s, p, seglen - 2) != 0) goto done;
            } else if (cur == 0xdb) {
                if (parse_dqt(s, p, seglen - 2) != 0) goto done;
            } else if (cur == 0xdd) {
                if (seglen != 4) {
                    fail(s, "bad restart interval");
                    goto done;
                }
                s->restart_interval = (int)seg16(p);
            } else if (cur == 0xee) {
                if (seglen >= 2 + 12 && memcmp(p, "Adobe", 5) == 0) {
                    s->adobe_transform = p[11];
                }
            } else if (cur == 0xda) {
                if (parse_sos(s, p, seglen - 2, &sc) != 0) goto done;
                s->pos = end;
                s->saw_sos = 1;
                if (decode_scan(s, &sc) != 0) goto done;
                pos = s->pos;
                m = s->marker;                               /* the marker that ended the entropy-coded data */
                s->marker = 0;
                continue;
            }
            pos = end;
        }
    }
    if (!s->saw_sof || !s->saw_sos) {
        fail(s, "the picture has no image data");
        goto done;
    }

    /* dequantise and transform every block */
    for (i = 0; i < s->ncomp; i++) {
        Comp *c = &s->comp[i];
        unsigned long bx, by, stride = c->bw * 8;

        c->plane = (unsigned char *)malloc((size_t)(c->bw * c->bh * 64UL));
        if (c->plane == NULL) {
            fail(s, "out of memory");
            goto done;
        }
        for (by = 0; by < c->bh; by++) {
            for (bx = 0; bx < c->bw; bx++) {
                idct_block(c->coef + (by * c->bw + bx) * 64, s->qt[c->tq], c->plane + by * 8 * stride + bx * 8, (size_t)stride);
            }
        }
        free(c->coef);
        c->coef = NULL;
    }
    W = s->width;
    H = s->height;
    for (i = 0; i < s->ncomp; i++) {
        full[i] = (unsigned char *)malloc((size_t)(W * H));
        if (full[i] == NULL || upsample(s, &s->comp[i], full[i]) != 0) {
            fail(s, "unsupported chroma subsampling");
            goto done;
        }
    }
    img->rgba = (unsigned char *)malloc((size_t)(W * H * 4UL));
    if (img->rgba == NULL) {
        fail(s, "out of memory");
        goto done;
    }
    rgb_direct = s->ncomp == 3 && (s->adobe_transform == 0 ||
                 (s->comp[0].id == 'R' && s->comp[1].id == 'G' && s->comp[2].id == 'B'));
    for (y = 0; y < H; y++) {
        unsigned char *o = img->rgba + y * W * 4;

        for (x = 0; x < W; x++, o += 4) {
            unsigned long k = y * W + x;

            if (s->ncomp == 1) {
                o[0] = o[1] = o[2] = full[0][k];
            } else if (rgb_direct) {
                o[0] = full[0][k];
                o[1] = full[1][k];
                o[2] = full[2][k];
            } else {
                long yy = full[0][k], cb = (long)full[1][k] - 128, cr = (long)full[2][k] - 128;

                o[0] = clamp8(yy + ((FIX(1.40200) * cr + ONE_HALF) >> SCALEBITS));
                o[1] = clamp8(yy + ((-FIX(0.34414) * cb - FIX(0.71414) * cr + ONE_HALF) >> SCALEBITS));
                o[2] = clamp8(yy + ((FIX(1.77200) * cb + ONE_HALF) >> SCALEBITS));
            }
            o[3] = 255;
        }
    }
    img->width = W;
    img->height = H;
    *err = "ok";
    rc = 0;
done:
    if (rc != 0) {
        *err = s->err ? s->err : "the picture is damaged";
        free(img->rgba);
        memset(img, 0, sizeof(*img));
    }
    for (i = 0; i < MAX_COMP; i++) free(full[i]);
    free_dec(s);
    free(s);
    return rc;
}
