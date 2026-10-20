#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/emoji.h"
#include "../core/sha2.h"
#include "test.h"

static EmojiPack *pack;

static void hex(const unsigned char *d, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[i * 2] = H[d[i] >> 4];
        out[i * 2 + 1] = H[d[i] & 15];
    }
    out[n * 2] = '\0';
}

static void test_pack(void)
{
    FILE *f = fopen("tests/emoji_samples.txt", "r");
    char line[200], key[100], want[80], got[80];
    unsigned char rgba[16 * 16 * 4], digest[32];
    int n = 0;
    size_t kl;
    unsigned char *tiff;

    CHECK(pack != NULL && emoji_pack_count(pack) >= 3800 && emoji_pack_size(pack) == 16);
    CHECK(f != NULL);
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "%99s %79s", key, want) != 2) {
            continue;
        }
        CHECK(emoji_pack_has(pack, key));
        CHECK(emoji_pack_rgba(pack, key, rgba) == 0);
        sha256(rgba, sizeof(rgba), digest);
        hex(digest, 32, got);
        if (strcmp(got, want) != 0) {
            printf("  %s: pixels differ\n", key);
        }
        CHECK(strcmp(got, want) == 0);
        n++;
    }
    if (f != NULL) {
        fclose(f);
    }
    CHECK(n >= 100);
    /* a real picture: transparent corners, a body in the middle */
    CHECK(emoji_pack_rgba(pack, "1f600", rgba) == 0 && rgba[3] == 0 && rgba[(8 * 16 + 8) * 4 + 3] == 255);
    tiff = emoji_pack_tiff(pack, "1f600", &kl);
    CHECK(tiff != NULL && kl == 192 + 16 * 16 * 3 && tiff[0] == 'M' && tiff[192] == 255 && tiff[193] == 255 && tiff[194] == 255);   /* corner: white */
    free(tiff);
    CHECK(!emoji_pack_has(pack, "") && !emoji_pack_has(pack, "zz") && !emoji_pack_has(pack, "1f60") && !emoji_pack_has(pack, "1f6000"));
    CHECK(emoji_pack_rgba(pack, "nope", rgba) == -1 && emoji_pack_tiff(pack, "nope", &kl) == NULL);
    CHECK(emoji_pack_key(pack, -1, &kl) == NULL && emoji_pack_key(pack, emoji_pack_count(pack), &kl) == NULL);
    /* the index is sorted: byte order, shorter first */
    {
        int i;
        size_t a, b;
        const char *prev = emoji_pack_key(pack, 0, &a), *cur;
        int sorted = 1;

        for (i = 1; i < emoji_pack_count(pack); i++) {
            int c;

            cur = emoji_pack_key(pack, i, &b);
            c = memcmp(prev, cur, a < b ? a : b);
            if (c > 0 || (c == 0 && a >= b)) sorted = 0;
            prev = cur;
            a = b;
        }
        CHECK(sorted);
    }
    /* the files that are not packs */
    CHECK(emoji_pack_open("tests/emoji_samples.txt") == NULL && emoji_pack_open("tests/no/such/file") == NULL);
}

/* the first emoji in `text`: its byte range and key, or "" */
static int first(const char *text, size_t *start, size_t *end, char *key)
{
    return emoji_next(pack, text, strlen(text), 0, start, end, key);
}

#define EXPECT(text, s0, e0, k) do { size_t a_, b_; char kk_[EMOJI_KEY_MAX]; \
    int r_ = first(text, &a_, &b_, kk_); \
    if (!(r_ == 1 && a_ == (s0) && b_ == (e0) && strcmp(kk_, k) == 0)) \
        printf("  expected [%d,%d) \"%s\", got r=%d [%lu,%lu) \"%s\"\n", (int)(s0), (int)(e0), k, r_, (unsigned long)a_, (unsigned long)b_, r_ ? kk_ : ""); \
    CHECK(r_ == 1 && a_ == (s0) && b_ == (e0) && strcmp(kk_, k) == 0); } while (0)

#define NONE(text) do { size_t a_, b_; char kk_[EMOJI_KEY_MAX]; int r_ = first(text, &a_, &b_, kk_); \
    if (r_) printf("  expected none in \"%s\", got \"%s\"\n", text, kk_); CHECK(r_ == 0); } while (0)

