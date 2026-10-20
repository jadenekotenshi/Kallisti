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
    unsigned char *names;                /* the names block, as in the file */
    size_t names_len;
    int nnames;
    unsigned long *nentry;               /* offset of each name entry within `names` */
    int nprimary;                        /* the names to show for an emoji, sorted by that emoji's key: */
    int *primary;                        /*   indices into nentry */
    char *keys;                          /*   and their keys, EMOJI_KEY_MAX bytes each, in the same order */
};

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static size_t make_key(const long *cps, int n, int keep_fe0f, char *key);
static long decode(const unsigned char *s, size_t n, size_t i, size_t *adv);
static int load_names(EmojiPack *p, unsigned long names_off, size_t names_len);

EmojiPack *emoji_pack_open(const char *path)
{
    EmojiPack *p;
    unsigned char hdr[20];
    int i;
    unsigned long at = 0;

    p = (EmojiPack *)calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->f = fopen(path, "rb");
    if (p->f == NULL || fread(hdr, 1, 20, p->f) != 20 || memcmp(hdr, "KEMJ", 4) != 0 || hdr[4] != 2) {
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
    if (at != p->index_len || load_names(p, be32(hdr + 12), (size_t)be32(hdr + 16)) != 0) {
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
    free(p->names);
    free(p->nentry);
    free(p->primary);
    free(p->keys);
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

/* ---------------------------------------------------------------- shortcode names */

/* the key of the emoji in u[0..n), derived as the pack's keys are (FE0F dropped unless there is a ZWJ) */
static size_t key_of_utf8(const unsigned char *u, size_t n, char *key)
{
    long cps[MAX_SEQ];
    int nc = 0;
    size_t i = 0, adv;

    while (i < n && nc < MAX_SEQ) {
        long cp = decode(u, n, i, &adv);

        if (cp < 0) {
            break;
        }
        cps[nc++] = cp;
        i += adv;
    }
    return make_key(cps, nc, 0, key);
}

static int load_names(EmojiPack *p, unsigned long names_off, size_t names_len)
{
    size_t at = 2;
    int n, np = 0, k;
    int *order = NULL;
    char *tmpkeys = NULL;
    int j = 0, a, b, rc = -1;

    if (names_len < 2 || names_len > 4000000UL) {
        return -1;
    }
    p->names = (unsigned char *)malloc(names_len);
    if (p->names == NULL || fseek(p->f, (long)names_off, SEEK_SET) != 0 || fread(p->names, 1, names_len, p->f) != names_len) {
        return -1;
    }
    p->names_len = names_len;
    n = (p->names[0] << 8) | p->names[1];
    p->nnames = n;
    p->nentry = (unsigned long *)malloc((size_t)(n > 0 ? n : 1) * sizeof(unsigned long));
    if (p->nentry == NULL) {
        return -1;
    }
    for (k = 0; k < n; k++) {                            /* walk the entries, checking each fits */
        size_t nl, ul;

        p->nentry[k] = at;
        if (at + 1 > names_len) return -1;
        nl = p->names[at];
        if (nl == 0 || at + 1 + nl + 1 > names_len) return -1;
        ul = p->names[at + 1 + nl];
        if (ul == 0 || at + 1 + nl + 1 + ul + 1 > names_len) return -1;
        if (p->names[at + 1 + nl + 1 + ul]) np++;
        at += 1 + nl + 1 + ul + 1;
    }
    if (at != names_len) {
        return -1;
    }
    /* the names to show for each emoji, findable by key: gather (key, entry) pairs and sort by key */
    p->primary = (int *)malloc((size_t)(np > 0 ? np : 1) * sizeof(int));
    p->keys = (char *)malloc((size_t)(np > 0 ? np : 1) * EMOJI_KEY_MAX);
    order = (int *)malloc((size_t)(np > 0 ? np : 1) * sizeof(int));
    tmpkeys = (char *)malloc((size_t)(np > 0 ? np : 1) * EMOJI_KEY_MAX);
    if (p->primary == NULL || p->keys == NULL || order == NULL || tmpkeys == NULL) {
        goto done;
    }
    for (k = 0; k < n; k++) {
        const unsigned char *e = p->names + p->nentry[k];
        size_t nl = e[0], ul = e[1 + nl];

        if (e[1 + nl + 1 + ul]) {
            key_of_utf8(e + 1 + nl + 1, ul, tmpkeys + (size_t)j * EMOJI_KEY_MAX);
            order[j] = k;
            j++;
        }
    }
    for (a = 1; a < np; a++) {                           /* insertion sort by key: about 2000 entries, once at start-up */
        int o = order[a];
        char kbuf[EMOJI_KEY_MAX];

        memcpy(kbuf, tmpkeys + (size_t)a * EMOJI_KEY_MAX, EMOJI_KEY_MAX);
        b = a - 1;
        while (b >= 0 && strcmp(tmpkeys + (size_t)b * EMOJI_KEY_MAX, kbuf) > 0) {
            memcpy(tmpkeys + (size_t)(b + 1) * EMOJI_KEY_MAX, tmpkeys + (size_t)b * EMOJI_KEY_MAX, EMOJI_KEY_MAX);
            order[b + 1] = order[b];
            b--;
        }
        memcpy(tmpkeys + (size_t)(b + 1) * EMOJI_KEY_MAX, kbuf, EMOJI_KEY_MAX);
        order[b + 1] = o;
    }
    memcpy(p->keys, tmpkeys, (size_t)np * EMOJI_KEY_MAX);
    memcpy(p->primary, order, (size_t)np * sizeof(int));
    p->nprimary = np;
    rc = 0;
done:
    free(order);
    free(tmpkeys);
    return rc;
}

int emoji_pack_name_count(const EmojiPack *p) { return p->nnames; }

const char *emoji_pack_lookup(const EmojiPack *p, const char *name, size_t len, size_t *ulen)
{
    int lo = 0, hi = p->nnames - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2, c;
        const unsigned char *e = p->names + p->nentry[mid];
        size_t nl = e[0], m = nl < len ? nl : len;

        c = memcmp(e + 1, name, m);
        if (c == 0) {
            c = nl < len ? -1 : (nl > len ? 1 : 0);
        }
        if (c == 0) {
            *ulen = e[1 + nl];
            return (const char *)e + 1 + nl + 1;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

int emoji_pack_name_for_key(const EmojiPack *p, const char *key, char *out, size_t cap)
{
    int lo = 0, hi = p->nprimary - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(p->keys + (size_t)mid * EMOJI_KEY_MAX, key);

        if (c == 0) {
            const unsigned char *e = p->names + p->nentry[p->primary[mid]];
            size_t nl = e[0];

            if (nl + 1 > cap) {
                return 0;
            }
            memcpy(out, e + 1, nl);
            out[nl] = '\0';
            return 1;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return 0;
}

static int name_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
}

static int alnum(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

size_t emoji_expand_shortcodes(const EmojiPack *p, const char *in, char *out, size_t cap)
{
    size_t i = 0, o = 0, n = strlen(in);
    int ticks = 0;                                       /* inside `code` (1) or a ``` block (3) */

    if (cap == 0) {
        return 0;
    }
    while (i < n) {
        char c = in[i];

        if (c == '`') {
            size_t run = 1;

            while (i + run < n && in[i + run] == '`') {
                run++;
            }
            if (ticks == 0) {
                ticks = run >= 3 ? 3 : 1;
            } else if ((ticks == 3 && run >= 3) || (ticks == 1 && run < 3)) {
                ticks = 0;
            }
            while (run > 0 && o + 1 < cap) {
                out[o++] = '`';
                run--;
                i++;
            }
            if (run > 0) {
                break;
            }
            continue;
        }
        if (c == ':' && ticks == 0 && !(i > 0 && (alnum((unsigned char)in[i - 1]) || in[i - 1] == '<'))) {
            size_t j = i + 1, k;
            char low[48];
            const char *u;
            size_t ul;

            while (j < n && j - i - 1 <= 40 && name_char((unsigned char)in[j])) {
                j++;
            }
            if (j < n && in[j] == ':' && j > i + 1 && j - i - 1 <= 40) {
                for (k = 0; k < j - i - 1; k++) {
                    char ch = in[i + 1 + k];

                    low[k] = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
                }
                u = emoji_pack_lookup(p, low, j - i - 1, &ul);
                if (u != NULL && o + ul < cap) {
                    memcpy(out + o, u, ul);
                    o += ul;
                    i = j + 1;
                    continue;
                }
            }
        }
        if (o + 1 >= cap) {
            break;
        }
        out[o++] = c;
        i++;
    }
    out[o] = '\0';
    return o;
}

static void put(char *out, size_t cap, size_t *o, const char *s, size_t n)
{
    if (*o + n < cap) {
        memcpy(out + *o, s, n);
        *o += n;
    }
}

size_t emoji_to_names(const EmojiPack *p, const char *utf8, size_t len, char *out, size_t cap)
{
    size_t at = 0, s0, e0, o = 0;
    char key[EMOJI_KEY_MAX], name[48];

    if (cap == 0) {
        return 0;
    }
    while (at < len && emoji_next(p, utf8, len, at, &s0, &e0, key)) {
        char base[EMOJI_KEY_MAX], tone = 0;
        const char *found = NULL;

        put(out, cap, &o, utf8 + at, s0 - at);
        if (emoji_pack_name_for_key(p, key, name, sizeof(name))) {
            found = name;
        } else {
            /* a skin-tone variant: the plain emoji's name plus ":skin-tone-N:" */
            size_t kl = strlen(key), b = 0, i = 0;

            while (i < kl) {
                size_t part_end = i;

                while (part_end < kl && key[part_end] != '-') part_end++;
                if (part_end - i == 5 && strncmp(key + i, "1f3f", 4) == 0 && key[i + 4] >= 'b' && key[i + 4] <= 'f') {
                    tone = (char)('1' + (key[i + 4] - 'b'));
                } else {
                    if (b > 0) base[b++] = '-';
                    memcpy(base + b, key + i, part_end - i);
                    b += part_end - i;
                }
                i = part_end + 1;
            }
            base[b] = '\0';
            if (tone != 0 && emoji_pack_name_for_key(p, base, name, sizeof(name))) {
                found = name;
            } else {
                tone = 0;
            }
        }
        if (found != NULL) {
            put(out, cap, &o, ":", 1);
            put(out, cap, &o, found, strlen(found));
            put(out, cap, &o, ":", 1);
            if (tone != 0) {
                put(out, cap, &o, ":skin-tone-", 11);
                put(out, cap, &o, &tone, 1);
                put(out, cap, &o, ":", 1);
            }
        } else {
            put(out, cap, &o, utf8 + s0, e0 - s0);
        }
        at = e0;
    }
    put(out, cap, &o, utf8 + at, len - at);
    out[o] = '\0';
    return o;
}
