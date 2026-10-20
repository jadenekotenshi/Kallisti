/* zlib/DEFLATE decompression: see inflate.h.  A table-free canonical-Huffman decoder in the manner of zlib's
 * "puff": small and easy to check, a little slow, which is fine for a picture or two. */
#include <string.h>
#include "inflate.h"

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    const unsigned char *in;
    size_t n, pos;
    unsigned long bitbuf;
    int bitcnt;
    unsigned char *out;
    size_t outlen, outpos;
    const char *err;
} Inf;

#define FASTBITS 10

typedef struct {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
    unsigned short fast[1 << FASTBITS];      /* symbol | (length << 9) for codes of up to FASTBITS bits, else 0 */
} Huff;

static int bits(Inf *s, int need)
{
    unsigned long v = s->bitbuf;

    while (s->bitcnt < need) {
        if (s->pos >= s->n) {
            s->err = "the compressed data ends too soon";
            return -1;
        }
        v |= (unsigned long)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1UL << need) - 1));
}

static int decode(Inf *s, const Huff *h)
{
    int len, code = 0, first = 0, count, index = 0, b;
    unsigned long v;
    unsigned e;

    /* the common case: a code of up to FASTBITS bits, found by looking at the next FASTBITS bits at once */
    while (s->bitcnt < FASTBITS && s->pos < s->n) {
        s->bitbuf |= (unsigned long)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    v = s->bitbuf & ((1UL << FASTBITS) - 1);
    e = h->fast[v];
    if (e != 0 && (int)(e >> 9) <= s->bitcnt) {
        s->bitbuf >>= (e >> 9);
        s->bitcnt -= (int)(e >> 9);
        return (int)(e & 0x1ff);
    }

    for (len = 1; len <= MAXBITS; len++) {
        b = bits(s, 1);
        if (b < 0) {
            return -1;
        }
        code |= b;
        count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = "bad Huffman code";
    return -1;
}

/* Builds the decoding tables for `n` code lengths; returns 0 for a complete code, >0 incomplete, <0 over-subscribed. */
static int construct(Huff *h, const short *length, int n)
{
    int symbol, len, left;
    short offs[MAXBITS + 1];

    for (len = 0; len <= MAXBITS; len++) {
        h->count[len] = 0;
    }
    for (symbol = 0; symbol < n; symbol++) {
        h->count[length[symbol]]++;
    }
    if (h->count[0] == n) {
        return 0;
    }
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return left;
        }
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) {
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    }
    for (symbol = 0; symbol < n; symbol++) {
        if (length[symbol] != 0) {
            h->symbol[offs[length[symbol]]++] = (short)symbol;
        }
    }
    /* the fast table: walk the canonical codes in order, as decode() does, and fill every slot a short code owns
     * (DEFLATE sends code bits most-significant first, so the table is indexed by the bit-reversed code) */
    {
        int code = 0, index = 0, k;

        for (k = 0; k < (1 << FASTBITS); k++) {
            h->fast[k] = 0;
        }
        for (len = 1; len <= FASTBITS; len++) {
            int c;

            for (c = 0; c < h->count[len]; c++, code++, index++) {
                int rev = 0, b, fill;

                for (b = 0; b < len; b++) {
                    if (code & (1 << b)) {
                        rev |= 1 << (len - 1 - b);
                    }
                }
                for (fill = rev; fill < (1 << FASTBITS); fill += 1 << len) {
                    h->fast[fill] = (unsigned short)(h->symbol[index] | (len << 9));
                }
            }
            code <<= 1;
        }
    }
    return left;
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                                 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
                                 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(Inf *s, const Huff *lencode, const Huff *distcode)
{
    int symbol, len, dist;
    size_t i;

    for (;;) {
        symbol = decode(s, lencode);
        if (symbol < 0) {
            return -1;
        }
        if (symbol < 256) {
            if (s->outpos >= s->outlen) {
                s->err = "the picture's data is larger than its header says";
                return -1;
            }
            s->out[s->outpos++] = (unsigned char)symbol;
        } else if (symbol == 256) {
            return 0;
        } else {
            symbol -= 257;
            if (symbol >= 29) {
                s->err = "bad length code";
                return -1;
            }
            len = bits(s, lext[symbol]);
            if (len < 0) {
                return -1;
            }
            len += lbase[symbol];
            symbol = decode(s, distcode);
            if (symbol < 0) {
                return -1;
            }
            if (symbol >= MAXDCODES) {
                s->err = "bad distance code";
                return -1;
            }
            dist = bits(s, dext[symbol]);
            if (dist < 0) {
                return -1;
            }
            dist += dbase[symbol];
            if ((size_t)dist > s->outpos) {
                s->err = "a back-reference points before the start";
                return -1;
            }
            if (s->outpos + (size_t)len > s->outlen) {
                s->err = "the picture's data is larger than its header says";
                return -1;
            }
            for (i = 0; i < (size_t)len; i++) {
                s->out[s->outpos + i] = s->out[s->outpos + i - (size_t)dist];
            }
            s->outpos += (size_t)len;
        }
    }
}

static int stored(Inf *s)
{
    unsigned len, nlen;

    s->pos -= (size_t)(s->bitcnt / 8);        /* whole bytes the fast decoder read ahead go back */
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->pos + 4 > s->n) {
        s->err = "the compressed data ends too soon";
        return -1;
    }
    len = s->in[s->pos] | (s->in[s->pos + 1] << 8);
    nlen = s->in[s->pos + 2] | (s->in[s->pos + 3] << 8);
    s->pos += 4;
    if (len != (~nlen & 0xffffU)) {
        s->err = "bad stored block";
        return -1;
    }
    if (s->pos + len > s->n) {
        s->err = "the compressed data ends too soon";
        return -1;
    }
    if (s->outpos + len > s->outlen) {
        s->err = "the picture's data is larger than its header says";
        return -1;
    }
    memcpy(s->out + s->outpos, s->in + s->pos, len);
    s->pos += len;
    s->outpos += len;
    return 0;
}

