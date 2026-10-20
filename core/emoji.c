/* Unicode emoji as pictures: see emoji.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emoji.h"
#include "img.h"
#include "inflate.h"

#define MAX_SEQ 40

struct EmojiPack {
    FILE *f;
    int size, count;
    unsigned char *index;                /* the index, as in the file */
    size_t index_len;
    unsigned long *entry;                /* offset of each entry within `index` */
};

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

EmojiPack *emoji_pack_open(const char *path)
{
    EmojiPack *p;
    unsigned char hdr[12];
    int i;
    unsigned long at = 0;

    p = (EmojiPack *)calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->f = fopen(path, "rb");
    if (p->f == NULL || fread(hdr, 1, 12, p->f) != 12 || memcmp(hdr, "KEMJ", 4) != 0 || hdr[4] != 1) {
        emoji_pack_close(p);
        return NULL;
    }
    p->size = hdr[5];
    p->count = (hdr[6] << 8) | hdr[7];
    p->index_len = (size_t)be32(hdr + 8);
    if (p->size < 1 || p->size > 64 || p->count < 1 || p->index_len < (size_t)p->count * 7 || p->index_len > 1000000UL) {
        emoji_pack_close(p);
        return NULL;
    }
    p->index = (unsigned char *)malloc(p->index_len);
    p->entry = (unsigned long *)malloc((size_t)p->count * sizeof(unsigned long));
    if (p->index == NULL || p->entry == NULL || fread(p->index, 1, p->index_len, p->f) != p->index_len) {
        emoji_pack_close(p);
        return NULL;
    }
    for (i = 0; i < p->count; i++) {                     /* where each entry starts; every entry must fit */
        p->entry[i] = at;
        if (at >= p->index_len || at + 1 + p->index[at] + 6 > p->index_len || p->index[at] == 0) {
            emoji_pack_close(p);
            return NULL;
        }
        at += 1 + p->index[at] + 6;
    }
    if (at != p->index_len) {
        emoji_pack_close(p);
        return NULL;
    }
    return p;
}

void emoji_pack_close(EmojiPack *p)
{
    if (p == NULL) {
        return;
    }
    if (p->f != NULL) {
        fclose(p->f);
    }
    free(p->index);
    free(p->entry);
    free(p);
}

int emoji_pack_count(const EmojiPack *p) { return p->count; }
int emoji_pack_size(const EmojiPack *p) { return p->size; }

const char *emoji_pack_key(const EmojiPack *p, int i, size_t *len)
{
    if (i < 0 || i >= p->count) {
        return NULL;
    }
    *len = p->index[p->entry[i]];
    return (const char *)p->index + p->entry[i] + 1;
}

/* index of the entry for key[0..n), or -1 */
static int find(const EmojiPack *p, const char *key, size_t n)
{
    int lo = 0, hi = p->count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2, c;
        size_t kl = p->index[p->entry[mid]], m = kl < n ? kl : n;

        c = memcmp(p->index + p->entry[mid] + 1, key, m);
        if (c == 0) {
            c = kl < n ? -1 : (kl > n ? 1 : 0);
        }
        if (c == 0) {
            return mid;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

int emoji_pack_has(const EmojiPack *p, const char *key)
{
    return find(p, key, strlen(key)) >= 0;
}

int emoji_pack_rgba(EmojiPack *p, const char *key, unsigned char *out)
{
    int i = find(p, key, strlen(key));
    const unsigned char *e;
    unsigned char *z;
    unsigned long off;
    unsigned len;
    const char *err;
    int rc;

    if (i < 0) {
        return -1;
    }
    e = p->index + p->entry[i];
    off = be32(e + 1 + e[0]);
    len = ((unsigned)e[1 + e[0] + 4] << 8) | e[1 + e[0] + 5];
    z = (unsigned char *)malloc(len > 0 ? len : 1);
    if (z == NULL) {
        return -1;
    }
    if (fseek(p->f, (long)off, SEEK_SET) != 0 || fread(z, 1, len, p->f) != len) {
        free(z);
        return -1;
    }
    rc = inflate_zlib(z, len, out, (size_t)p->size * (size_t)p->size * 4, &err);
    free(z);
    return rc;
}

unsigned char *emoji_pack_tiff(EmojiPack *p, const char *key, size_t *len)
{
    size_t px = (size_t)p->size * (size_t)p->size, i;
    unsigned char *rgba = (unsigned char *)malloc(px * 4), *rgb = (unsigned char *)malloc(px * 3), *tiff = NULL;

    if (rgba != NULL && rgb != NULL && emoji_pack_rgba(p, key, rgba) == 0) {
        for (i = 0; i < px; i++) {                       /* over white */
            unsigned long a = rgba[i * 4 + 3];

            rgb[i * 3] = (unsigned char)((rgba[i * 4] * a + 255UL * (255UL - a)) / 255UL);
            rgb[i * 3 + 1] = (unsigned char)((rgba[i * 4 + 1] * a + 255UL * (255UL - a)) / 255UL);
            rgb[i * 3 + 2] = (unsigned char)((rgba[i * 4 + 2] * a + 255UL * (255UL - a)) / 255UL);
        }
        tiff = tiff_encode_rgb(rgb, (unsigned long)p->size, (unsigned long)p->size, len);
    }
    free(rgba);
    free(rgb);
    return tiff;
}

/* ---------------------------------------------------------------- finding emoji in text */

/* the code point at s[i], or -1 for bytes that are not well-formed UTF-8; *adv is how many bytes it took */
static long decode(const unsigned char *s, size_t n, size_t i, size_t *adv)
{
    unsigned c = s[i];
    long cp;
    size_t need, k;

    if (c < 0x80) {
        *adv = 1;
        return c;
    }
    if (c >= 0xc2 && c < 0xe0) {
        need = 1;
        cp = c & 0x1f;
    } else if (c >= 0xe0 && c < 0xf0) {
        need = 2;
        cp = c & 0x0f;
    } else if (c >= 0xf0 && c < 0xf5) {
        need = 3;
        cp = c & 0x07;
    } else {
        *adv = 1;
        return -1;
    }
    if (i + need >= n) {
        *adv = 1;
        return -1;
    }
    for (k = 1; k <= need; k++) {
        if ((s[i + k] & 0xc0) != 0x80) {
            *adv = 1;
            return -1;
        }
        cp = (cp << 6) | (s[i + k] & 0x3f);
    }
    if ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000L || cp > 0x10ffffL))) {
        *adv = 1;
        return -1;
    }
    *adv = need + 1;
    return cp;
}

