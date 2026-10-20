/* Kallisti's remembered settings: see kprefs.h. */
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "kprefs.h"
#include "oscompat.h"

void kprefs_defaults(KPrefs *p)
{
    memset(p, 0, sizeof(*p));
    p->is_bot = 1;
    p->check_dates = 1;
    p->emoji_pictures = 1;
    p->inline_images = 1;
}

static void copy_value(char *dst, size_t cap, const char *v)
{
    size_t n = strlen(v);

    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, v, n);
    dst[n] = '\0';
}

int kprefs_load(KPrefs *p, const char *path)
{
    FILE *f;
    char line[512], *eq, *end;

    if (p == NULL || path == NULL) {
        return -1;
    }
    kprefs_defaults(p);
    f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r')) {
            *--end = '\0';
        }
        eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq++ = '\0';
        if (strcmp(line, "token") == 0) {
            copy_value(p->token, sizeof(p->token), eq);
        } else if (strcmp(line, "remember") == 0) {
            p->remember = eq[0] == '1';
        } else if (strcmp(line, "bot") == 0) {
            p->is_bot = eq[0] != '0';
        } else if (strcmp(line, "proxy") == 0) {
            copy_value(p->proxy, sizeof(p->proxy), eq);
        } else if (strcmp(line, "proxy_user") == 0) {
            copy_value(p->proxy_user, sizeof(p->proxy_user), eq);
        } else if (strcmp(line, "proxy_pass") == 0) {
            copy_value(p->proxy_pass, sizeof(p->proxy_pass), eq);
        } else if (strcmp(line, "emoji") == 0) {
            p->emoji_pictures = eq[0] != '0';
        } else if (strcmp(line, "images") == 0) {
            p->inline_images = eq[0] != '0';
        } else if (strcmp(line, "dates") == 0) {
            p->check_dates = eq[0] != '0';
        }
    }
    fclose(f);
    if (!p->remember) {
        memset(p->token, 0, sizeof(p->token));       /* a token without the user's say-so is not used */
        memset(p->proxy_pass, 0, sizeof(p->proxy_pass));
    }
    return 0;
}

int kprefs_save(const KPrefs *p, const char *path)
{
    FILE *f;

    if (p == NULL || path == NULL) {
        return -1;
    }
    f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }
    chmod(path, 0600);                               /* before anything secret goes in */
    fprintf(f, "bot=%d\n", p->is_bot ? 1 : 0);
    fprintf(f, "dates=%d\n", p->check_dates ? 1 : 0);
    fprintf(f, "emoji=%d\n", p->emoji_pictures ? 1 : 0);
    fprintf(f, "images=%d\n", p->inline_images ? 1 : 0);
    fprintf(f, "remember=%d\n", p->remember ? 1 : 0);
    if (p->proxy[0] != '\0') {
        fprintf(f, "proxy=%s\n", p->proxy);
    }
    if (p->proxy_user[0] != '\0') {
        fprintf(f, "proxy_user=%s\n", p->proxy_user);
    }
    if (p->remember && p->proxy_pass[0] != '\0') {
        fprintf(f, "proxy_pass=%s\n", p->proxy_pass);
    }
    if (p->remember && p->token[0] != '\0') {
        fprintf(f, "token=%s\n", p->token);
    }
    if (fclose(f) != 0) {
        return -1;
    }
    return 0;
}
