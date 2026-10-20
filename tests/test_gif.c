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

/* every frame of every animation is exactly the screen the generator composited (disposal methods included) */
static void test_anim(void)
{
    FILE *f = fopen("tests/gif/anim_manifest.txt", "r");
    static char line[16384];
    int n = 0;

    CHECK(f != NULL);
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        char name[64], path[128], *delays, *shas, *tok;
        unsigned long w, h;
        int nframes, i;
        size_t len;
        unsigned char *data;
        GifAnim a;
        const char *err;

        if (sscanf(line, "%63s %lu %lu %d", name, &w, &h, &nframes) != 4) {
            continue;
        }
        /* the line is: name w h n delays shas, the last two comma-separated */
        delays = strchr(strchr(strchr(strchr(line, ' ') + 1, ' ') + 1, ' ') + 1, ' ') + 1;
        shas = strchr(delays, ' ') + 1;
        *(shas - 1) = '\0';
        sprintf(path, "tests/gif/%s.gif", name);
        data = slurp(path, &len);
        CHECK(data != NULL);
        if (data == NULL) {
            continue;
        }
        if (gif_decode_anim(data, len, 1000, 100000000UL, &a, &err) != 0) {
            printf("  %s: %s\n", name, err);
            CHECK(0);
            free(data);
            continue;
        }
        CHECK(a.width == w && a.height == h && a.nframes == nframes && a.total_frames == nframes);
        tok = shas;
        for (i = 0; i < a.nframes; i++) {
            char want[80], got[80];
            unsigned char digest[32];
            size_t k = strcspn(tok, ",\n");

            memcpy(want, tok, k);
            want[k] = '\0';
            tok += k + (tok[k] == ',' ? 1 : 0);
            sha256(a.rgba[i], (size_t)(a.width * a.height * 4UL), digest);
            hex(digest, 32, got);
            if (strcmp(got, want) != 0) {
                printf("  %s: frame %d differs\n", name, i);
            }
            CHECK(strcmp(got, want) == 0);
        }
        {
            const char *dp = delays;

            for (i = 0; i < a.nframes; i++) {
                unsigned cs = (unsigned)strtoul(dp, NULL, 10);

                CHECK(a.delay_ms[i] == (cs <= 1 ? 100U : cs * 10U));       /* browsers' rule for 0 and 1 */
                dp = strchr(dp, ',') ? strchr(dp, ',') + 1 : dp;
            }
        }
        gif_anim_free(&a);
        free(data);
        n++;
    }
    if (f != NULL) {
        fclose(f);
    }
    CHECK(n == 9);
}

/* limits: a frame cap, a pixel budget, and the first frame always comes */
static void test_anim_limits(void)
{
    size_t len;
    unsigned char *d = slurp("tests/gif/a_many.gif", &len);
    GifAnim a;
    const char *err;

    CHECK(d != NULL);
    if (d == NULL) {
        return;
    }
    CHECK(gif_decode_anim(d, len, 40, 100000000UL, &a, &err) == 0 && a.nframes == 40 && a.total_frames == 130);
    gif_anim_free(&a);
    CHECK(gif_decode_anim(d, len, 1000, 16UL * 12 * 25, &a, &err) == 0 && a.nframes == 25 && a.total_frames == 130);   /* budget: 25 screens */
    gif_anim_free(&a);
    CHECK(gif_decode_anim(d, len, 1000, 1, &a, &err) == 0 && a.nframes == 1 && a.total_frames == 130);              /* never less than one */
    gif_anim_free(&a);
    CHECK(gif_decode_anim(d, len, 0, 100, &a, &err) == 0 && a.nframes == 1);
    gif_anim_free(&a);
    CHECK(gif_decode_anim(d, len, 1000, 100000000UL, &a, &err) == 0 && a.nframes == 130);
    gif_anim_free(&a);
    /* a damaged later frame ends the animation there; the frames before it still play */
    {
        unsigned char *c = (unsigned char *)malloc(len);
        size_t i;
        int short_ok = 0;

        for (i = len / 2; i < len; i += 37) {
            memcpy(c, d, len);
            c[i] ^= 0xff;
            if (gif_decode_anim(c, len, 1000, 100000000UL, &a, &err) == 0) {
                short_ok += a.nframes >= 1;
                gif_anim_free(&a);
            }
        }
        CHECK(short_ok > 0);
        /* cut in the middle: whole frames survive */
        CHECK(gif_decode_anim(d, len / 2, 1000, 100000000UL, &a, &err) == 0 && a.nframes > 10 && a.nframes < 130);
        gif_anim_free(&a);
        free(c);
    }
    free(d);
    CHECK(gif_decode_anim((const unsigned char *)"GIF89a", 6, 10, 1000, &a, &err) != 0 && a.nframes == 0 && a.rgba == NULL);
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

static void test_img_anim(void)
{
    size_t len;
    unsigned char *d = slurp("tests/gif/a_mixed.gif", &len);
    ImgAnim a;
    const char *err;
    int i;

    CHECK(d != NULL);
    if (d == NULL) {
        return;
    }
    CHECK(img_gif_to_anim(d, len, 0, 0, 100, 100000000UL, &a, &err) == 0 && a.n == 5 && a.w == 30 && a.h == 20 && a.total_frames == 5);
    for (i = 0; i < a.n; i++) {
        CHECK(a.len[i] == 192 + 30UL * 20 * 3 && a.tiff[i][0] == 'M' && a.delay_ms[i] >= 100 && a.delay_ms[i] <= 100);
    }
    img_anim_free(&a);
    CHECK(img_gif_to_anim(d, len, 10, 10, 100, 100000000UL, &a, &err) == 0 && a.n == 5 && a.w == 10 && a.h == 7);        /* 3x smaller */
    CHECK(a.len[0] == 192 + 10UL * 7 * 3);
    img_anim_free(&a);
    CHECK(img_gif_to_anim(d, len, 0, 0, 2, 100000000UL, &a, &err) == 0 && a.n == 2 && a.total_frames == 5);
    img_anim_free(&a);
    free(d);
    d = slurp("tests/img/t2_d8_i0_13x7.png", &len);
    CHECK(d != NULL && img_gif_to_anim(d, len, 0, 0, 10, 1000, &a, &err) != 0 && a.n == 0 && a.tiff == NULL);
    free(d);
}

int main(void)
{
    test_manifest();
    test_anim();
    test_anim_limits();
    test_damage();
    test_refusals();
    test_to_tiff();
    test_img_anim();
    TEST_DONE("gif");
}
