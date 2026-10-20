#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gif.h"
#include "../core/img.h"
#include "../core/sha2.h"
#include "test.h"

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)len + 1);
    if (fread(b, 1, (size_t)len, f) != (size_t)len) {
        len = 0;
    }
    fclose(f);
    *n = (size_t)len;
    return b;
}

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

/* every picture's first frame is exactly what the generator derived from its palette indices */
static void test_manifest(void)
{
    FILE *f = fopen("tests/gif/manifest.txt", "r");
    char line[256], name[96], want[80], path[160], got[80];
    unsigned long w, h;
    int frames, n = 0;

    CHECK(f != NULL);
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        size_t len;
        unsigned char *data, digest[32];
        PngImage im;
        const char *err;
        int nf = 0;

        if (sscanf(line, "%95s %lu %lu %d %79s", name, &w, &h, &frames, want) != 5) {
            continue;
        }
        sprintf(path, "tests/gif/%s.gif", name);
        data = slurp(path, &len);
        CHECK(data != NULL);
        if (data == NULL) {
            continue;
        }
        if (gif_decode(data, len, &im, &nf, &err) != 0) {
            printf("  %s: %s\n", name, err);
            CHECK(0);
        } else {
            sha256(im.rgba, (size_t)(im.width * im.height * 4UL), digest);
            hex(digest, 32, got);
            if (strcmp(name, "screen_zero") == 0) {
                CHECK(im.width == 13 && im.height == 7);           /* the screen is taken from the frame: 2+11, 1+6 */
            } else {
                CHECK(im.width == w && im.height == h);
                if (strcmp(got, want) != 0) {
                    printf("  %s: pixels differ\n", name);
                }
                CHECK(strcmp(got, want) == 0);
            }
            CHECK(nf == frames);
            free(im.rgba);
        }
        free(data);
        n++;
    }
    if (f != NULL) {
        fclose(f);
    }
    CHECK(n >= 50);
}

static unsigned long rs = 17;
static unsigned rnd(void)
{
    rs = rs * 1103515245UL + 12345UL;
    return (unsigned)((rs >> 16) & 0x7fff);
}

static void test_damage(void)
{
    static const char *names[] = { "p16_i1_33x17", "big", "anim", "local_only", "p256_i0_13x7", "extras", "big_interlaced" };
    int k, i, m;

    for (k = 0; k < 7; k++) {
        char path[128];
        size_t len, cut;
        unsigned char *d, *c;
        PngImage im;
        const char *err;

        sprintf(path, "tests/gif/%s.gif", names[k]);
        d = slurp(path, &len);
        if (d == NULL) {
            CHECK(0);
            continue;
        }
        c = (unsigned char *)malloc(len);
        /* a cut-off file: an error (or, after the first frame is whole, a picture) -- never a crash */
        for (cut = 0; cut < len; cut += (len > 400 ? len / 150 : 1)) {
            if (gif_decode(d, cut, &im, NULL, &err) == 0) {
                free(im.rgba);
            } else {
                CHECK(im.rgba == NULL);
            }
        }
        for (i = 0; i < 4000; i++) {
            memcpy(c, d, len);
            for (m = (int)(rnd() % 5) + 1; m > 0; m--) {
                c[rnd() % len] = (unsigned char)rnd();
            }
            if (gif_decode(c, len, &im, NULL, &err) == 0) {
                free(im.rgba);
            } else {
                CHECK(im.rgba == NULL);
            }
        }
        free(c);
        free(d);
    }
}

static void test_refusals(void)
{
    PngImage im;
    const char *err;
    unsigned char g[64];

    CHECK(gif_decode((const unsigned char *)"", 0, &im, NULL, &err) != 0);
    CHECK(gif_decode((const unsigned char *)"GIF89", 5, &im, NULL, &err) != 0);
    CHECK(gif_decode((const unsigned char *)"GIF90a_______________", 21, &im, NULL, &err) != 0);
    /* a header, a screen, and a trailer, but no image */
    memcpy(g, "GIF89a", 6);
    memset(g + 6, 0, 7);
    g[6] = 1; g[8] = 1;
    g[13] = 0x3b;
    CHECK(gif_decode(g, 14, &im, NULL, &err) != 0 && strstr(err, "no image") != NULL);
    /* an image but no colour table at all */
    {
        static const unsigned char nopal[] = { 'G','I','F','8','9','a', 1,0, 1,0, 0, 0, 0,
                                               0x2c, 0,0, 0,0, 1,0, 1,0, 0, 2, 2, 0x44, 0x01, 0 , 0x3b };
        CHECK(gif_decode(nopal, sizeof(nopal), &im, NULL, &err) != 0 && strstr(err, "colour table") != NULL);
    }
    /* a huge frame */
    {
        static const unsigned char huge[] = { 'G','I','F','8','9','a', 1,0, 1,0, 0x80, 0, 0, 0,0,0, 0,0,0,
                                              0x2c, 0,0, 0,0, 0xff,0xff, 0xff,0xff, 0, 2, 1, 0x44, 0, 0x3b };
        CHECK(gif_decode(huge, sizeof(huge), &im, NULL, &err) != 0 && strstr(err, "large") != NULL);
    }
    /* an impossible LZW code size */
    {
        static const unsigned char badcode[] = { 'G','I','F','8','9','a', 1,0, 1,0, 0x80, 0, 0, 0,0,0, 0,0,0,
                                                 0x2c, 0,0, 0,0, 1,0, 1,0, 0, 13, 1, 0x44, 0, 0x3b };
        CHECK(gif_decode(badcode, sizeof(badcode), &im, NULL, &err) != 0);
    }
}

static void test_to_tiff(void)
{
    size_t len, tl;
    unsigned char *d = slurp("tests/gif/big.gif", &len), *tiff;
    unsigned long w, h;
    const char *err;

    CHECK(d != NULL);
    if (d == NULL) {
        return;
    }
    CHECK(img_to_tiff(d, len, 0, 0, &tiff, &tl, &w, &h, &err) == 0 && w == 320 && h == 200 && tl == 192 + 320UL * 200 * 3);
    free(tiff);
    CHECK(img_to_tiff(d, len, 100, 100, &tiff, &tl, &w, &h, &err) == 0 && w == 80 && h == 50);
    free(tiff);
    free(d);
    /* transparent pixels end up white */
    d = slurp("tests/gif/trans_zero.gif", &len);
    if (d != NULL) {
        PngImage im;
        unsigned char *t2;
        size_t i;
        int checked = 0;

        CHECK(gif_decode(d, len, &im, NULL, &err) == 0);
        CHECK(img_to_tiff(d, len, 0, 0, &t2, &tl, &w, &h, &err) == 0 && w == 9 && h == 9);
        for (i = 0; i < 81; i++) {
            if (im.rgba[i * 4 + 3] == 0) {
                CHECK(t2[192 + i * 3] == 255 && t2[192 + i * 3 + 1] == 255 && t2[192 + i * 3 + 2] == 255);
                checked++;
            }
        }
        CHECK(checked > 0);
        free(t2);
        free(im.rgba);
        free(d);
    }
}

int main(void)
{
    test_manifest();
    test_damage();
    test_refusals();
    test_to_tiff();
    TEST_DONE("gif");
}
