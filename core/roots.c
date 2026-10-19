/* Loading the trust store from disk, or from the compiled-in roots: see roots.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#ifdef OPENSTEP
#include <sys/dir.h>        /* this system's <dirent.h> does not declare DIR; the BSD header does */
#endif
#include "roots.h"

#ifdef OPENSTEP
typedef struct direct roots_dirent;
#else
typedef struct dirent roots_dirent;
#endif

#define MAX_FILE (4L * 1024 * 1024)

static const char *BUNDLE_NAMES[] = { "cert.pem", "certs.pem", "ca-bundle.crt", "ca-certificates.crt", "curl-ca-bundle.crt", NULL };

/* Reads a whole file (up to MAX_FILE bytes) into a malloc'd buffer, or NULL. */
static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || n > MAX_FILE || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)n + 1);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len = (size_t)n;
    return buf;
}

/* A PEM file with any number of certificates, or a single DER certificate. */
static int add_file(TrustStore *ts, const char *path)
{
    size_t len;
    unsigned char *data = read_file(path, &len);
    int added;

    if (data == NULL) {
        return 0;
    }
    added = trust_add_pem(ts, (const char *)data, len);
    if (added == 0 && len > 4 && data[0] == 0x30) {
        added = trust_add_der(ts, data, len) == 1 ? 1 : 0;
    }
    free(data);
    return added;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);

    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* OpenSSL's hashed names: eight hex digits, a dot, a number. */
static int hashed_name(const char *s)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return 0;
        }
    }
    if (s[8] != '.' || s[9] == '\0') {
        return 0;
    }
    for (i = 9; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    return 1;
}

static int scan_dir(TrustStore *ts, const char *dir)
{
    DIR *dp = opendir(dir);
    roots_dirent *de;
    char path[1024];
    int added = 0;

    if (dp == NULL) {
        return 0;
    }
    while ((de = readdir(dp)) != NULL) {
        const char *name = de->d_name;

        if (name[0] == '.' || strlen(dir) + strlen(name) + 2 > sizeof(path)) {
            continue;
        }
        if (ends_with(name, ".pem") || ends_with(name, ".crt") || ends_with(name, ".cer") || hashed_name(name)) {
            sprintf(path, "%s/%s", dir, name);
            added += add_file(ts, path);
        }
    }
    closedir(dp);
    return added;
}

int roots_load_dir(TrustStore *ts, const char *dir)
{
    char path[1024];
    int i, added = 0;

    if (strlen(dir) + 32 > sizeof(path)) {
        return 0;
    }
    for (i = 0; BUNDLE_NAMES[i] != NULL; i++) {
        sprintf(path, "%s/%s", dir, BUNDLE_NAMES[i]);
        added += add_file(ts, path);
        sprintf(path, "%s/certs/%s", dir, BUNDLE_NAMES[i]);
        added += add_file(ts, path);
    }
    sprintf(path, "%s/certs", dir);
    added += scan_dir(ts, path);
    added += scan_dir(ts, dir);
    return added;
}

int roots_load_default(TrustStore *ts, const char *dir, int *count)
{
    int n = dir != NULL ? roots_load_dir(ts, dir) : 0;

    if (n > 0) {
        *count = n;
        return ROOTS_FROM_DIR;
    }
    n = roots_bundled_load(ts);
    *count = n;
    return n > 0 ? ROOTS_FROM_BUNDLED : ROOTS_FROM_NONE;
}