static int is_modifier(long cp) { return cp >= 0x1f3fb && cp <= 0x1f3ff; }
static int is_ri(long cp) { return cp >= 0x1f1e6 && cp <= 0x1f1ff; }

/* the pack's key for cps[0..n): hex code points joined by '-', FE0F dropped unless there is a ZWJ (as Twemoji names its
 * files) -- or, with keep_fe0f, kept always. Returns the key's length. */
static size_t make_key(const long *cps, int n, int keep_fe0f, char *key)
{
    int i, zwj = 0;
    size_t o = 0;

    for (i = 0; i < n; i++) {
        if (cps[i] == 0x200d) zwj = 1;
    }
    for (i = 0; i < n; i++) {
        char hexbuf[12];
        int hn = 0, d;
        long v = cps[i];

        if (v == 0xfe0f && !zwj && !keep_fe0f) {
            continue;
        }
        if (o > 0) {
            key[o++] = '-';
        }
        if (v == 0) {
            hexbuf[hn++] = '0';
        }
        while (v > 0) {
            d = (int)(v & 15);
            hexbuf[hn++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
            v >>= 4;
        }
        while (hn > 0) {
            key[o++] = hexbuf[--hn];
        }
    }
    key[o] = '\0';
    return o;
}

int emoji_next(const EmojiPack *p, const char *text, size_t len, size_t from, size_t *start, size_t *end, char key[EMOJI_KEY_MAX])
{
    const unsigned char *s = (const unsigned char *)text;
    size_t i = from;

    while (i < len) {
        size_t adv, pos[MAX_SEQ + 1];
        long cps[MAX_SEQ];
        int n = 0, extent, L;
        long c0;
        size_t j;

        c0 = decode(s, len, i, &adv);
        /* cheap rejections: ASCII other than the keycap bases, and anything below the first emoji (U+00A9) */
        if (c0 < 0 || (c0 < 0xa9 && !((c0 >= '0' && c0 <= '9') || c0 == '#' || c0 == '*')) ||
            (c0 >= 0xaa && c0 < 0x2000 && c0 != 0xae)) {
            i += adv;
            continue;
        }
        /* gather the code points that could belong to one sequence */
        j = i;
        pos[0] = i;
        while (j < len && n < MAX_SEQ) {
            size_t a;
            long cp = decode(s, len, j, &a);

            if (cp < 0) {
                break;
            }
            cps[n++] = cp;
            j += a;
            pos[n] = j;
        }
        /* how far the sequence reaches */
        extent = 1;
        if (c0 < 0x80) {                                           /* a keycap: base, optional FE0F, 20E3 */
            if (n >= 2 && cps[1] == 0x20e3) extent = 2;
            else if (n >= 3 && cps[1] == 0xfe0f && cps[2] == 0x20e3) extent = 3;
            else {
                i += adv;
                continue;
            }
        } else if (is_ri(c0)) {
            extent = (n >= 2 && is_ri(cps[1])) ? 2 : 1;
        } else {
            int k = 1;

            if (c0 <= 0x21ff) {                                    /* symbols the fonts have: only with U+FE0F */
                if (!(n >= 2 && cps[1] == 0xfe0f)) {
                    i += adv;
                    continue;
                }
            }
            if (c0 == 0x1f3f4 && n >= 3 && cps[1] >= 0xe0020 && cps[1] <= 0xe007e) {      /* a flag from tag characters */
                k = 1;
                while (k < n && cps[k] >= 0xe0020 && cps[k] <= 0xe007e) k++;
                if (k < n && cps[k] == 0xe007f) k++;
            } else {
                for (;;) {
                    if (k < n && cps[k] == 0xfe0f) k++;
                    if (k < n && is_modifier(cps[k])) k++;
                    if (k < n && cps[k] == 0xfe0f) k++;
                    if (k + 1 < n && cps[k] == 0x200d && cps[k + 1] >= 0x2190) {
                        k += 2;                                    /* ZWJ and the next element */
                        continue;
                    }
                    break;
                }
            }
            extent = k;
        }
        /* the longest prefix the pack has a picture for */
        for (L = extent; L >= 1; L--) {
            size_t kl;

            if (cps[L - 1] == 0x200d) {
                continue;                                          /* never end on a joiner */
            }
            kl = make_key(cps, L, 0, key);
            if (find(p, key, kl) >= 0) {
                *start = pos[0];
                *end = pos[L];
                return 1;
            }
            kl = make_key(cps, L, 1, key);                         /* some files keep their FE0F */
            if (find(p, key, kl) >= 0) {
                *start = pos[0];
                *end = pos[L];
                return 1;
            }
        }
        i += adv;
    }
    return 0;
}