static int fixed(Inf *s)
{
    static Huff lencode, distcode;
    static int built;

    if (!built) {
        short lengths[FIXLCODES];
        int symbol;

        for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(Inf *s)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen, ndist, ncode, index, err, symbol, len, rep;
    short lengths[MAXLCODES + MAXDCODES];
    Huff lencode, distcode;

    nlen = bits(s, 5);
    ndist = bits(s, 5);
    ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) {
        return -1;
    }
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) {
        s->err = "bad Huffman table sizes";
        return -1;
    }
    for (index = 0; index < ncode; index++) {
        int v = bits(s, 3);

        if (v < 0) {
            return -1;
        }
        lengths[order[index]] = (short)v;
    }
    for (; index < 19; index++) {
        lengths[order[index]] = 0;
    }
    err = construct(&lencode, lengths, 19);
    if (err != 0) {
        s->err = "bad Huffman code lengths";
        return -1;
    }
    index = 0;
    while (index < nlen + ndist) {
        symbol = decode(s, &lencode);
        if (symbol < 0) {
            return -1;
        }
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            len = 0;
            if (symbol == 16) {
                if (index == 0) {
                    s->err = "bad Huffman code lengths";
                    return -1;
                }
                len = lengths[index - 1];
                rep = bits(s, 2);
                rep = rep < 0 ? rep : 3 + rep;
            } else if (symbol == 17) {
                rep = bits(s, 3);
                rep = rep < 0 ? rep : 3 + rep;
            } else {
                rep = bits(s, 7);
                rep = rep < 0 ? rep : 11 + rep;
            }
            if (rep < 0) {
                return -1;
            }
            if (index + rep > nlen + ndist) {
                s->err = "bad Huffman code lengths";
                return -1;
            }
            while (rep--) {
                lengths[index++] = (short)len;
            }
        }
    }
    if (lengths[256] == 0) {
        s->err = "bad Huffman code lengths";
        return -1;
    }
    err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) {
        s->err = "bad Huffman code lengths";
        return -1;
    }
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) {
        s->err = "bad Huffman code lengths";
        return -1;
    }
    return codes(s, &lencode, &distcode);
}

unsigned long adler32_sum(const unsigned char *p, size_t n)
{
    unsigned long a = 1, b = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        a += p[i];
        if (a >= 65521UL) {
            a -= 65521UL;
        }
        b += a;
        if (b >= 65521UL) {
            b -= 65521UL;
        }
    }
    return (b << 16) | a;
}

unsigned long crc32_sum(unsigned long crc, const unsigned char *p, size_t n)
{
    static unsigned long table[256];
    static int built;
    size_t i;

    if (!built) {
        unsigned long c;
        int k, j;

        for (k = 0; k < 256; k++) {
            c = (unsigned long)k;
            for (j = 0; j < 8; j++) {
                c = (c & 1) ? (0xedb88320UL ^ (c >> 1)) : (c >> 1);
            }
            table[k] = c;
        }
        built = 1;
    }
    crc ^= 0xffffffffUL;
    for (i = 0; i < n; i++) {
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    }
    return (crc ^ 0xffffffffUL) & 0xffffffffUL;
}

int inflate_zlib(const unsigned char *in, size_t n, unsigned char *out, size_t outlen, const char **err)
{
    Inf s;
    int last, type;
    unsigned long want;

    memset(&s, 0, sizeof(s));
    s.in = in;
    s.n = n;
    s.out = out;
    s.outlen = outlen;
    *err = "ok";
    if (n < 6 || (in[0] & 0x0f) != 8 || (in[0] >> 4) > 7 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) {
        *err = "not a zlib stream";
        return -1;
    }
    s.pos = 2;
    do {
        last = bits(&s, 1);
        type = last < 0 ? -1 : bits(&s, 2);
        if (type < 0) {
            *err = s.err;
            return -1;
        }
        if (type == 0) {
            if (stored(&s) != 0) {
                *err = s.err;
                return -1;
            }
        } else if (type == 1) {
            if (fixed(&s) != 0) {
                *err = s.err;
                return -1;
            }
        } else if (type == 2) {
            if (dynamic(&s) != 0) {
                *err = s.err;
                return -1;
            }
        } else {
            *err = "bad block type";
            return -1;
        }
    } while (!last);
    if (s.outpos != outlen) {
        *err = "the picture's data is smaller than its header says";
        return -1;
    }
    /* the Adler-32 follows on a byte boundary, after giving back what the decoder read ahead */
    s.pos -= (size_t)(s.bitcnt / 8);
    if (s.pos + 4 > n) {
        *err = "the compressed data ends too soon";
        return -1;
    }
    want = ((unsigned long)in[s.pos] << 24) | ((unsigned long)in[s.pos + 1] << 16) | ((unsigned long)in[s.pos + 2] << 8) |
           (unsigned long)in[s.pos + 3];
    if (adler32_sum(out, outlen) != want) {
        *err = "the picture's data is damaged (checksum)";
        return -1;
    }
    return 0;
}
