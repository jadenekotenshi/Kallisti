#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/jpeg.h"
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

/* every picture decodes to exactly libjpeg's pixels */
static void test_manifest(void)
{
    FILE *f = fopen("tests/jpg/manifest.txt", "r");
    char line[256], name[96], want[80], path[160], got[80];
    unsigned long w, h;
    int n = 0;

    CHECK(f != NULL);
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        size_t len;
        unsigned char *data, digest[32];
        PngImage im;
        const char *err;

        if (sscanf(line, "%95s %lu %lu %79s", name, &w, &h, want) != 4) {
            continue;
        }
        sprintf(path, "tests/jpg/%s.jpg", name);
        data = slurp(path, &len);
        CHECK(data != NULL);
        if (data == NULL) {
            continue;
        }
        if (jpeg_decode(data, len, &im, &err) != 0) {
            printf("  %s: %s\n", name, err);
            CHECK(0);
        } else {
            sha256(im.rgba, (size_t)(im.width * im.height * 4UL), digest);
            hex(digest, 32, got);
            CHECK(im.width == w && im.height == h);
            if (strcmp(got, want) != 0) {
                printf("  %s: pixels differ from libjpeg's\n", name);
            }
            CHECK(strcmp(got, want) == 0);
            free(im.rgba);
        }
        free(data);
        n++;
    }
    if (f != NULL) {
        fclose(f);
    }
    CHECK(n >= 40);
}

static unsigned long rs = 31;
static unsigned rnd(void)
{
    rs = rs * 1103515245UL + 12345UL;
    return (unsigned)((rs >> 16) & 0x7fff);
}

static void test_damage(void)
{
    static const char *names[] = { "c_420_b_64x48", "c_420_p_64x48", "g_p_64x48", "rst_420_b", "rst_444_b", "c_422_p_37x23" };
    int k, i, m;

    for (k = 0; k < 6; k++) {
        char path[128];
        size_t len, cut;
        unsigned char *d, *c;
        PngImage im;
        const char *err;

        sprintf(path, "tests/jpg/%s.jpg", names[k]);
        d = slurp(path, &len);
        if (d == NULL) {
            CHECK(0);
            continue;
        }
        c = (unsigned char *)malloc(len);
        /* any cut-off ends in an error and never in a crash */
        for (cut = 0; cut < len; cut += (len > 400 ? len / 200 : 1)) {
            if (jpeg_decode(d, cut, &im, &err) == 0) {
                free(im.rgba);                    /* (a cut after the last scan, before EOI, can still decode) */
            } else {
                CHECK(im.rgba == NULL);
            }
        }
        CHECK(jpeg_decode(d, len, &im, &err) == 0);
        free(im.rgba);
        /* random damage: whatever the result, no crash and no leaked half-result */
        for (i = 0; i < 4000; i++) {
            memcpy(c, d, len);
            for (m = (int)(rnd() % 4) + 1; m > 0; m--) {
                c[rnd() % len] = (unsigned char)rnd();
            }
            if (jpeg_decode(c, len, &im, &err) == 0) {
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
    size_t len;
    unsigned char *d = slurp("tests/jpg/c_420_b_64x48.jpg", &len), *c;
    PngImage im;
    const char *err;
    size_t i;

    CHECK(d != NULL);
    if (d == NULL) {
        return;
    }
    c = (unsigned char *)malloc(len);
    CHECK(jpeg_decode((const unsigned char *)"", 0, &im, &err) != 0);
    CHECK(jpeg_decode((const unsigned char *)"\xff\xd8", 2, &im, &err) != 0);
    CHECK(jpeg_decode((const unsigned char *)"GIF89a....", 10, &im, &err) != 0);
    /* find the SOF0 segment and tamper with it */
    for (i = 2; i + 10 < len; i++) {
        if (d[i] == 0xff && d[i + 1] == 0xc0) {
            break;
        }
    }
    CHECK(i + 10 < len);
    memcpy(c, d, len); c[i + 4] = 12;                              /* 12-bit precision */
    CHECK(jpeg_decode(c, len, &im, &err) != 0 && strstr(err, "8-bit") != NULL);
    memcpy(c, d, len); c[i + 1] = 0xc3;                            /* lossless */
    CHECK(jpeg_decode(c, len, &im, &err) != 0);
    memcpy(c, d, len); c[i + 1] = 0xc9;                            /* arithmetic coding */
    CHECK(jpeg_decode(c, len, &im, &err) != 0);
    memcpy(c, d, len); c[i + 9] = 4;                               /* four components (CMYK) */
    CHECK(jpeg_decode(c, len, &im, &err) != 0);
    memcpy(c, d, len); c[i + 5] = 0; c[i + 6] = 0;                 /* height 0 */
    CHECK(jpeg_decode(c, len, &im, &err) != 0);
    memcpy(c, d, len); c[i + 5] = 0xff; c[i + 6] = 0xff; c[i + 7] = 0xff; c[i + 8] = 0xff;   /* 65535 x 65535 */
    CHECK(jpeg_decode(c, len, &im, &err) != 0 && strstr(err, "large") != NULL);
    free(c);
    free(d);
}

/* PNG or JPEG into a TIFF, whichever the first bytes say */
static void test_to_tiff(void)
{
    size_t len, tl;
    unsigned char *d = slurp("tests/jpg/big_420.jpg", &len), *tiff, *p;
    unsigned long w, h;
    const char *err;

    CHECK(d != NULL);
    if (d == NULL) {
        return;
    }
    CHECK(img_to_tiff(d, len, 0, 0, &tiff, &tl, &w, &h, &err) == 0 && w == 800 && h == 600 && tl == 192 + 800UL * 600 * 3);
    CHECK(tiff[0] == 'M' && tiff[1] == 'M');
    free(tiff);
    CHECK(img_to_tiff(d, len, 400, 400, &tiff, &tl, &w, &h, &err) == 0 && w == 400 && h == 300);
    free(tiff);
    free(d);
    p = slurp("tests/img/t2_d8_i0_13x7.png", &len);
    CHECK(p != NULL && img_to_tiff(p, len, 0, 0, &tiff, &tl, &w, &h, &err) == 0 && w == 13 && h == 7);
    free(tiff);
    free(p);
    CHECK(img_to_tiff((const unsigned char *)"RIFF....WEBPVP8 ", 16, 0, 0, &tiff, &tl, &w, &h, &err) != 0 &&
          strstr(err, "PNG, JPEG or GIF") != NULL);
}

int main(void)
{
    test_manifest();
    test_damage();
    test_refusals();
    test_to_tiff();
    TEST_DONE("jpeg");
}
