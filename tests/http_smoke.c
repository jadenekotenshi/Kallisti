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
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include "../core/http.h"
#include "../core/tls.h"
#include "../core/rng.h"

static int t_pass, t_fail;
#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

typedef struct {
    int fd;
    tls_session *tls;        /* NULL for plaintext */
    unsigned char *pending;  /* decrypted bytes not yet handed to the caller */
    size_t npending, cap;
    int closed;
} Conn;

static void seed_rng(void)
{
    if (ssh_rng_ready()) {
        return;
    }
    if (ssh_rng_seed_system() == 0) {
        ssh_rng_add("http smoke", 10, 256);
    }
}

static int connect_retrying(int port)
{
    int i;

    for (i = 0; i < 80; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        struct timeval tv;

        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons((unsigned short)port);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            tv.tv_sec = 10;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return fd;
        }
        close(fd);
        usleep(100000);
    }
    return -1;
}

static int send_all(int fd, const unsigned char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);

        if (w <= 0) {
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int tls_flush(Conn *c)
{
    const u8 *out;
    size_t len;

    out = tls_output(c->tls, &len);
    if (len > 0) {
        if (send_all(c->fd, out, len) != 0) {
            return -1;
        }
        tls_output_done(c->tls, len);
    }
    return 0;
}

static void pend(Conn *c, const unsigned char *d, size_t n)
{
    if (c->npending + n > c->cap) {
        c->cap = (c->npending + n) * 2;
        c->pending = (unsigned char *)realloc(c->pending, c->cap);
    }
    memcpy(c->pending + c->npending, d, n);
    c->npending += n;
}

/* Handles every queued TLS event: returns 0, or -1 on a fatal one. */
static int tls_events(Conn *c, int *handshake_done)
{
    tls_event ev;

    while (tls_next_event(c->tls, &ev)) {
        if (ev.type == TLS_EV_CERT) {
            tls_cert_accept(c->tls, 1);
        } else if (ev.type == TLS_EV_HANDSHAKE_DONE) {
            if (handshake_done) {
                *handshake_done = 1;
            }
        } else if (ev.type == TLS_EV_DATA) {
            pend(c, ev.data, ev.len);
        } else if (ev.type == TLS_EV_CLOSED) {
            c->closed = 1;
        } else if (ev.type == TLS_EV_ERROR) {
            fprintf(stderr, "TLS error: %s\n", ev.text);
            return -1;
        }
    }
    return tls_flush(c);
}

static int conn_open(Conn *c, int port, int use_tls)
{
    memset(c, 0, sizeof(*c));
    c->fd = connect_retrying(port);
    if (c->fd < 0) {
        return -1;
    }
    if (!use_tls) {
        return 0;
    }
    c->tls = tls_new("localhost");
    if (tls_start(c->tls) != 0 || tls_flush(c) != 0) {
        return -1;
    }
    {
        int done = 0;
        unsigned char buf[8192];

        while (!done) {
            ssize_t n = recv(c->fd, buf, sizeof(buf), 0);

            if (n <= 0) {
                return -1;
            }
            if (tls_input(c->tls, buf, (size_t)n) != 0) {
                tls_events(c, NULL);
                return -1;
            }
            if (tls_events(c, &done) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int conn_send(Conn *c, const unsigned char *d, size_t n)
{
    if (c->tls == NULL) {
        return send_all(c->fd, d, n);
    }
    if (tls_write(c->tls, d, n) != 0) {
        return -1;
    }
    return tls_flush(c);
}

/* Hands over up to n plaintext bytes: >0 bytes, 0 at a clean close, -1 on error. */
static long conn_recv(Conn *c, unsigned char *buf, size_t n)
{
    unsigned char raw[16384];
    ssize_t r;

    for (;;) {
        if (c->npending > 0) {
            size_t k = c->npending < n ? c->npending : n;

            memcpy(buf, c->pending, k);
            memmove(c->pending, c->pending + k, c->npending - k);
            c->npending -= k;
            return (long)k;
        }
        if (c->tls != NULL && c->closed) {
            return 0;
        }
        r = recv(c->fd, raw, sizeof(raw), 0);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;
        }
        if (c->tls == NULL) {
            pend(c, raw, (size_t)r);
        } else {
            if (tls_input(c->tls, raw, (size_t)r) != 0) {
                tls_events(c, NULL);
                return -1;
            }
            if (tls_events(c, NULL) != 0) {
                return -1;
            }
        }
    }
}

static void conn_close(Conn *c)
{
    if (c->tls != NULL) {
        tls_free(c->tls);
    }
    close(c->fd);
    free(c->pending);
}

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
        return HTTP_ERROR;
    }
    if (hname != NULL) {
        http_req_header(&q, hname, hvalue);
    }
    if (http_req_end(&q, body, body ? strlen(body) : 0) != 0 ||
        conn_send(c, (const unsigned char *)q.buf, q.len) != 0) {
        http_req_free(&q);
        return HTTP_ERROR;
    }
    http_req_free(&q);

    http_resp_init(r, 4 * 1024 * 1024, strcmp(method, "HEAD") == 0);
    while (rc == HTTP_NEED_MORE) {
        n = conn_recv(c, buf, sizeof(buf));
        if (n < 0) {
            return HTTP_ERROR;
        }
        if (n == 0) {
            return http_resp_eof(r);
        }
        rc = http_resp_feed(r, buf, (size_t)n, &used);
        if (rc == HTTP_DONE && used != (size_t)n) {
            return HTTP_ERROR;           /* the server never pipelines: nothing may be left over */
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
