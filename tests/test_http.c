#include <stdlib.h>
#include <string.h>
#include "../core/http.h"
#include "test.h"

/* Feeds text to a fresh parser in pieces of `step` bytes.  Returns the last result; *consumed is how many
 * bytes the parser took (it stops taking bytes once the response is complete). */
static int run(const char *text, size_t len, size_t step, int head, size_t max_body, int do_eof,
               HttpResp *r, size_t *consumed)
{
    size_t pos = 0, used, n;
    int rc = HTTP_NEED_MORE;

    http_resp_init(r, max_body, head);
    while (pos < len) {
        n = len - pos < step ? len - pos : step;
        rc = http_resp_feed(r, (const unsigned char *)text + pos, n, &used);
        pos += used;
        if (rc != HTTP_NEED_MORE) {
            break;
        }
    }
    if (rc == HTTP_NEED_MORE && do_eof) {
        rc = http_resp_eof(r);
    }
    *consumed = pos;
    return rc;
}

/* The same expectations must hold for every way of cutting the input. */
static void check_all_splits(const char *text, int head, size_t max_body, int do_eof, int want_rc,
                             int want_status, const char *want_body, size_t want_consumed,
                             const char *what)
{
    size_t len = strlen(text), step, consumed;
    int bad = 0;
    HttpResp r;

    if (want_consumed == (size_t)-1) {
        want_consumed = len;
    }

    for (step = 1; step <= len + 1; step++) {
        int rc = run(text, len, step, head, max_body, do_eof, &r, &consumed);

        if (rc != want_rc || (want_rc == HTTP_DONE && (r.status != want_status ||
            r.body_len != strlen(want_body) || (r.body_len && memcmp(r.body, want_body, r.body_len) != 0) ||
            consumed != want_consumed))) {
            if (!bad) {
                printf("  split failure (%s): step=%lu rc=%d status=%d bodylen=%lu consumed=%lu err=%s\n",
                       what, (unsigned long)step, rc, r.status, (unsigned long)r.body_len,
                       (unsigned long)consumed, r.err);
            }
            bad = 1;
        }
        http_resp_free(&r);
    }
    CHECK(!bad);
}

static void test_urls(void)
{
    HttpUrl u;

    CHECK(http_parse_url("https://discord.com/api/v10/gateway/bot", &u) == 0);
    CHECK(strcmp(u.scheme, "https") == 0 && strcmp(u.host, "discord.com") == 0 && u.port == 443 &&
          u.tls == 1 && strcmp(u.path, "/api/v10/gateway/bot") == 0);
    CHECK(http_parse_url("wss://gateway.discord.gg/?v=10&encoding=json", &u) == 0);
    CHECK(strcmp(u.scheme, "wss") == 0 && u.port == 443 && u.tls && strcmp(u.path, "/?v=10&encoding=json") == 0);
    CHECK(http_parse_url("wss://gateway.discord.gg", &u) == 0 && strcmp(u.path, "/") == 0);
    CHECK(http_parse_url("HTTP://Example.COM:8080/a?b=c#frag", &u) == 0);
    CHECK(strcmp(u.scheme, "http") == 0 && strcmp(u.host, "Example.COM") == 0 && u.port == 8080 && !u.tls &&
          strcmp(u.path, "/a?b=c") == 0);
    CHECK(http_parse_url("ws://localhost", &u) == 0 && u.port == 80 && !u.tls);
    CHECK(http_parse_url("https://h?x=1", &u) == 0 && strcmp(u.path, "/?x=1") == 0);
    CHECK(http_parse_url("http://[::1]:9000/x", &u) == 0 && strcmp(u.host, "::1") == 0 && u.port == 9000 &&
          strcmp(u.path, "/x") == 0);
    CHECK(http_parse_url("http://[2001:db8::1]/", &u) == 0 && strcmp(u.host, "2001:db8::1") == 0 && u.port == 80);
    CHECK(http_parse_url("https://h/#only", &u) == 0 && strcmp(u.path, "/") == 0);

    CHECK(http_parse_url("discord.com/api", &u) == -1);
    CHECK(http_parse_url("ftp://h/x", &u) == -1);
    CHECK(http_parse_url("https:///x", &u) == -1);
    CHECK(http_parse_url("https://user@h/x", &u) == -1);
    CHECK(http_parse_url("https://h:0/x", &u) == -1);
    CHECK(http_parse_url("https://h:65536/x", &u) == -1);
    CHECK(http_parse_url("https://h:80a/x", &u) == -1);
    CHECK(http_parse_url("https://h:/x", &u) == -1);
    CHECK(http_parse_url("https://h/a b", &u) == -1);
    CHECK(http_parse_url("https://h/a\r\nHost: evil", &u) == -1);
    CHECK(http_parse_url("http://[::1/x", &u) == -1);
    CHECK(http_parse_url("http://[]/x", &u) == -1);
    CHECK(http_parse_url("http://[::1]x/", &u) == -1);
    CHECK(http_parse_url("", &u) == -1 && http_parse_url(NULL, &u) == -1);
    {
        char longurl[2200];
        memset(longurl, 'a', sizeof(longurl));
        memcpy(longurl, "http://h/", 9);
        longurl[sizeof(longurl) - 1] = '\0';
        CHECK(http_parse_url(longurl, &u) == -1);
    }
}

