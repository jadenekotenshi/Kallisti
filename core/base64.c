/* Base64: see base64.h. */
#include <string.h>
#include "base64.h"

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const unsigned char *in, size_t n, char *out)
{
    size_t i = 0, o = 0;
    unsigned long v;

    while (n - i >= 3) {
        v = ((unsigned long)in[i] << 16) | ((unsigned long)in[i + 1] << 8) | in[i + 2];
        out[o++] = alphabet[(v >> 18) & 63];
        out[o++] = alphabet[(v >> 12) & 63];
        out[o++] = alphabet[(v >> 6) & 63];
        out[o++] = alphabet[v & 63];
        i += 3;
    }
    if (n - i == 1) {
        v = (unsigned long)in[i] << 16;
        out[o++] = alphabet[(v >> 18) & 63];
        out[o++] = alphabet[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        v = ((unsigned long)in[i] << 16) | ((unsigned long)in[i + 1] << 8);
        out[o++] = alphabet[(v >> 18) & 63];
        out[o++] = alphabet[(v >> 12) & 63];
        out[o++] = alphabet[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

int base64_decode(const char *s, unsigned char *out, size_t *outlen)
{
    size_t n = strlen(s), i, o = 0;
    int a, b, c, d;
    unsigned long v;

    if (n % 4 != 0) {
        return -1;
    }
    for (i = 0; i < n; i += 4) {
        a = b64val((unsigned char)s[i]);
        b = b64val((unsigned char)s[i + 1]);
        if (a < 0 || b < 0) {
            return -1;
        }
        if (s[i + 2] == '=') {
            /* "xx==": only valid as the final quantum, and the unused bits must be zero */
            if (i + 4 != n || s[i + 3] != '=' || (b & 15) != 0) {
                return -1;
            }
            v = ((unsigned long)a << 18) | ((unsigned long)b << 12);
            out[o++] = (unsigned char)(v >> 16);
            break;
        }
        c = b64val((unsigned char)s[i + 2]);
        if (c < 0) {
            return -1;
        }
        if (s[i + 3] == '=') {
            if (i + 4 != n || (c & 3) != 0) {
                return -1;
            }
            v = ((unsigned long)a << 18) | ((unsigned long)b << 12) | ((unsigned long)c << 6);
            out[o++] = (unsigned char)(v >> 16);
            out[o++] = (unsigned char)(v >> 8);
            break;
        }
        d = b64val((unsigned char)s[i + 3]);
        if (d < 0) {
            return -1;
        }
        v = ((unsigned long)a << 18) | ((unsigned long)b << 12) | ((unsigned long)c << 6) | (unsigned long)d;
        out[o++] = (unsigned char)(v >> 16);
        out[o++] = (unsigned char)(v >> 8);
        out[o++] = (unsigned char)v;
    }
    *outlen = o;
    return 0;
}
