/*
 * discord_smoke.c -- the whole client stack against the real Discord, over the network (no token needed):
 * a TLS 1.2 handshake whose certificate chain is validated against the trust store Kallisti would use
 * (/usr/local/ssl if it has certificates, else the compiled-in roots), a REST call (GET /api/v10/gateway,
 * which is public) through rest.c, and a WebSocket connection to the gateway that waits for Discord's HELLO.
 * The gateway is closed before it is asked to identify, so no login happens.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../core/roots.h"
#include "../core/rest.h"
#include "../core/ws.h"
#include "../core/json.h"
#include "smoke_conn.h"

static int t_pass, t_fail;
#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void utc_now(der_time *t)
{
    time_t now = time(NULL);
    struct tm *g = gmtime(&now);

    t->year = g->tm_year + 1900;
    t->month = g->tm_mon + 1;
    t->day = g->tm_mday;
    t->hour = g->tm_hour;
    t->min = g->tm_min;
    t->sec = g->tm_sec;
}

static void test_rest(TrustStore *ts)
{
    Conn c;
    RestClient rc;
    HttpReq q;
    HttpResp r;
    RestResult res;
    unsigned char buf[16384];
    long id, n;
    int st = HTTP_NEED_MORE;
    size_t used;

    smoke_trust = ts;
    CHECK(conn_open_host(&c, "discord.com", 443) == 0);
    if (c.tls == NULL || !tls_is_established(c.tls)) {
        fprintf(stderr, "  (TLS failed: %s)\n", smoke_error);
        return;
    }
    printf("  discord.com: %s\n", tls_cipher_name(c.tls));
    rest_init(&rc, "unused-token", 1);
    CHECK(rest_get_gateway_bot(&rc) > 0);
    /* /gateway/bot needs a real token, so ask the public /gateway instead by rewriting the job's path */
    strcpy(rc.jobs[0].path, "/gateway");
    strcpy(rc.jobs[0].route, "GET /gateway");
    CHECK(rest_next_request(&rc, 0, &q, &id) == 1);
    CHECK(conn_send(&c, (const unsigned char *)q.buf, q.len) == 0);
    http_req_free(&q);
    http_resp_init(&r, REST_MAX_RESPONSE, 0);
    while (st == HTTP_NEED_MORE) {
        n = conn_recv(&c, buf, sizeof(buf));
        if (n <= 0) {
            st = http_resp_eof(&r);
            break;
        }
        st = http_resp_feed(&r, buf, (size_t)n, &used);
    }
    CHECK(st == HTTP_DONE);
    if (st == HTTP_DONE) {
        rest_response(&rc, 0, &r);
        CHECK(rest_next_result(&rc, &res));
        CHECK(res.status == 200 && res.ok);
        CHECK(res.body != NULL && strncmp(json_get_str(res.body, "url", ""), "wss://gateway.discord.gg", 24) == 0);
        printf("  GET /api/v10/gateway -> %d %s\n", res.status, json_get_str(res.body, "url", "?"));
    }
    http_resp_free(&r);
    rest_free(&rc);
    conn_close(&c);
}

static void test_gateway(TrustStore *ts)
{
    Conn c;
    WsClient ws;
    WsEvent ev;
    HttpReq q;
    HttpResp r;
    unsigned char buf[16384];
    long n;
    size_t used;
    int rc = HTTP_NEED_MORE, got = 0, i;

    smoke_trust = ts;
    CHECK(conn_open_host(&c, "gateway.discord.gg", 443) == 0);
    if (c.tls == NULL || !tls_is_established(c.tls)) {
        fprintf(stderr, "  (TLS failed: %s)\n", smoke_error);
        return;
    }
    CHECK(ws_init(&ws, "wss://gateway.discord.gg/?v=10&encoding=json") == 0);
    ws.rng = ssh_rng_bytes;
    http_req_init(&q);
    CHECK(ws_handshake_begin(&ws, &q) == 0 && http_req_end(&q, NULL, 0) == 0);
    CHECK(conn_send(&c, (const unsigned char *)q.buf, q.len) == 0);
    http_req_free(&q);
    http_resp_init(&r, 65536, 0);
    while (rc == HTTP_NEED_MORE) {
        n = conn_recv(&c, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        rc = http_resp_feed(&r, buf, (size_t)n, &used);
        if (rc == HTTP_DONE) {
            CHECK(ws_handshake_check(&ws, &r) == 0);
            if (used < (size_t)n) {
                ws_input(&ws, buf + used, (size_t)n - used);
            }
        }
    }
    CHECK(rc == HTTP_DONE && ws.state == WS_OPEN);
    http_resp_free(&r);
    for (i = 0; i < 20 && !got && ws.state == WS_OPEN; i++) {
        if (ws_next_event(&ws, &ev)) {
            if (ev.type == WS_EV_TEXT) {
                JDoc d;
                JNode *root, *hb;
                long interval = 0;

                memset(&d, 0, sizeof(d));
                root = json_parse(&d, (const char *)ev.data, ev.len);
                hb = json_get(json_get(root, "d"), "heartbeat_interval");
                CHECK(root != NULL && json_get(root, "op") != NULL && json_get(root, "op")->ival == 10);
                CHECK(json_int(hb, &interval) && interval > 1000);
                printf("  gateway HELLO, heartbeat_interval %ld ms\n", interval);
                json_free(&d);
                got = 1;
            }
            continue;
        }
        n = conn_recv(&c, buf, sizeof(buf));
        if (n <= 0 || ws_input(&ws, buf, (size_t)n) != 0) {
            break;
        }
    }
    CHECK(got);
    ws_send_close(&ws, 1000, NULL);
    {
        const unsigned char *out;
        size_t len;

        out = ws_output(&ws, &len);
        if (len > 0) {
            conn_send(&c, out, len);
        }
    }
    ws_free(&ws);
    conn_close(&c);
}

int main(void)
{
    TrustStore ts;
    int count = 0, src;
    der_time now;

    seed_rng();
    trust_init(&ts);
    src = roots_load_default(&ts, ROOTS_SYSTEM_DIR, &count);
    printf("trust store: %d roots from %s\n", count, src == ROOTS_FROM_DIR ? ROOTS_SYSTEM_DIR : "the compiled-in set");
    CHECK(count > 0);
    utc_now(&now);
    smoke_check_dates = 1;
    smoke_now = now;
    test_rest(&ts);
    test_gateway(&ts);
    trust_free(&ts);
    printf("discord_smoke: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
