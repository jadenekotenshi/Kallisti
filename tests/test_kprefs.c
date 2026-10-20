#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../core/kprefs.h"
#include "test.h"

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    fputs(text, f);
    fclose(f);
}

int main(void)
{
    KPrefs p, q;
    struct stat st;
    const char *path = "build/test_kprefs.txt";
    char big[1200];

    mkdir("build", 0777);
    remove(path);

    kprefs_defaults(&p);
    CHECK(p.is_bot == 1 && p.remember == 0 && p.token[0] == '\0' && p.check_dates == 1);
    CHECK(kprefs_load(&q, path) == 0 && q.is_bot == 1 && !q.remember && q.token[0] == '\0');       /* no file */
    CHECK(kprefs_load(NULL, path) == -1 && kprefs_load(&q, NULL) == -1);

    /* remembered token round-trips, and the file is private */
    strcpy(p.token, "abc.def-123");
    p.remember = 1;
    p.is_bot = 0;
    p.check_dates = 0;
    strcpy(p.proxy, "socks5://proxy.lan:1080");
    strcpy(p.proxy_user, "bob");
    strcpy(p.proxy_pass, "s3cret");
    CHECK(kprefs_save(&p, path) == 0);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 077) == 0);
    CHECK(kprefs_load(&q, path) == 0 && q.remember && !q.is_bot && !q.check_dates && strcmp(q.token, "abc.def-123") == 0);
    CHECK(strcmp(q.proxy, "socks5://proxy.lan:1080") == 0 && strcmp(q.proxy_user, "bob") == 0 && strcmp(q.proxy_pass, "s3cret") == 0);

    /* not remembered: the token never reaches the file */
    p.remember = 0;
    CHECK(kprefs_save(&p, path) == 0);
    {
        FILE *f = fopen(path, "r");
        char buf[256];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);

        buf[n] = '\0';
        fclose(f);
        CHECK(strstr(buf, "abc.def") == NULL && strstr(buf, "remember=0") != NULL);
        CHECK(strstr(buf, "s3cret") == NULL && strstr(buf, "proxy=socks5://proxy.lan:1080") != NULL && strstr(buf, "proxy_user=bob") != NULL);
    }
    CHECK(kprefs_load(&q, path) == 0 && !q.remember && q.token[0] == '\0' && q.proxy_pass[0] == '\0' && strcmp(q.proxy_user, "bob") == 0);

    /* a token in a file that does not say remember=1 is ignored; junk is skipped; CRLF is fine */
    write_text(path, "token=sneaky\r\nbot=1\r\nnonsense\r\n=x\r\nunknown=5\r\n");
    CHECK(kprefs_load(&q, path) == 0 && q.token[0] == '\0' && q.is_bot);
    write_text(path, "remember=1\r\ntoken=kept\r\nbot=0\r\n");
    CHECK(kprefs_load(&q, path) == 0 && strcmp(q.token, "kept") == 0 && !q.is_bot);

    /* an over-long token is cut, not overflowed */
    memset(big, 'x', sizeof(big));
    memcpy(big, "remember=1\ntoken=", 17);
    big[sizeof(big) - 2] = '\n';
    big[sizeof(big) - 1] = '\0';
    write_text(path, big);
    CHECK(kprefs_load(&q, path) == 0 && strlen(q.token) == sizeof(q.token) - 1);

    /* saving over an unwritable path fails cleanly */
    CHECK(kprefs_save(&p, "build/no/such/dir/prefs") == -1);
    remove(path);
    TEST_DONE("kprefs");
}
