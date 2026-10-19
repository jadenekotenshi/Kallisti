/*
 * http_smoke.c -- drives core/http.c against a REAL HTTP/1.1 server (tests/http_smoke_server.py, a
 * scripted Python server) over a real TCP socket: once in plaintext, and once through the vendored
 * TLS 1.2 engine (the server wraps itself in TLS with a throwaway self-signed certificate).  It
 * covers the request builder (headers and bodies the server echoes back), and every response shape
 * the parser has to cope with on a keep-alive connection: Content-Length, chunked with a trailer,
 * large bodies spanning many reads and TLS records, HEAD, 100 Continue, 204, a 429 with Retry-After,
 * and a read-until-close reply on a connection of its own.
 *
 * usage: http_smoke SCRIPT PORT_PLAIN PORT_TLS CERT KEY
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../core/http.h"
#include "smoke_conn.h"

static int t_pass, t_fail;
#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* One request, one response.  Returns the parser's final result; r is the filled response. */
static int exchange(Conn *c, const char *method, const char *path, int port, int use_tls,
                    const char *hname, const char *hvalue, const char *body, HttpResp *r)
{
    HttpReq q;
    unsigned char buf[16384];
    int rc = HTTP_NEED_MORE;
    size_t used;
    long n;

    http_req_init(&q);
    if (http_req_begin(&q, method, path, "localhost", port, use_tls) != 0) {
        return HTTP_FAILED;
    }
    if (hname != NULL) {
        http_req_header(&q, hname, hvalue);
    }
    if (http_req_end(&q, body, body ? strlen(body) : 0) != 0 ||
        conn_send(c, (const unsigned char *)q.buf, q.len) != 0) {
        http_req_free(&q);
        return HTTP_FAILED;
    }
    http_req_free(&q);

    http_resp_init(r, 4 * 1024 * 1024, strcmp(method, "HEAD") == 0);
    while (rc == HTTP_NEED_MORE) {
        n = conn_recv(c, buf, sizeof(buf));
        if (n < 0) {
            return HTTP_FAILED;
        }
        if (n == 0) {
            return http_resp_eof(r);
        }
        rc = http_resp_feed(r, buf, (size_t)n, &used);
        if (rc == HTTP_DONE && used != (size_t)n) {
            return HTTP_FAILED;           /* the server never pipelines: nothing may be left over */
        }
    }
    return rc;
}

static int body_is(const HttpResp *r, const char *s)
{
    return r->body_len == strlen(s) && memcmp(r->body, s, r->body_len) == 0;
}

