/*
 * ws_smoke.c -- drives core/ws.c against an independent WebSocket implementation (Python's
 * `websockets` library, tests/ws_smoke_server.py) over a real socket, in plaintext and through the
 * vendored TLS 1.2 engine: the HTTP upgrade handshake (built and parsed with core/http.c), echoes of
 * text and binary messages up to 70 KB, a ping answered by the library, a fragmented message from the
 * server, the library's own ping answered automatically by us, a 200 KB binary and a ~1 MB text
 * message, the close handshake from either side, and a handshake the server refuses (404).
 *
 * usage: ws_smoke SERVER_SCRIPT PYTHON PORT_PLAIN PORT_TLS CERT KEY
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../core/ws.h"
#include "smoke_conn.h"

static int t_pass, t_fail;
#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* Sends whatever the WebSocket has queued. */
static int pump_out(Conn *c, WsClient *ws)
{
    const unsigned char *out;
    size_t n;

    out = ws_output(ws, &n);
    if (n > 0) {
        if (conn_send(c, out, n) != 0) {
            return -1;
        }
        ws_output_done(ws, n);
    }
    return 0;
}

/* Waits for the next event, reading from the connection as needed. Returns 1, or 0 on a closed/failed connection. */
static int next_event(Conn *c, WsClient *ws, WsEvent *ev)
{
    unsigned char buf[16384];
    long n;

    for (;;) {
        if (ws_next_event(ws, ev)) {
            pump_out(c, ws);
            return 1;
        }
        n = conn_recv(c, buf, sizeof(buf));
        if (n <= 0) {
            return 0;
        }
        if (ws_input(ws, buf, (size_t)n) != 0) {
            return 0;
        }
    }
}

/* The upgrade: our request, the server's reply parsed by http.c, ws_handshake_check, leftover bytes to the ws. */
static int handshake(Conn *c, WsClient *ws, int port, int tls, const char *path, int *status_out)
{
    char url[128];
    HttpReq q;
    HttpResp r;
    unsigned char buf[16384];
    size_t used;
    long n;
    int rc = HTTP_NEED_MORE, ok;

    sprintf(url, "%s://localhost:%d%s", tls ? "wss" : "ws", port, path);
    if (ws_init(ws, url) != 0) {
        return -1;
    }
    http_req_init(&q);
    if (ws_handshake_begin(ws, &q) != 0 || http_req_header(&q, "User-Agent", "Kallisti-smoke") != 0 ||
        http_req_end(&q, NULL, 0) != 0 || conn_send(c, (const unsigned char *)q.buf, q.len) != 0) {
        http_req_free(&q);
        return -1;
    }
    http_req_free(&q);
    http_resp_init(&r, 65536, 0);
    while (rc == HTTP_NEED_MORE) {
        n = conn_recv(c, buf, sizeof(buf));
        if (n <= 0) {
            http_resp_free(&r);
            return -1;
        }
        rc = http_resp_feed(&r, buf, (size_t)n, &used);
        if (rc == HTTP_DONE && used < (size_t)n) {
            /* WebSocket bytes that arrived with the 101 */
            if (ws_handshake_check(ws, &r) == 0) {
                ws_input(ws, buf + used, (size_t)n - used);
            }
        }
    }
    if (status_out != NULL) {
        *status_out = r.status;
    }
    if (rc != HTTP_DONE) {
        http_resp_free(&r);
        return -1;
    }
    ok = (ws->state == WS_OPEN) ? 0 : ws_handshake_check(ws, &r);
    http_resp_free(&r);
    return ok;
}

static int text_is(const WsEvent *ev, const char *s)
{
    return ev->type == WS_EV_TEXT && ev->len == strlen(s) && memcmp(ev->data, s, ev->len) == 0;
}

