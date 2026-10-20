#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/png.h"
#include "../core/img.h"
#include "../core/inflate.h"
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

/* every picture of the manifest decodes to exactly the RGBA its generator derived from the samples */
static void test_manifest(void)
{
    FILE *f = fopen("tests/img/manifest.txt", "r");
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
        sprintf(path, "tests/img/%s.png", name);
        data = slurp(path, &len);
        CHECK(data != NULL);
        if (data == NULL) {
            continue;
        }
        if (png_decode(data, len, &im, &err) != 0) {
            printf("  %s: %s\n", name, err);
            CHECK(0);
        } else {
            sha256(im.rgba, (size_t)(im.width * im.height * 4UL), digest);
            hex(digest, 32, got);
            CHECK(im.width == w && im.height == h);
            if (strcmp(got, want) != 0) {
                printf("  %s: pixels differ\n", name);
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
    CHECK(n >= 90);
}

static void test_bad_input(void)
{
    size_t len;
    unsigned char *d = slurp("tests/img/t2_d8_i0_13x7.png", &len), *c;
    PngImage im;
    const char *err;
    size_t i;
    int passes = 0;

    CHECK(d != NULL);
    if (d == NULL) {
        return;
    }
    CHECK(png_decode(d, len, &im, &err) == 0 && im.rgba != NULL);
    free(im.rgba);
    c = (unsigned char *)malloc(len);
    /* damage anywhere is caught: flip each byte in turn (chunk CRCs, the zlib checksum and the header checks) */
    for (i = 0; i < len; i++) {
        memcpy(c, d, len);
        c[i] ^= 0x10;
        if (png_decode(c, len, &im, &err) == 0) {
            passes++;
            free(im.rgba);
        } else {
            CHECK(im.rgba == NULL);
        }
    }
    CHECK(passes == 0);
    /* truncation */
    for (i = 0; i < len; i++) {
        CHECK(png_decode(d, i, &im, &err) != 0 && im.rgba == NULL);
    }
    CHECK(png_decode((const unsigned char *)"", 0, &im, &err) != 0);
    CHECK(png_decode((const unsigned char *)"GIF89a......................", 28, &im, &err) != 0);
    free(c);
    free(d);
}

/* chunk-level mutations that keep the CRCs valid, so the decoder's own checks are what refuse them */
static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static void fix_crc(unsigned char *png, size_t chunk_off)
{
    unsigned long clen = ((unsigned long)png[chunk_off] << 24) | ((unsigned long)png[chunk_off + 1] << 16) |
                         ((unsigned long)png[chunk_off + 2] << 8) | png[chunk_off + 3];

    put32(png + chunk_off + 8 + clen, crc32_sum(0, png + chunk_off + 4, 4 + (size_t)clen));
}

static void test_header_abuse(void)
{
    size_t len;
    unsigned char *d = slurp("tests/img/t6_d8_i0_13x7.png", &len), *c;
    PngImage im;
    const char *err;

    CHECK(d != NULL);
    if (d == NULL) {
        return;
    }
    c = (unsigned char *)malloc(len);
    /* IHDR at offset 8: width, height, depth, ctype, comp, filter, interlace at 16..28 */
    memcpy(c, d, len); put32(c + 16, 0x7fffffffUL); fix_crc(c, 8);
    CHECK(png_decode(c, len, &im, &err) != 0 && strstr(err, "large") != NULL);          /* huge picture */
    memcpy(c, d, len); put32(c + 16, 0); fix_crc(c, 8);
    CHECK(png_decode(c, len, &im, &err) != 0);                                           /* zero width */
    memcpy(c, d, len); put32(c + 16, 5000); put32(c + 20, 5000); fix_crc(c, 8);
    CHECK(png_decode(c, len, &im, &err) != 0);                                           /* over the pixel limit */
    memcpy(c, d, len); c[24] = 3; fix_crc(c, 8);
    CHECK(png_decode(c, len, &im, &err) != 0);                                           /* depth 3 */
    memcpy(c, d, len); c[25] = 5; fix_crc(c, 8);
    CHECK(png_decode(c, len, &im, &err) != 0);                                           /* colour type 5 */
    memcpy(c, d, len); c[26] = 1; fix_crc(c, 8);
    CHECK(png_decode(c, len, &im, &err) != 0);                                           /* compression method 1 */
    memcpy(c, d, len); c[28] = 2; fix_crc(c, 8);
    CHECK(png_decode(c, len, &im, &err) != 0);                                           /* interlace method 2 */
    memcpy(c, d, len); put32(c + 16, 14); fix_crc(c, 8);
    CHECK(png_decode(c, len, &im, &err) != 0);                                           /* header says wider than the data */
    memcpy(c, d, len); put32(c + 16, 12); fix_crc(c, 8);
    CHECK(png_decode(c, len, &im, &err) != 0);                                           /* ... and narrower */
    free(c);
    free(d);
}

static unsigned long rs = 77;
static unsigned rnd(void)
{
    rs = rs * 1103515245UL + 12345UL;
    return (unsigned)((rs >> 16) & 0x7fff);
}

static void test_fuzz(void)
{
    static const char *names[] = { "t3_d4_i1_33x17", "t6_d16_i0_13x7", "t0_d1_i1_13x7", "big_rgb_interlaced", "t4_d8_i1_33x17" };
    int k, i, m;

    for (k = 0; k < 5; k++) {
        char path[128];
        size_t len;
        unsigned char *d, *c;
        PngImage im;
        const char *err;

        sprintf(path, "tests/img/%s.png", names[k]);
        d = slurp(path, &len);
        if (d == NULL) {
            CHECK(0);
            continue;
        }
        c = (unsigned char *)malloc(len);
        for (i = 0; i < 3000; i++) {
            memcpy(c, d, len);
            for (m = (int)(rnd() % 4) + 1; m > 0; m--) {
                c[rnd() % len] = (unsigned char)rnd();
            }
            /* sometimes repair the checksums so the damage reaches the decoder proper */
            if (rnd() % 2 == 0) {
                size_t off = 8;

                while (off + 12 <= len) {
                    unsigned long cl = ((unsigned long)c[off] << 24) | ((unsigned long)c[off + 1] << 16) | ((unsigned long)c[off + 2] << 8) | c[off + 3];

                    if (cl > len || off + 12 + cl > len) break;
                    fix_crc(c, off);
                    off += 12 + cl;
                }
            }
            if (png_decode(c, len, &im, &err) == 0) {
                free(im.rgba);
            }
        }
        free(c);
        free(d);
    }
    CHECK(1);
}

/* PNG -> flattened, shrunk -> TIFF */
static unsigned long tiff_u16(const unsigned char *p) { return ((unsigned long)p[0] << 8) | p[1]; }
static unsigned long tiff_u32(const unsigned char *p) { return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3]; }

static void test_tiff(void)
{
    size_t len, tl;
    unsigned char *d = slurp("tests/img/big_rgba.png", &len), *tiff;
    unsigned long w, h, entries, i, off = 0, bytes = 0, width = 0, height = 0;
    const char *err;

    CHECK(d != NULL);
    if (d == NULL) {
        return;
    }
    CHECK(img_to_tiff(d, len, 0, 0, &tiff, &tl, &w, &h, &err) == 0 && w == 320 && h == 200);
    CHECK(tiff[0] == 'M' && tiff[1] == 'M' && tiff_u16(tiff + 2) == 42 && tiff_u32(tiff + 4) == 8);
    entries = tiff_u16(tiff + 8);
    CHECK(entries == 13);
    for (i = 0; i < entries; i++) {
        const unsigned char *e = tiff + 10 + i * 12;

        if (tiff_u16(e) == 256) width = tiff_u32(e + 8);
        if (tiff_u16(e) == 257) height = tiff_u32(e + 8);
        if (tiff_u16(e) == 273) off = tiff_u32(e + 8);
        if (tiff_u16(e) == 279) bytes = tiff_u32(e + 8);
    }
    CHECK(width == 320 && height == 200 && bytes == 320UL * 200 * 3 && off + bytes == tl);
    free(tiff);

    /* shrunk by a whole factor until it fits: 320x200 into 100x100 is 4x smaller (80x50) */
    CHECK(img_to_tiff(d, len, 100, 100, &tiff, &tl, &w, &h, &err) == 0 && w == 80 && h == 50 && tl == 192 + 80UL * 50 * 3);
    free(tiff);
    CHECK(img_to_tiff(d, len, 1000, 100, &tiff, &tl, &w, &h, &err) == 0 && w == 160 && h == 100);
    free(tiff);
    CHECK(img_to_tiff((const unsigned char *)"nope", 4, 0, 0, &tiff, &tl, &w, &h, &err) != 0 && tiff == NULL);
    free(d);

    /* transparency is laid over white */
    d = slurp("tests/img/t6_d8_i0_1x1.png", &len);
    if (d != NULL) {
        PngImage im;
        unsigned char *t2;

        CHECK(png_decode(d, len, &im, &err) == 0);
        CHECK(img_to_tiff(d, len, 0, 0, &t2, &tl, &w, &h, &err) == 0 && w == 1 && h == 1);
        {
            unsigned long a = im.rgba[3];

            CHECK(t2[192] == (unsigned char)((im.rgba[0] * a + 255UL * (255UL - a)) / 255UL));
        }
        free(t2);
        free(im.rgba);
        free(d);
    }
    CHECK(tiff_encode_rgb((const unsigned char *)"abc", 0, 1, &tl) == NULL);
}

static void test_inflate(void)
{
    /* "hello hello hello hello" with fixed Huffman codes, made by zlib */
    static const unsigned char z[] = { 0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0x01, 0x68, 0x03, 0x08, 0xb1 };
    unsigned char out[23];
    const char *err;

    memset(out, 0, sizeof(out));
    CHECK(inflate_zlib(z, sizeof(z), out, 23, &err) == 0 && memcmp(out, "hello hello hello hello", 23) == 0);
    CHECK(inflate_zlib(z, sizeof(z), out, 22, &err) != 0);           /* more data than expected */
    CHECK(inflate_zlib(z, sizeof(z), out, 24, &err) != 0);           /* less */
    CHECK(inflate_zlib(z, 0, out, 23, &err) != 0 && inflate_zlib(z, 5, out, 23, &err) != 0);
    CHECK(adler32_sum((const unsigned char *)"Wikipedia", 9) == 0x11E60398UL);
    CHECK(crc32_sum(0, (const unsigned char *)"123456789", 9) == 0xCBF43926UL);
}

int main(void)
{
    test_inflate();
    test_manifest();
    test_bad_input();
    test_header_abuse();
    test_tiff();
    test_fuzz();
    TEST_DONE("png");
}