static void run_suite(int port, int use_tls)
{
    Conn c;
    HttpResp r;
    size_t i;
    int ok;

    if (conn_open(&c, port, use_tls) != 0) {
        fprintf(stderr, "could not open the %s connection\n", use_tls ? "TLS" : "plain");
        CHECK(0);
        return;
    }

    CHECK(exchange(&c, "GET", "/len", port, use_tls, NULL, NULL, NULL, &r) == HTTP_DONE);
    CHECK(r.status == 200 && body_is(&r, "hello world") && r.keep_alive);
    CHECK(strcmp(http_resp_header(&r, "x-ratelimit-remaining"), "4") == 0);
    http_resp_free(&r);

    CHECK(exchange(&c, "GET", "/chunked", port, use_tls, NULL, NULL, NULL, &r) == HTTP_DONE);
    CHECK(r.status == 200 && body_is(&r, "alpha beta gamma") && r.keep_alive);
    http_resp_free(&r);

    /* a 300 KB body, then a 200 KB chunked one: many reads, many TLS records */
    CHECK(exchange(&c, "GET", "/big", port, use_tls, NULL, NULL, NULL, &r) == HTTP_DONE);
    ok = r.body_len == 300 * 1024;
    for (i = 0; ok && i < r.body_len; i++) {
        ok = r.body[i] == (unsigned char)((i * 7 + 3) & 0xff);
    }
    CHECK(ok);
    http_resp_free(&r);
    CHECK(exchange(&c, "GET", "/bigchunked", port, use_tls, NULL, NULL, NULL, &r) == HTTP_DONE);
    ok = r.body_len == 200 * 1024;
    for (i = 0; ok && i < r.body_len; i++) {
        ok = r.body[i] == (unsigned char)((i * 7 + 3) & 0xff);
    }
    CHECK(ok);
    http_resp_free(&r);

    /* a POST whose body and headers the server reads back */
    CHECK(exchange(&c, "POST", "/echo", port, use_tls, "Content-Type", "application/json",
                   "{\"content\":\"hi \\u2603\"}", &r) == HTTP_DONE);
    CHECK(r.status == 200 && strstr((const char *)r.body, "\"method\": \"POST\"") != NULL &&
          strstr((const char *)r.body, "\"len\": 23") != NULL &&
          strstr((const char *)r.body, "application/json") != NULL);
    http_resp_free(&r);

    /* what the server saw: Host (with the port, since it is not the scheme default), Accept-Encoding,
     * and our own header */
    CHECK(exchange(&c, "GET", "/headers", port, use_tls, "Authorization", "Bot secret-token", NULL, &r) == HTTP_DONE);
    CHECK(strstr((const char *)r.body, "accept-encoding=identity\n") != NULL &&
          strstr((const char *)r.body, "authorization=Bot secret-token\n") != NULL);
    {
        char host[40];
        sprintf(host, "host=localhost:%d\n", port);
        CHECK(strstr((const char *)r.body, host) != NULL);
    }
    http_resp_free(&r);

    /* HEAD carries the length but no body; the connection stays usable */
    CHECK(exchange(&c, "HEAD", "/len", port, use_tls, NULL, NULL, NULL, &r) == HTTP_DONE);
    CHECK(r.status == 200 && r.body_len == 0 && http_resp_header(&r, "Content-Length") != NULL);
    http_resp_free(&r);
    CHECK(exchange(&c, "GET", "/100", port, use_tls, NULL, NULL, NULL, &r) == HTTP_DONE);
    CHECK(r.status == 201 && body_is(&r, "ok"));
    http_resp_free(&r);
    CHECK(exchange(&c, "GET", "/204", port, use_tls, NULL, NULL, NULL, &r) == HTTP_DONE);
    CHECK(r.status == 204 && r.body_len == 0);
    http_resp_free(&r);
    CHECK(exchange(&c, "GET", "/429", port, use_tls, NULL, NULL, NULL, &r) == HTTP_DONE);
    CHECK(r.status == 429 && strcmp(http_resp_header(&r, "Retry-After"), "1") == 0 &&
          strstr((const char *)r.body, "\"retry_after\":0.5") != NULL);
    http_resp_free(&r);
    CHECK(exchange(&c, "GET", "/missing", port, use_tls, NULL, NULL, NULL, &r) == HTTP_DONE);
    CHECK(r.status == 404 && r.body_len == 0);
    http_resp_free(&r);
    /* and after all that, the same connection still answers */
    CHECK(exchange(&c, "GET", "/len", port, use_tls, NULL, NULL, NULL, &r) == HTTP_DONE);
    CHECK(body_is(&r, "hello world"));
    http_resp_free(&r);
    conn_close(&c);

    /* a reply that ends only when the server closes the connection */
    if (conn_open(&c, port, use_tls) == 0) {
        CHECK(exchange(&c, "GET", "/close", port, use_tls, NULL, NULL, NULL, &r) == HTTP_DONE);
        CHECK(r.status == 200 && r.minor == 0 && !r.keep_alive &&
              body_is(&r, "read until the connection closes"));
        http_resp_free(&r);
        conn_close(&c);
    } else {
        CHECK(0);
    }
}

int main(int argc, char **argv)
{
    pid_t plain, secure;
    int status;

    if (argc != 6) {
        fprintf(stderr, "usage: http_smoke SCRIPT PORT_PLAIN PORT_TLS CERT KEY\n");
        return 2;
    }
    seed_rng();
    signal(SIGPIPE, SIG_IGN);

    plain = fork();
    if (plain == 0) {
        execlp("python3", "python3", argv[1], argv[2], (char *)NULL);
        _exit(127);
    }
    secure = fork();
    if (secure == 0) {
        execlp("python3", "python3", argv[1], argv[3], argv[4], argv[5], (char *)NULL);
        _exit(127);
    }

    run_suite(atoi(argv[2]), 0);
    run_suite(atoi(argv[3]), 1);

    kill(plain, SIGTERM);
    kill(secure, SIGTERM);
    waitpid(plain, &status, 0);
    waitpid(secure, &status, 0);
    printf("http_smoke: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