static void run_suite(int port, int tls)
{
    Conn c;
    WsClient ws;
    WsEvent ev;
    static unsigned char blob[70000];
    size_t i;
    int status = 0, ok;
    const char *uni = "\xC3\xBCn\xC3\xAF" "code \xE2\x98\x83 \xF0\x9F\x98\x80";

    /* ---- /echo ---- */
    CHECK(conn_open(&c, port, tls) == 0);
    CHECK(handshake(&c, &ws, port, tls, "/echo", &status) == 0 && status == 101 && ws.state == WS_OPEN);
    CHECK(ws_send_text(&ws, "hello", 5) == 0 && pump_out(&c, &ws) == 0);
    CHECK(next_event(&c, &ws, &ev) && text_is(&ev, "hello"));
    CHECK(ws_send_text(&ws, uni, strlen(uni)) == 0 && pump_out(&c, &ws) == 0);
    CHECK(next_event(&c, &ws, &ev) && text_is(&ev, uni));
    for (i = 0; i < sizeof(blob); i++) {
        blob[i] = (unsigned char)((i * 13 + (i >> 7)) & 0xff);
    }
    CHECK(ws_send_binary(&ws, blob, 1000) == 0 && pump_out(&c, &ws) == 0);
    CHECK(next_event(&c, &ws, &ev) && ev.type == WS_EV_BINARY && ev.len == 1000 && memcmp(ev.data, blob, 1000) == 0);
    CHECK(ws_send_binary(&ws, blob, 70000) == 0 && pump_out(&c, &ws) == 0);
    CHECK(next_event(&c, &ws, &ev) && ev.type == WS_EV_BINARY && ev.len == 70000 && memcmp(ev.data, blob, 70000) == 0);
    CHECK(ws_send_binary(&ws, blob, 0) == 0 && pump_out(&c, &ws) == 0);
    CHECK(next_event(&c, &ws, &ev) && ev.type == WS_EV_BINARY && ev.len == 0);
    /* our ping: the library answers with a pong carrying the same payload */
    CHECK(ws_send_ping(&ws, (const unsigned char *)"ping-payload", 12) == 0 && pump_out(&c, &ws) == 0);
    CHECK(next_event(&c, &ws, &ev) && ev.type == WS_EV_PONG && ev.len == 12 && memcmp(ev.data, "ping-payload", 12) == 0);
    /* we close: the library echoes our close */
    CHECK(ws_send_close(&ws, 1000, "done") == 0 && pump_out(&c, &ws) == 0);
    CHECK(next_event(&c, &ws, &ev) && ev.type == WS_EV_CLOSE && ev.code == 1000 && ws_is_closed(&ws));
    ws_free(&ws);
    conn_close(&c);

    /* ---- /greet: fragments, and the library's ping which we must answer by ourselves ---- */
    CHECK(conn_open(&c, port, tls) == 0);
    CHECK(handshake(&c, &ws, port, tls, "/greet", &status) == 0);
    CHECK(next_event(&c, &ws, &ev) && text_is(&ev, "hello"));
    CHECK(next_event(&c, &ws, &ev) && ev.type == WS_EV_BINARY && ev.len == 17 && memcmp(ev.data, "part1 part2 part3", 17) == 0);
    CHECK(next_event(&c, &ws, &ev) && ev.type == WS_EV_PING && ev.len == 13 && memcmp(ev.data, "are-you-there", 13) == 0);
    CHECK(next_event(&c, &ws, &ev) && text_is(&ev, "pong-ok"));      /* only sent once our pong arrived */
    CHECK(ws_send_close(&ws, 1000, NULL) == 0 && pump_out(&c, &ws) == 0);
    CHECK(next_event(&c, &ws, &ev) && ev.type == WS_EV_CLOSE);
    ws_free(&ws);
    conn_close(&c);

    /* ---- /big: large messages, many reads and TLS records ---- */
    CHECK(conn_open(&c, port, tls) == 0);
    CHECK(handshake(&c, &ws, port, tls, "/big", &status) == 0);
    CHECK(next_event(&c, &ws, &ev) && ev.type == WS_EV_BINARY && ev.len == 200 * 1024);
    ok = ev.type == WS_EV_BINARY && ev.len == 200 * 1024;
    for (i = 0; ok && i < ev.len; i++) {
        ok = ev.data[i] == (unsigned char)((i * 7 + 3) & 0xff);
    }
    CHECK(ok);
    CHECK(next_event(&c, &ws, &ev) && ev.type == WS_EV_TEXT && ev.len > 900000 && ws_utf8_valid(ev.data, ev.len));
    CHECK(strncmp((const char *)ev.data, "snowman \xE2\x98\x83 and grin \xF0\x9F\x98\x80 snowman", 28) == 0);
    ws_send_close(&ws, 1000, NULL);
    pump_out(&c, &ws);
    ws_free(&ws);
    conn_close(&c);

    /* ---- /close: a message, then the server closes with a code and reason ---- */
    CHECK(conn_open(&c, port, tls) == 0);
    CHECK(handshake(&c, &ws, port, tls, "/close", &status) == 0);
    CHECK(next_event(&c, &ws, &ev) && text_is(&ev, "last"));
    CHECK(next_event(&c, &ws, &ev) && ev.type == WS_EV_CLOSE && ev.code == 4001 && strcmp(ev.text, "server says bye") == 0);
    CHECK(ws_is_closed(&ws));
    CHECK(pump_out(&c, &ws) == 0);                                   /* the echoed close goes out */
    ws_free(&ws);
    conn_close(&c);

    /* ---- a route the server refuses: a 404 instead of the upgrade ---- */
    CHECK(conn_open(&c, port, tls) == 0);
    CHECK(handshake(&c, &ws, port, tls, "/nope", &status) == -1 && status == 404 && ws.state == WS_CONNECTING);
    CHECK(strstr(ws.err, "404") != NULL);
    ws_free(&ws);
    conn_close(&c);
}

int main(int argc, char **argv)
{
    pid_t plain, secure;
    int status;

    if (argc != 7) {
        fprintf(stderr, "usage: ws_smoke SERVER_SCRIPT PYTHON PORT_PLAIN PORT_TLS CERT KEY\n");
        return 2;
    }
    seed_rng();
    signal(SIGPIPE, SIG_IGN);

    plain = fork();
    if (plain == 0) {
        execlp(argv[2], argv[2], argv[1], argv[3], (char *)NULL);
        _exit(127);
    }
    secure = fork();
    if (secure == 0) {
        execlp(argv[2], argv[2], argv[1], argv[4], argv[5], argv[6], (char *)NULL);
        _exit(127);
    }

    run_suite(atoi(argv[3]), 0);
    run_suite(atoi(argv[4]), 1);

    kill(plain, SIGTERM);
    kill(secure, SIGTERM);
    waitpid(plain, &status, 0);
    waitpid(secure, &status, 0);
    printf("ws_smoke: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