static void test_matching(void)
{
    /* single emoji, in the middle of text */
    EXPECT("a\xF0\x9F\x98\x80" "b", 1, 5, "1f600");
    EXPECT("\xF0\x9F\x98\x80", 0, 4, "1f600");
    /* text-style symbols: the heart is an emoji even bare; (c) (r) (tm) and arrows only with U+FE0F */
    EXPECT("\xE2\x9D\xA4", 0, 3, "2764");
    EXPECT("\xE2\x9D\xA4\xEF\xB8\x8F", 0, 6, "2764");
    NONE("\xC2\xA9");
    EXPECT("\xC2\xA9\xEF\xB8\x8F", 0, 5, "a9");
    NONE("\xC2\xAE");
    EXPECT("\xC2\xAE\xEF\xB8\x8F", 0, 5, "ae");
    NONE("\xE2\x84\xA2");
    EXPECT("\xE2\x84\xA2\xEF\xB8\x8F", 0, 6, "2122");
    NONE("\xE2\x86\x94");
    EXPECT("\xE2\x86\x94\xEF\xB8\x8F", 0, 6, "2194");
    NONE("\xE2\x86\x92");                                            /* a plain arrow is not an emoji at all */
    /* skin tones, joined families, flags */
    EXPECT("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", 0, 8, "1f44d-1f3fd");
    EXPECT("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", 0, 18, "1f468-200d-1f469-200d-1f467");
    EXPECT("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6", 0, 25,
           "1f468-200d-1f469-200d-1f467-200d-1f466");
    EXPECT("\xF0\x9F\xA7\x91\xF0\x9F\x8F\xBD\xE2\x80\x8D\xF0\x9F\x9A\x80", 0, 15, "1f9d1-1f3fd-200d-1f680");              /* astronaut, medium skin */
    EXPECT("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8", 0, 8, "1f1fa-1f1f8");                                                      /* US flag */
    EXPECT("\xF0\x9F\x87\xBA" "x", 0, 4, "1f1fa");                                                                         /* one letter alone */
    EXPECT("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8\xF0\x9F\x87\xAC", 0, 8, "1f1fa-1f1f8");                                       /* a third letter is another emoji */
    /* keycaps: the digit alone is not an emoji */
    EXPECT("1\xEF\xB8\x8F\xE2\x83\xA3", 0, 7, "31-20e3");
    EXPECT("#\xEF\xB8\x8F\xE2\x83\xA3", 0, 7, "23-20e3");
    EXPECT("*\xE2\x83\xA3", 0, 4, "2a-20e3");
    EXPECT("x0\xE2\x83\xA3y", 1, 5, "30-20e3");
    NONE("1");
    NONE("12 #4 *5 abc");
    NONE("1\xEF\xB8\x8F");                                                                                                 /* a digit and a selector: no keycap */
    /* a flag made of tag characters (England) */
    EXPECT("\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xA5\xF3\xA0\x81\xAE\xF3\xA0\x81\xA7\xF3\xA0\x81\xBF", 0, 28,
           "1f3f4-e0067-e0062-e0065-e006e-e0067-e007f");
    EXPECT("\xF0\x9F\x8F\xB4", 0, 4, "1f3f4");                                                                             /* the black flag alone */
    /* sequences the pack does not know fall back to the pieces it does */
    EXPECT("\xF0\x9F\x98\x80\xE2\x80\x8D\xF0\x9F\x9A\x80", 0, 4, "1f600");                                                 /* smile + ZWJ + rocket: just the smile first */
    EXPECT("\xF0\x9F\x98\x80\xE2\x80\x8D", 0, 4, "1f600");                                                                 /* a trailing joiner is left alone */
    EXPECT("\xE2\x9D\xA4\xEF\xB8\x8F\xF0\x9F\x8F\xBD", 0, 6, "2764");                                                      /* a heart takes no skin tone */
    /* things that are not emoji */
    NONE("");
    NONE("plain text, with punctuation: 100%!");
    NONE("\xF0\x9D\x94\x98");                                                                                              /* a mathematical letter */
    NONE("\xE4\xB8\xAD\xE6\x96\x87");                                                                                      /* Chinese */
    NONE("\xEF\xB8\x8F");                                                                                                  /* a lone selector */
    NONE("\xE2\x80\x8D");                                                                                                  /* a lone joiner */
    NONE("\xF0\x9F");                                                                                                      /* a cut-off sequence */
    NONE("\xF0\x9F\x98");
    NONE("\xFF\xFE\xC0\x80\xF8\x88\x80\x80\x80");                                                                          /* ill-formed bytes */
    /* the existing emoji after bad bytes */
    EXPECT("\xFF\xF0\x9F\x98\x80", 1, 5, "1f600");
    EXPECT("\xF0\x9F\x98\xF0\x9F\x98\x80", 3, 7, "1f600");
}

static void test_scanning(void)
{
    const char *t = "hi \xF0\x9F\x98\x80 and \xE2\x9D\xA4\xEF\xB8\x8F, \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD!\xF0\x9F\x98\x80\xF0\x9F\x98\x80";
    size_t len = strlen(t), pos = 0, a, b;
    char key[EMOJI_KEY_MAX];
    const char *want[] = { "1f600", "2764", "1f44d-1f3fd", "1f600", "1f600" };
    int n = 0;

    while (emoji_next(pack, t, len, pos, &a, &b, key)) {
        CHECK(n < 5 && strcmp(key, want[n]) == 0 && a >= pos && b > a && b <= len);
        pos = b;
        n++;
    }
    CHECK(n == 5);
    /* `from` in the middle of a sequence starts the search there */
    CHECK(emoji_next(pack, "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", 8, 4, &a, &b, key) == 1 && a == 4 && b == 8 && strcmp(key, "1f3fd") == 0);
    CHECK(emoji_next(pack, t, len, len, &a, &b, key) == 0 && emoji_next(pack, t, len, len + 10, &a, &b, key) == 0);
}