static void test_requests(void)
{
    HttpReq r;

    http_req_init(&r);
    CHECK(http_req_begin(&r, "GET", "/api/v10/gateway/bot", "discord.com", 443, 1) == 0);
    CHECK(http_req_header(&r, "Authorization", "Bot abc.def") == 0);
    CHECK(http_req_header(&r, "User-Agent", "Kallisti (OPENSTEP, 0.1)") == 0);
    CHECK(http_req_end(&r, NULL, 0) == 0);
    CHECK(strcmp(r.buf, "GET /api/v10/gateway/bot HTTP/1.1\r\nHost: discord.com\r\nAccept-Encoding: identity\r\n"
                        "Authorization: Bot abc.def\r\nUser-Agent: Kallisti (OPENSTEP, 0.1)\r\n\r\n") == 0);
    http_req_free(&r);

    http_req_init(&r);
    CHECK(http_req_begin(&r, "POST", "/api/v10/channels/1/messages", "discord.com", 443, 1) == 0);
    CHECK(http_req_header(&r, "Content-Type", "application/json") == 0);
    CHECK(http_req_end(&r, "{\"content\":\"hi\"}", 16) == 0);
    CHECK(strstr(r.buf, "Content-Length: 16\r\n\r\n{\"content\":\"hi\"}") != NULL);
    CHECK(r.len == strlen(r.buf));
    http_req_free(&r);

    /* an empty POST still states its length; an empty GET does not */
    http_req_init(&r);
    http_req_begin(&r, "POST", "/x", "h", 80, 0);
    http_req_end(&r, NULL, 0);
    CHECK(strstr(r.buf, "Content-Length: 0\r\n") != NULL && strstr(r.buf, "Host: h\r\n") != NULL);
    http_req_free(&r);
    http_req_init(&r);
    http_req_begin(&r, "GET", "/x", "h", 80, 0);
    http_req_end(&r, NULL, 0);
    CHECK(strstr(r.buf, "Content-Length") == NULL);
    http_req_free(&r);

    /* Host carries the port unless it is the scheme's default; an IPv6 literal gets brackets */
    http_req_init(&r);
    http_req_begin(&r, "GET", "/", "example.com", 8443, 1);
    CHECK(strstr(r.buf, "Host: example.com:8443\r\n") != NULL);
    http_req_free(&r);
    http_req_init(&r);
    http_req_begin(&r, "GET", "/", "example.com", 443, 0);
    CHECK(strstr(r.buf, "Host: example.com:443\r\n") != NULL);
    http_req_free(&r);
    http_req_init(&r);
    http_req_begin(&r, "GET", "/", "::1", 80, 0);
    CHECK(strstr(r.buf, "Host: [::1]\r\n") != NULL);
    http_req_free(&r);

    /* header injection and malformed pieces are refused */
    http_req_init(&r);
    http_req_begin(&r, "GET", "/", "h", 80, 0);
    CHECK(http_req_header(&r, "X-A", "v\r\nX-Evil: 1") == -1 && r.failed);
    CHECK(http_req_end(&r, NULL, 0) == -1);
    http_req_free(&r);
    http_req_init(&r);
    http_req_begin(&r, "GET", "/", "h", 80, 0);
    CHECK(http_req_header(&r, "Bad Name", "v") == -1);
    http_req_free(&r);
    http_req_init(&r);
    CHECK(http_req_begin(&r, "GET", "/a\r\nHost: x", "h", 80, 0) == -1);
    http_req_free(&r);
    http_req_init(&r);
    CHECK(http_req_begin(&r, "GET", "no-slash", "h", 80, 0) == -1);
    http_req_free(&r);
    http_req_init(&r);
    CHECK(http_req_begin(&r, "GE T", "/", "h", 80, 0) == -1);
    http_req_free(&r);
    http_req_init(&r);
    CHECK(http_req_begin(&r, "GET", "/", "h\r\nx", 80, 0) == -1);
    http_req_free(&r);
}

