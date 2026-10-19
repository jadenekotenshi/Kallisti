#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../core/roots.h"
#include "test.h"

static der_time NOW = { 2026, 10, 19, 18, 0, 0 };        /* when the real chains below were captured */
static char err[300];

static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long len;

    if (f == NULL) {
        printf("  cannot open %s (run from the repository root)\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)len + 1);
    if (fread(b, 1, (size_t)len, f) != (size_t)len) {
        len = 0;
    }
    fclose(f);
    b[len] = '\0';
    *n = (size_t)len;
    return b;
}

static int write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return -1;
    }
    fwrite(data, 1, n, f);
    fclose(f);
    return 0;
}

/* verifies the PEM chain file (leaf first) against ts */
static int verify_pem(const TrustStore *ts, const char *path, const char *host, const der_time *now)
{
    size_t n;
    char *text = slurp(path, &n);
    TrustStore chain;
    ChainCert cc[CHAIN_MAX_CERTS];
    int i, rc;

    if (text == NULL) {
        return -99;
    }
    trust_init(&chain);
    trust_add_pem(&chain, text, n);
    free(text);
    for (i = 0; i < chain.n; i++) {
        cc[i].der = chain.a[i].der;
        cc[i].len = chain.a[i].der_len;
    }
    rc = chain_verify(ts, cc, chain.n, host, now, err, sizeof(err));
    trust_free(&chain);
    return rc;
}

static void test_bundled(void)
{
    TrustStore ts;
    int n;

    trust_init(&ts);
    n = roots_bundled_load(&ts);
    CHECK(n >= 25 && ts.n == n);
    CHECK(roots_bundled_load(&ts) == 0);                       /* nothing new the second time */
    /* the real endpoints, as captured: three certificates each, ending in the GTS Root R4 cross-sign */
    CHECK(verify_pem(&ts, "tests/certs/real_discord.com.pem", "discord.com", &NOW) == 0);
    CHECK(verify_pem(&ts, "tests/certs/real_gateway.discord.gg.pem", "gateway.discord.gg", &NOW) == 0);
    CHECK(verify_pem(&ts, "tests/certs/real_discord.com.pem", "discord.com", NULL) == 0);
    CHECK(verify_pem(&ts, "tests/certs/real_discord.com.pem", "evil.example", &NOW) != 0);
    CHECK(verify_pem(&ts, "tests/certs/real_discord.com.pem", "gateway.discord.gg", &NOW) != 0);
    {
        der_time later = { 2030, 1, 1, 0, 0, 0 };
        der_time early = { 2026, 1, 1, 0, 0, 0 };

        CHECK(verify_pem(&ts, "tests/certs/real_discord.com.pem", "discord.com", &later) != 0 && strstr(err, "expired") != NULL);
        CHECK(verify_pem(&ts, "tests/certs/real_discord.com.pem", "discord.com", &early) != 0 && strstr(err, "not valid yet") != NULL);
    }
    trust_free(&ts);

    /* a store without Google's roots cannot validate them (the chain's own root is not trusted by itself) */
    trust_init(&ts);
    CHECK(verify_pem(&ts, "tests/certs/real_discord.com.pem", "discord.com", &NOW) != 0);
    trust_free(&ts);
}

static void test_directory(void)
{
    TrustStore ts;
    char *text;
    size_t n;
    int count = 0, src;
    const char *base = "build/roots_test";

    mkdir("build", 0777);
    system("rm -rf build/roots_test");
    mkdir(base, 0777);

    /* nothing there: the bundled roots */
    trust_init(&ts);
    src = roots_load_default(&ts, base, &count);
    CHECK(src == ROOTS_FROM_BUNDLED && count == ts.n && count >= 25);
    trust_free(&ts);
    trust_init(&ts);
    src = roots_load_default(&ts, "build/roots_test_missing", &count);
    CHECK(src == ROOTS_FROM_BUNDLED);
    trust_free(&ts);
    trust_init(&ts);
    src = roots_load_default(&ts, NULL, &count);
    CHECK(src == ROOTS_FROM_BUNDLED);
    trust_free(&ts);

    /* a directory with junk and no certificates is still "nothing there" */
    write_file("build/roots_test/openssl.cnf", "[ca]\n", 5);
    write_file("build/roots_test/cert.pem", "not a certificate\n", 18);
    trust_init(&ts);
    CHECK(roots_load_default(&ts, base, &count) == ROOTS_FROM_BUNDLED);
    trust_free(&ts);

    /* an OpenSSL-style layout: a bundle, a certs/ directory with PEM, DER and hashed names */
    mkdir("build/roots_test/certs", 0777);
    text = slurp("tests/certs/rootA.pem", &n);
    {
        char *two = (char *)malloc(n * 2 + 64);

        strcpy(two, "# bundle\n");
        strcat(two, text);
        free(text);
        text = slurp("tests/certs/rootB.pem", &n);
        strcat(two, text);
        write_file("build/roots_test/cert.pem", two, strlen(two));
        free(two);
    }
    free(text);
    text = slurp("tests/certs/sha1root.pem", &n);
    write_file("build/roots_test/certs/old.crt", text, n);
    write_file("build/roots_test/certs/12345678.0", text, n);                 /* a duplicate, under a hashed name */
    write_file("build/roots_test/certs/notes.txt", text, n);                  /* ignored: wrong extension */
    free(text);
    {
        TrustStore one;
        text = slurp("tests/certs/rootA2.pem", &n);
        trust_init(&one);
        trust_add_pem(&one, text, n);
        write_file("build/roots_test/certs/der.cer", one.a[0].der, one.a[0].der_len);
        trust_free(&one);
        free(text);
    }
    trust_init(&ts);
    src = roots_load_default(&ts, base, &count);
    CHECK(src == ROOTS_FROM_DIR && count == 4 && ts.n == 4);                   /* rootA, rootB, sha1root, rootA2 */
    CHECK(verify_pem(&ts, "tests/certs/leafA.pem", "example.test", NULL) != 0);        /* no intermediate */
    trust_free(&ts);
    /* the directory wins outright: the real chain is not trusted by it */
    trust_init(&ts);
    roots_load_default(&ts, base, &count);
    CHECK(verify_pem(&ts, "tests/certs/real_discord.com.pem", "discord.com", &NOW) != 0);
    trust_free(&ts);
    system("rm -rf build/roots_test");
}

int main(void)
{
    test_bundled();
    test_directory();
    TEST_DONE("roots");
}
