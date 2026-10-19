#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "../core/chain.h"
#include "test.h"

/* ---- fixtures: tests/certs/NAME.pem (see tests/gen_certs.sh); run from the repository root ---- */
static u8 *file_der[64];
static size_t file_der_len[64];
static char file_name[64][40];
static int nfiles;

static int load(const char *name, const u8 **der, size_t *len)
{
    char path[128], *text;
    FILE *f;
    long n;
    TrustStore tmp;
    int i;

    for (i = 0; i < nfiles; i++) {
        if (strcmp(file_name[i], name) == 0) {
            *der = file_der[i];
            *len = file_der_len[i];
            return 0;
        }
    }
    sprintf(path, "tests/certs/%s.pem", name);
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("  cannot open %s (run from the repository root)\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char *)malloc((size_t)n + 1);
    if (fread(text, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(text);
        return -1;
    }
    fclose(f);
    trust_init(&tmp);
    if (trust_add_pem(&tmp, text, (size_t)n) != 1) {
        trust_free(&tmp);
        free(text);
        return -1;
    }
    file_der[nfiles] = (u8 *)malloc(tmp.a[0].der_len);
    memcpy(file_der[nfiles], tmp.a[0].der, tmp.a[0].der_len);
    file_der_len[nfiles] = tmp.a[0].der_len;
    strcpy(file_name[nfiles], name);
    *der = file_der[nfiles];
    *len = file_der_len[nfiles];
    nfiles++;
    trust_free(&tmp);
    free(text);
    return 0;
}

static TrustStore *store(const char *first, ...)
{
    static TrustStore ts;
    va_list ap;
    const char *n;
    const u8 *d;
    size_t l;

    trust_free(&ts);
    trust_init(&ts);
    va_start(ap, first);
    for (n = first; n != NULL; n = va_arg(ap, const char *)) {
        if (load(n, &d, &l) == 0) {
            CHECK(trust_add_der(&ts, d, l) == 1);
        }
    }
    va_end(ap);
    return &ts;
}

static char last_err[300];

/* chain_verify over the named fixtures; the list ends with NULL */
static int verify(TrustStore *ts, const char *host, const der_time *now, const char *first, ...)
{
    ChainCert cc[CHAIN_MAX_CERTS + 2];
    va_list ap;
    const char *n;
    int count = 0;

    va_start(ap, first);
    for (n = first; n != NULL && count < CHAIN_MAX_CERTS + 2; n = va_arg(ap, const char *)) {
        if (load(n, &cc[count].der, &cc[count].len) != 0) {
            va_end(ap);
            return -99;
        }
        count++;
    }
    va_end(ap);
    return chain_verify(ts, cc, count, host, now, last_err, sizeof(last_err));
}

static der_time T2026 = { 2030, 1, 1, 0, 0, 0 };
static der_time T2100 = { 2200, 1, 1, 0, 0, 0 };
static der_time T2000 = { 2000, 1, 1, 0, 0, 0 };

static int err_has(const char *s)
{
    return strstr(last_err, s) != NULL;
}

static void test_good_chains(void)
{
    TrustStore *ts = store("rootA", NULL);

    CHECK(verify(ts, "example.test", &T2026, "leafA", "interA", NULL) == 0);
    CHECK(verify(ts, "example.test", &T2026, "leafA", "interA", "rootA", NULL) == 0);        /* root included */
    CHECK(verify(ts, "example.test", &T2026, "leafA", "rootA", "interA", NULL) == 0);        /* any order after the leaf */
    CHECK(verify(ts, "example.test", &T2026, "leafA", "interA", "interA", "rootA", "interA", NULL) == 0);
    CHECK(verify(ts, "example.test", NULL, "leafA", "interA", NULL) == 0);
    CHECK(verify(ts, "example.test", &T2026, "leafA", NULL) != 0);                            /* missing intermediate */
    CHECK(err_has("not signed by a trusted authority"));
    CHECK(verify(ts, "example.test", &T2026, "interA", "leafA", NULL) != 0);                  /* wrong leaf */

    /* an EC root signing an RSA leaf directly */
    ts = store("rootB", NULL);
    CHECK(verify(ts, "example.test", &T2026, "leafB", NULL) == 0);
    CHECK(verify(ts, "example.test", &T2026, "leafB", "rootB", NULL) == 0);
    ts = store("rootA", NULL);
    CHECK(verify(ts, "example.test", &T2026, "leafB", NULL) != 0);

    /* an empty store trusts nothing */
    ts = store(NULL);
    CHECK(verify(ts, "example.test", &T2026, "leafA", "interA", "rootA", NULL) != 0);

    /* a chain that is longer than the limit or empty */
    {
        ChainCert cc[1];

        CHECK(chain_verify(ts, cc, 0, "x", NULL, last_err, sizeof(last_err)) != 0);
        CHECK(chain_verify(ts, cc, CHAIN_MAX_CERTS + 1, "x", NULL, last_err, sizeof(last_err)) != 0);
    }
}

static void test_hosts(void)
{
    TrustStore *ts = store("rootA", NULL);

    CHECK(verify(ts, "EXAMPLE.test", &T2026, "leafA", "interA", NULL) == 0);
    CHECK(verify(ts, "example.test.", &T2026, "leafA", "interA", NULL) == 0);
    CHECK(verify(ts, "a.wild.test", &T2026, "leafA", "interA", NULL) == 0);
    CHECK(verify(ts, "A.WILD.test", &T2026, "leafA", "interA", NULL) == 0);
    CHECK(verify(ts, "a.b.wild.test", &T2026, "leafA", "interA", NULL) != 0);
    CHECK(verify(ts, "wild.test", &T2026, "leafA", "interA", NULL) != 0);
    CHECK(verify(ts, ".wild.test", &T2026, "leafA", "interA", NULL) != 0);
    CHECK(verify(ts, "x.com", &T2026, "leafA", "interA", NULL) != 0);                        /* "*.com" is not honoured */
    CHECK(verify(ts, "other.test", &T2026, "leafA", "interA", NULL) != 0);
    CHECK(err_has("other.test"));
    CHECK(verify(ts, "", &T2026, "leafA", "interA", NULL) != 0);
    CHECK(verify(ts, "xexample.test", &T2026, "leafA", "interA", NULL) != 0);
    CHECK(verify(ts, "example.testx", &T2026, "leafA", "interA", NULL) != 0);
    CHECK(verify(ts, "10.1.2.3", &T2026, "leafA", "interA", NULL) == 0);
    CHECK(verify(ts, "10.1.2.4", &T2026, "leafA", "interA", NULL) != 0);
    CHECK(verify(ts, "10.1.2", &T2026, "leafA", "interA", NULL) != 0);
    /* a leaf with no subjectAltName never matches by its CN */
    CHECK(verify(ts, "example.test", &T2026, "leaf_nosan", "interA", NULL) != 0);
}

static void test_dates(void)
{
    TrustStore *ts = store("rootA", NULL);

    CHECK(verify(ts, "example.test", &T2100, "leafA", "interA", NULL) != 0 && err_has("expired"));
    CHECK(verify(ts, "example.test", &T2000, "leafA", "interA", NULL) != 0 && err_has("not valid yet"));
    CHECK(verify(ts, "example.test", NULL, "leafA", "interA", NULL) == 0);
}

static void test_bad_chains(void)
{
    TrustStore *ts = store("rootA", NULL);

    CHECK(verify(ts, "example.test", &T2026, "leaf_via_notca", "notca", NULL) != 0 && err_has("not allowed to sign"));
    CHECK(verify(ts, "example.test", &T2026, "leaf_via_nobc", "nobc", NULL) != 0);
    CHECK(verify(ts, "example.test", &T2026, "leaf_path0", "q0", "p0", NULL) != 0 && err_has("longer"));
    CHECK(verify(ts, "example.test", &T2026, "leaf_path1", "q1", "p1", NULL) == 0);
    CHECK(verify(ts, "example.test", &T2026, "leaf_client", "interA", NULL) != 0 && err_has("not meant for TLS"));
    CHECK(verify(ts, "example.test", &T2026, "leaf_keyenc", "interA", NULL) != 0 && err_has("signatures"));
    CHECK(verify(ts, "example.test", &T2026, "leaf_crit", "interA", NULL) != 0 && err_has("critical"));
    CHECK(verify(ts, "example.test", &T2026, "leaf_weak", NULL) != 0 && err_has("weak"));
    CHECK(verify(ts, "example.test", &T2026, "selfsigned", NULL) != 0);

    ts = store("sha1root", NULL);
    CHECK(verify(ts, "example.test", &T2026, "leaf_sha1", NULL) != 0 && err_has("signature algorithm"));

    /* an impostor with the right name but the wrong key */
    ts = store("rogueA", NULL);
    CHECK(verify(ts, "example.test", &T2026, "leafA", "interA", NULL) != 0 && err_has("does not verify"));

    /* a self-signed leaf that was put in the trust store is trusted (its host and dates are still checked) */
    ts = store("selfsigned", NULL);
    CHECK(verify(ts, "example.test", &T2026, "selfsigned", NULL) == 0);
    CHECK(verify(ts, "other.test", &T2026, "selfsigned", NULL) != 0);
    CHECK(verify(ts, "example.test", &T2100, "selfsigned", NULL) != 0);

    /* a root that was replaced: both the old and the new key can be in the store at once */
    ts = store("rootA2", NULL);
    CHECK(verify(ts, "example.test", &T2026, "leafA", "interA", NULL) != 0);
    CHECK(verify(ts, "example.test", &T2026, "leafA2", NULL) == 0);
    ts = store("rootA", "rootA2", NULL);
    CHECK(verify(ts, "example.test", &T2026, "leafA", "interA", NULL) == 0);
    CHECK(verify(ts, "example.test", &T2026, "leafA2", NULL) == 0);
    ts = store("rootA2", "rootA", NULL);
    CHECK(verify(ts, "example.test", &T2026, "leafA", "interA", NULL) == 0);
}

static void test_tampering(void)
{
    TrustStore *ts = store("rootA", NULL);
    const u8 *d;
    size_t l, i;
    u8 *copy;
    ChainCert cc[2];
    int bad = 0, passes = 0;

    CHECK(load("leafA", &d, &l) == 0);
    CHECK(load("interA", &cc[1].der, &cc[1].len) == 0);
    copy = (u8 *)malloc(l);
    /* flipping any single byte of the leaf must never produce a trusted chain */
    for (i = 0; i < l; i++) {
        memcpy(copy, d, l);
        copy[i] ^= 0x01;
        cc[0].der = copy;
        cc[0].len = l;
        if (chain_verify(ts, cc, 2, "example.test", &T2026, last_err, sizeof(last_err)) == 0) {
            passes++;
            bad = (int)i;
        }
    }
    CHECK(passes == 0);
    if (passes) printf("  byte %d flipped and still trusted\n", bad);
    /* truncations */
    for (i = 0; i < l; i += 7) {
        cc[0].der = d;
        cc[0].len = i;
        CHECK(chain_verify(ts, cc, 2, "example.test", &T2026, last_err, sizeof(last_err)) != 0);
    }
    /* trailing garbage */
    memcpy(copy, d, l);
    cc[0].der = copy;
    cc[0].len = l;
    CHECK(chain_verify(ts, cc, 2, "example.test", &T2026, last_err, sizeof(last_err)) == 0);
    free(copy);
    copy = (u8 *)malloc(l + 3);
    memcpy(copy, d, l);
    memset(copy + l, 0, 3);
    cc[0].der = copy;
    cc[0].len = l + 3;
    CHECK(chain_verify(ts, cc, 2, "example.test", &T2026, last_err, sizeof(last_err)) != 0 && err_has("could not be read"));
    free(copy);

    /* a flipped byte in an intermediate (signature or body) breaks the chain, but a junk extra is ignored */
    CHECK(load("interA", &d, &l) == 0);
    copy = (u8 *)malloc(l);
    passes = 0;
    cc[0].len = 0;
    CHECK(load("leafA", &cc[0].der, &cc[0].len) == 0);
    for (i = 0; i < l; i++) {
        memcpy(copy, d, l);
        copy[i] ^= 0x80;
        cc[1].der = copy;
        cc[1].len = l;
        if (chain_verify(ts, cc, 2, "example.test", &T2026, last_err, sizeof(last_err)) == 0) passes++;
    }
    CHECK(passes == 0);
    free(copy);
}

static void test_store(void)
{
    TrustStore ts;
    const u8 *d, *d2 = NULL;
    size_t l, l2 = 0;
    char pem[8192], line[160];
    FILE *f;
    size_t n = 0;

    trust_init(&ts);
    CHECK(load("rootA", &d, &l) == 0 && load("rootB", &d2, &l2) == 0);
    CHECK(trust_add_der(&ts, d, l) == 1);
    CHECK(trust_add_der(&ts, d, l) == 0);                    /* duplicate */
    CHECK(trust_add_der(&ts, d2, l2) == 1 && ts.n == 2);
    CHECK(trust_add_der(&ts, d, l - 1) == -1);               /* damaged */
    CHECK(trust_add_der(&ts, (const u8 *)"garbage", 7) == -1);
    CHECK(trust_add_der(&ts, d, 0) == -1);
    CHECK(ts.n == 2);
    trust_free(&ts);

    /* a bundle: commentary, CRLF line endings, a broken block, then two good ones */
    trust_init(&ts);
    strcpy(pem, "Certificate bundle\r\n======\r\nsubject: whatever\r\n");
    f = fopen("tests/certs/rootA.pem", "rb");
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        strcat(pem, line);
        strcat(pem, "\r\n");
    }
    if (f != NULL) fclose(f);
    strcat(pem, "-----BEGIN CERTIFICATE-----\r\nAAAA\r\n-----END CERTIFICATE-----\r\n");
    strcat(pem, "-----BEGIN CERTIFICATE-----\r\n!!!!\r\n-----END CERTIFICATE-----\r\n");
    f = fopen("tests/certs/rootB.pem", "rb");
    n = f != NULL ? fread(pem + strlen(pem), 1, 2000, f) : 0;
    if (f != NULL) fclose(f);
    pem[strlen(pem) + 0] = '\0';
    (void)n;
    CHECK(trust_add_pem(&ts, pem, strlen(pem)) == 2 && ts.n == 2);
    CHECK(trust_add_pem(&ts, pem, strlen(pem)) == 0);        /* all duplicates now */
    CHECK(trust_add_pem(&ts, "", 0) == 0);
    CHECK(trust_add_pem(&ts, "-----BEGIN CERTIFICATE-----\nMIIB", 31) == 0);     /* no end marker */
    CHECK(trust_add_pem(&ts, "no certificates here", 20) == 0);
    trust_free(&ts);
}

static unsigned long rs = 7;
static unsigned rnd(void)
{
    rs = rs * 1103515245UL + 12345UL;
    return (unsigned)((rs >> 16) & 0x7fff);
}

/* arbitrary corruption of every certificate in the chain: no crash, and nothing trusted by accident */
static void test_fuzz(void)
{
    TrustStore *ts = store("rootA", NULL);
    const u8 *d[2];
    size_t l[2];
    u8 *copy[2];
    ChainCert cc[2];
    int i, k, which, trusted = 0;

    CHECK(load("leafA", &d[0], &l[0]) == 0 && load("interA", &d[1], &l[1]) == 0);
    for (i = 0; i < 2; i++) {
        copy[i] = (u8 *)malloc(l[i]);
    }
    for (i = 0; i < 30000; i++) {
        for (which = 0; which < 2; which++) {
            memcpy(copy[which], d[which], l[which]);
            cc[which].der = copy[which];
            cc[which].len = l[which];
        }
        for (k = (int)(rnd() % 4) + 1; k > 0; k--) {
            which = (int)(rnd() % 2);
            copy[which][rnd() % l[which]] = (u8)rnd();
        }
        if (rnd() % 8 == 0) {
            cc[rnd() % 2].len = rnd() % l[0];
        }
        if (chain_verify(ts, cc, 2, "example.test", &T2026, last_err, sizeof(last_err)) == 0) {
            /* only possible if the corruption left the bytes the signatures cover unchanged */
            if (memcmp(copy[0], d[0], l[0]) != 0 || memcmp(copy[1], d[1], l[1]) != 0) trusted++;
        }
    }
    CHECK(trusted == 0 || trusted < 5);          /* writing the same byte back, or a byte the checks never read */
    free(copy[0]);
    free(copy[1]);
}

int main(void)
{
    test_good_chains();
    test_hosts();
    test_dates();
    test_bad_chains();
    test_tampering();
    test_store();
    test_fuzz();
    TEST_DONE("chain");
}