/* every picture in the pack can be reached: write its code points as UTF-8 and find exactly that key */
static void test_every_key(void)
{
    int i, failures = 0, n = emoji_pack_count(pack);

    for (i = 0; i < n; i++) {
        size_t kl, o = 0;
        const char *k = emoji_pack_key(pack, i, &kl);
        char kc[EMOJI_KEY_MAX], u[200], got[EMOJI_KEY_MAX];
        const char *p;
        long cps[40];
        int nc = 0, c, zwj = 0, keycap = 0, k2;
        size_t a, b;

        memcpy(kc, k, kl);
        kc[kl] = '\0';
        for (p = kc; *p != '\0'; ) {
            long v = 0;

            while (*p != '\0' && *p != '-') {
                v = v * 16 + (*p <= '9' ? *p - '0' : *p - 'a' + 10);
                p++;
            }
            cps[nc++] = v;
            if (v == 0x200d) zwj = 1;
            if (v == 0x20e3) keycap = 1;
            if (*p == '-') p++;
        }
        /* a symbol the fonts have needs its variation selector to count as an emoji */
        if (cps[0] <= 0x21ff && !keycap && !(nc > 1 && cps[1] == 0xfe0f)) {
            for (k2 = nc; k2 > 1; k2--) cps[k2] = cps[k2 - 1];
            cps[1] = 0xfe0f;
            nc++;
        }
        (void)zwj;
        for (c = 0; c < nc; c++) {
            long v = cps[c];

            if (v < 0x80) u[o++] = (char)v;
            else if (v < 0x800) { u[o++] = (char)(0xc0 | (v >> 6)); u[o++] = (char)(0x80 | (v & 0x3f)); }
            else if (v < 0x10000L) { u[o++] = (char)(0xe0 | (v >> 12)); u[o++] = (char)(0x80 | ((v >> 6) & 0x3f)); u[o++] = (char)(0x80 | (v & 0x3f)); }
            else { u[o++] = (char)(0xf0 | (v >> 18)); u[o++] = (char)(0x80 | ((v >> 12) & 0x3f)); u[o++] = (char)(0x80 | ((v >> 6) & 0x3f)); u[o++] = (char)(0x80 | (v & 0x3f)); }
        }
        u[o] = '\0';
        if (!emoji_next(pack, u, o, 0, &a, &b, got) || a != 0 || b != o || strcmp(got, kc) != 0) {
            if (failures < 8) {
                printf("  key %s: found \"%s\" [%lu,%lu) of %lu bytes\n", kc,
                       emoji_next(pack, u, o, 0, &a, &b, got) ? got : "nothing", (unsigned long)a, (unsigned long)b, (unsigned long)o);
            }
            failures++;
        }
    }
    CHECK(failures == 0);
}

static unsigned long rs = 41;
static unsigned rnd(void)
{
    rs = rs * 1103515245UL + 12345UL;
    return (unsigned)((rs >> 16) & 0x7fff);
}

/* arbitrary mixes of emoji pieces and junk: no crash, and every answer is sane */
static void test_fuzz(void)
{
    static const char *bits[] = { "\xF0\x9F\x98\x80", "\xE2\x9D\xA4", "\xEF\xB8\x8F", "\xE2\x80\x8D", "\xF0\x9F\x8F\xBD", "\xF0\x9F\x87\xBA",
                                  "\xF0\x9F\x87\xB8", "\xE2\x83\xA3", "1", "#", "a", " ", "\xF0\x9F\x91\xA8", "\xF0\x9F\x91\xA9", "\xF3\xA0\x81\xA7",
                                  "\xF3\xA0\x81\xBF", "\xF0\x9F\x8F\xB4", "\xFF", "\xC2\xA9", "\xE2\x86\x94", "\xF0\x9F" };
    int i, k;
    char buf[400], key[EMOJI_KEY_MAX];

    for (i = 0; i < 30000; i++) {
        size_t n = 0, pos = 0, a, b, len;

        for (k = (int)(rnd() % 14); k > 0; k--) {
            const char *bit = bits[rnd() % (sizeof(bits) / sizeof(bits[0]))];

            strcpy(buf + n, bit);
            n += strlen(bit);
        }
        len = n;
        while (emoji_next(pack, buf, len, pos, &a, &b, key)) {
            if (!(a >= pos && b > a && b <= len && key[0] != '\0' && emoji_pack_has(pack, key))) {
                CHECK(0);
                break;
            }
            pos = b;
        }
    }
    CHECK(1);
}

int main(void)
{
    pack = emoji_pack_open("app/emoji.pack");
    if (pack == NULL) {
        printf("  cannot open app/emoji.pack (run from the repository root)\n");
        return 1;
    }
    test_pack();
    test_matching();
    test_scanning();
    test_every_key();
    test_fuzz();
    emoji_pack_close(pack);
    TEST_DONE("emoji");
}