static void test_basic_responses(void)
{
    HttpResp r;
    size_t consumed;
    int rc;

    /* Content-Length */
    check_all_splits("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello",
                     0, 1000, 0, HTTP_DONE, 200, "hello", (size_t)-1, "content-length");
    /* chunked, with an extension and a trailer */
    check_all_splits("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nTrailer: x\r\n\r\n",
                     0, 1000, 0, HTTP_DONE, 200, "hello world", (size_t)-1, "chunked");
    check_all_splits("HTTP/1.1 200 OK\r\ntransfer-encoding: gzip, Chunked\r\n\r\nA\r\n0123456789\r\n0\r\n\r\n",
                     0, 1000, 0, HTTP_DONE, 200, "0123456789", (size_t)-1, "chunked mixed case, hex upper");
    /* read until close: no Content-Length */
    check_all_splits("HTTP/1.0 200 OK\r\n\r\nuntil the end",
                     0, 1000, 1, HTTP_DONE, 200, "until the end", (size_t)-1, "close-delimited");
    /* HEAD: a Content-Length that never arrives */
    check_all_splits("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n", 1, 1000, 0, HTTP_DONE, 200, "", (size_t)-1, "HEAD");
    check_all_splits("HTTP/1.1 204 No Content\r\n\r\n", 0, 1000, 0, HTTP_DONE, 204, "", (size_t)-1, "204");
    check_all_splits("HTTP/1.1 304 Not Modified\r\nContent-Length: 7\r\n\r\n", 0, 1000, 0, HTTP_DONE, 304, "", (size_t)-1, "304");
    check_all_splits("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0, 1000, 0, HTTP_DONE, 200, "", (size_t)-1, "empty body");
    /* an interim 100 Continue is skipped */
    check_all_splits("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok",
                     0, 1000, 0, HTTP_DONE, 201, "ok", (size_t)-1, "100 continue");
    /* a 101 ends at the headers and leaves the WebSocket bytes alone (66 = header block length) */
    {
        const char *t = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: abc=\r\n\r\n\x81\x05hello";
        size_t hlen = strlen(t) - 7;

        rc = run(t, strlen(t), 5, 0, 1000, 0, &r, &consumed);
        CHECK(rc == HTTP_DONE && r.status == 101 && consumed == hlen);
        CHECK(strcmp(http_resp_header(&r, "upgrade"), "websocket") == 0);
        http_resp_free(&r);
        rc = run(t, strlen(t), 1000, 0, 1000, 0, &r, &consumed);
        CHECK(rc == HTTP_DONE && consumed == hlen && memcmp(t + consumed, "\x81\x05hello", 7) == 0);
        http_resp_free(&r);
    }
    /* two responses in one buffer: the parser stops after the first */
    {
        const char *t = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcHTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nz";
        size_t used;

        http_resp_init(&r, 1000, 0);
        rc = http_resp_feed(&r, (const unsigned char *)t, strlen(t), &used);
        CHECK(rc == HTTP_DONE && used == 41 && r.body_len == 3 && memcmp(r.body, "abc", 3) == 0);
        http_resp_free(&r);
        http_resp_init(&r, 1000, 0);
        rc = http_resp_feed(&r, (const unsigned char *)t + used, strlen(t) - used, &used);
        CHECK(rc == HTTP_DONE && r.body_len == 1 && r.body[0] == 'z');
        http_resp_free(&r);
    }
    /* leading blank lines before the status line, and bare-LF line endings */
    check_all_splits("\r\n\r\nHTTP/1.1 200 OK\nContent-Length: 2\n\nhi", 0, 1000, 0, HTTP_DONE, 200, "hi", (size_t)-1, "LF only");
}

static void test_headers_and_flags(void)
{
    HttpResp r;
    size_t consumed;

    run("HTTP/1.1 200 OK\r\nX-RateLimit-Remaining:   4  \r\nX-RateLimit-Reset-After: 0.5\r\n"
        "Retry-After: 2\r\nX-Dup: a\r\nX-Dup: b\r\nContent-Length: 0\r\n\r\n",
        strlen("HTTP/1.1 200 OK\r\nX-RateLimit-Remaining:   4  \r\nX-RateLimit-Reset-After: 0.5\r\n"
               "Retry-After: 2\r\nX-Dup: a\r\nX-Dup: b\r\nContent-Length: 0\r\n\r\n"),
        7, 0, 1000, 0, &r, &consumed);
    CHECK(strcmp(http_resp_header(&r, "x-ratelimit-remaining"), "4") == 0);
    CHECK(strcmp(http_resp_header(&r, "X-RATELIMIT-RESET-AFTER"), "0.5") == 0);
    CHECK(strcmp(http_resp_header(&r, "retry-after"), "2") == 0);
    CHECK(strcmp(http_resp_header(&r, "X-Dup"), "a") == 0);
    CHECK(http_resp_header(&r, "Nope") == NULL);
    CHECK(strcmp(r.reason, "OK") == 0 && r.minor == 1 && r.keep_alive);
    http_resp_free(&r);

    run("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n",
        strlen("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n"), 4, 0, 1000, 0, &r, &consumed);
    CHECK(r.status == 429 && strcmp(r.reason, "Too Many Requests") == 0);
    http_resp_free(&r);

    /* keep-alive rules */
#define KA(text, head, want) do { const char *t_ = text; \
    run(t_, strlen(t_), 1000, 0, 1000, 1, &r, &consumed); CHECK(r.keep_alive == (want)); http_resp_free(&r); } while (0)
    KA("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0, 1);
    KA("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", 0, 0);
    KA("HTTP/1.1 200 OK\r\nConnection: Keep-Alive, Upgrade\r\nContent-Length: 0\r\n\r\n", 0, 1);
    KA("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n", 0, 0);
    KA("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n", 0, 1);
    KA("HTTP/1.1 200 OK\r\n\r\nbody", 0, 0);                        /* close-delimited can't be reused */
#undef KA
}

static void test_errors(void)
{
    static const char *bad[] = {
        "garbage\r\n\r\n",
        "HTTP/2 200 OK\r\n\r\n",
        "HTTP/1.1 20 OK\r\n\r\n",
        "HTTP/1.1 2000 OK\r\n\r\n",
        "HTTP/1.1 200OK\r\n\r\n",
        "HTTP/1.1 200 OK\r\nNo colon here\r\n\r\n",
        "HTTP/1.1 200 OK\r\n: empty name\r\n\r\n",
        "HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n",
        "HTTP/1.1 200 OK\r\nA: b\r\n folded\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!",
        "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n",          /* over max_body (1000) */
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5 x\r\nhello\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFF\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3e8\r\nx",   /* 1000 ok, but then.. see below */
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n7d1\r\n",   /* 2001 bytes > max_body */
        NULL
    };
    HttpResp r;
    size_t consumed;
    int i, rc;
    char *big;

    for (i = 0; bad[i] != NULL; i++) {
        /* 3e8 (1000 bytes) is within max_body; it simply never completes, so only EOF reports the error */
        rc = run(bad[i], strlen(bad[i]), 3, 0, 1000, 1, &r, &consumed);
        CHECK(rc == HTTP_ERROR && r.err[0] != '\0');
        http_resp_free(&r);
    }

    /* truncated: connection closed in the middle of a Content-Length body, and in the headers */
    rc = run("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", strlen("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort"), 8, 0, 1000, 1, &r, &consumed);
    CHECK(rc == HTTP_ERROR);
    http_resp_free(&r);
    rc = run("HTTP/1.1 200 OK\r\nContent-Le", strlen("HTTP/1.1 200 OK\r\nContent-Le"), 8, 0, 1000, 1, &r, &consumed);
    CHECK(rc == HTTP_ERROR);
    http_resp_free(&r);
    rc = run("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", strlen("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel"), 8, 0, 1000, 1, &r, &consumed);
    CHECK(rc == HTTP_ERROR);
    http_resp_free(&r);
    /* ...but a read-until-close body is complete at EOF */
    rc = run("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nabc", strlen("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nabc"), 8, 0, 1000, 1, &r, &consumed);
    CHECK(rc == HTTP_DONE && r.body_len == 3 && !r.keep_alive);
    http_resp_free(&r);

    /* oversized header block, one byte past the limit */
    big = (char *)malloc(HTTP_MAX_HEADER_BYTES + 200);
    memcpy(big, "HTTP/1.1 200 OK\r\nX-Big: ", 24);
    memset(big + 24, 'a', HTTP_MAX_HEADER_BYTES);
    memcpy(big + 24 + HTTP_MAX_HEADER_BYTES, "\r\n\r\n", 5);
    rc = run(big, strlen(big), 4096, 0, 1000, 0, &r, &consumed);
    CHECK(rc == HTTP_ERROR && strstr(r.err, "headers too large") != NULL);
    http_resp_free(&r);
    free(big);

    /* body that grows past max_body when no length is given */
    {
        char text[300];
        memcpy(text, "HTTP/1.0 200 OK\r\n\r\n", 19);
        memset(text + 19, 'x', 200);
        text[219] = '\0';
        rc = run(text, 219, 50, 0, 100, 0, &r, &consumed);
        CHECK(rc == HTTP_ERROR && strstr(r.err, "too large") != NULL);
        http_resp_free(&r);
    }
    /* feeding after an error or completion stays put */
    {
        size_t used;
        http_resp_init(&r, 1000, 0);
        CHECK(http_resp_feed(&r, (const unsigned char *)"junk\r\n\r\n", 8, &used) == HTTP_ERROR);
        CHECK(http_resp_feed(&r, (const unsigned char *)"more", 4, &used) == HTTP_ERROR && used == 0);
        http_resp_free(&r);
    }
}

/* Deterministic mutation fuzz: random edits and random splits must never crash or hang; the sanitizer
 * build is what makes this meaningful. */
static unsigned long lcg_state = 12345UL;

static unsigned long lcg(void)
{
    lcg_state = lcg_state * 1103515245UL + 12345UL;
    return (lcg_state >> 16) & 0x7fffUL;
}

static void test_fuzz(void)
{
    static const char *seeds[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nT: x\r\n\r\n",
        "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok",
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n\x81\x05hello",
        "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\nuntil close",
        NULL
    };
    int s, iter, k;
    char buf[256];
    size_t len, pos, n, used;
    HttpResp r;
    int rc;

    for (s = 0; seeds[s] != NULL; s++) {
        for (iter = 0; iter < 4000; iter++) {
            len = strlen(seeds[s]);
            memcpy(buf, seeds[s], len);
            for (k = 0; k < 1 + (int)(lcg() % 4); k++) {
                switch (lcg() % 3) {
                case 0:
                    buf[lcg() % len] = (char)(lcg() & 0xff);
                    break;
                case 1:
                    if (len > 8) {
                        len--;
                    }
                    break;
                default:
                    buf[lcg() % len] = "\r\n :0-9a-f;"[lcg() % 11];
                    break;
                }
            }
            http_resp_init(&r, 400, (int)(lcg() & 1));
            pos = 0;
            rc = HTTP_NEED_MORE;
            while (pos < len && rc == HTTP_NEED_MORE) {
                n = 1 + lcg() % 12;
                if (n > len - pos) {
                    n = len - pos;
                }
                rc = http_resp_feed(&r, (const unsigned char *)buf + pos, n, &used);
                pos += used;
            }
            if (rc == HTTP_NEED_MORE) {
                rc = http_resp_eof(&r);
            }
            CHECK(rc == HTTP_DONE || rc == HTTP_ERROR);
            http_resp_free(&r);
        }
    }
}

int main(void)
{
    test_urls();
    test_requests();
    test_basic_responses();
    test_headers_and_flags();
    test_errors();
    test_fuzz();
    TEST_DONE("http");
}
