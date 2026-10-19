/*
 * gw_smoke.c -- drives core/gateway.c end to end against a scripted fake Discord gateway
 * (tests/gw_smoke_server.py, on Python's `websockets`), in plaintext and through the vendored TLS engine:
 * IDENTIFY, READY, dispatches, heartbeats, RECONNECT then RESUME, INVALID_SESSION then a fresh IDENTIFY,
 * a connection that stops ACKing, and a fatal close code.  The driver below is the shape the app's run loop
 * takes: execute the CONNECT/CLOSE commands, feed bytes in, flush ws output, sleep until gw_next_deadline().
 *
 * usage: gw_smoke SERVER_SCRIPT PYTHON PORT_PLAIN PORT_TLS CERT KEY REPORT_PREFIX
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../core/gateway.h"
#include "../core/http.h"
#include "smoke_conn.h"

static int t_pass, t_fail;
#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static GwMs now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (GwMs)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
}

static int zero_rng(void *out, size_t len)
{
    memset(out, 0, len);
    return 0;
}

#define MAX_LOG 32
typedef struct {
    char line[MAX_LOG][96];
    int n;
} Log;

static void log_add(Log *l, const char *s)
{
    if (l->n < MAX_LOG) {
        strncpy(l->line[l->n], s, 95);
        l->line[l->n][95] = '\0';
        l->n++;
    }
}

static void record(Log *l, const GwEvent *ev)
{
    char b[96];

    switch (ev->type) {
    case GW_EV_READY:
        sprintf(b, "READY %s %s", ev->user_id, ev->name);
        break;
    case GW_EV_RESUMED:
        strcpy(b, "RESUMED");
        break;
    case GW_EV_DISPATCH:
        if (strcmp(ev->name, "MESSAGE_CREATE") == 0) {
            sprintf(b, "DISPATCH MESSAGE_CREATE s=%ld %s", ev->seq, json_get_str(ev->d, "content", "?"));
        } else {
            sprintf(b, "DISPATCH %s s=%ld", ev->name, ev->seq);
        }
        break;
    case GW_EV_DISCONNECTED:
        sprintf(b, "DISCONNECTED %d %s", ev->code, ev->will_reconnect ? "reconnect" : "final");
        break;
    case GW_EV_FATAL:
        sprintf(b, "FATAL %d", ev->code);
        break;
    default:
        strcpy(b, "?");
    }
    log_add(l, b);
}

/* The "url" the test substitutes for Discord's: the gateway asks for the default one first. */
static void split_url(const char *url, char *scratch, size_t cap, const char *test_base, const char **out)
{
    if (strncmp(url, GW_DEFAULT_URL, 20) == 0 && strstr(url, "gateway.discord.gg") != NULL) {
        sprintf(scratch, "%s/?v=10&encoding=json", test_base);
        *out = scratch;
    } else {
        *out = url;
    }
    (void)cap;
}

typedef struct {
    Conn c;
    WsClient ws;
    int open;           /* a live connection, handshake done */
    int tls;
    int port;
} Link;

static void link_drop(Link *lk)
{
    if (lk->open) {
        ws_free(&lk->ws);
        conn_close(&lk->c);
        lk->open = 0;
    }
}

/* Connects and performs the HTTP upgrade; 0 on success. */
static int link_open(Link *lk, const char *url, int port, int tls, WsClient **wsout)
{
    HttpReq q;
    HttpResp r;
    unsigned char buf[16384];
    size_t used;
    long n;
    int rc = HTTP_NEED_MORE, ok = -1;

    if (conn_open(&lk->c, port, tls) != 0) {
        return -1;
    }
    if (ws_init(&lk->ws, url) != 0) {
        conn_close(&lk->c);
        return -1;
    }
    lk->ws.rng = ssh_rng_bytes;
    http_req_init(&q);
    if (ws_handshake_begin(&lk->ws, &q) != 0 || http_req_end(&q, NULL, 0) != 0 ||
        conn_send(&lk->c, (const unsigned char *)q.buf, q.len) != 0) {
        http_req_free(&q);
        ws_free(&lk->ws);
        conn_close(&lk->c);
        return -1;
    }
    http_req_free(&q);
    http_resp_init(&r, 65536, 0);
    while (rc == HTTP_NEED_MORE) {
        n = conn_recv(&lk->c, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        rc = http_resp_feed(&r, buf, (size_t)n, &used);
        if (rc == HTTP_DONE) {
            ok = ws_handshake_check(&lk->ws, &r);
            if (ok == 0 && used < (size_t)n) {
                ws_input(&lk->ws, buf + used, (size_t)n - used);
            }
        }
    }
    http_resp_free(&r);
    if (ok != 0) {
        ws_free(&lk->ws);
        conn_close(&lk->c);
        return -1;
    }
    lk->open = 1;
    *wsout = &lk->ws;
    return 0;
}

static int flush_ws(Link *lk)
{
    const unsigned char *out;
    size_t n;

    if (!lk->open) {
        return 0;
    }
    out = ws_output(&lk->ws, &n);
    if (n > 0) {
        if (conn_send(&lk->c, out, n) != 0) {
            return -1;
        }
        ws_output_done(&lk->ws, n);
    }
    return 0;
}

/* Runs the gateway until it is FATAL, or the deadline passes.  Returns 0 when FATAL was reached. */
static int run_session(int port, int tls, Log *log)
{
    Gateway gw;
    Link lk;
    GwCommand cmd;
    GwEvent ev;
    WsEvent wev;
    GwMs start, t, pending_connect_at = 0;
    char base[64], scratch[400], pend_url[400];
    int have_connect = 0, result = -1, closed_code;
    unsigned char buf[16384];
    long wait, got;

    memset(&lk, 0, sizeof(lk));
    sprintf(base, "%s://localhost:%d", tls ? "wss" : "ws", port);
    gw_init(&gw, "Bot test.token.value", GW_INTENT_GUILDS | GW_INTENT_GUILD_MESSAGES | GW_INTENT_MESSAGE_CONTENT);
    gw.rng = zero_rng;
    start = now_ms();
    gw_start(&gw, start);

    while (now_ms() - start < 40000U) {
        t = now_ms();
        /* commands */
        while (gw_next_command(&gw, &cmd)) {
            if (cmd.type == GW_CMD_CONNECT) {
                const char *u;

                split_url(cmd.url, scratch, sizeof(scratch), base, &u);
                strcpy(pend_url, u);
                pending_connect_at = t + cmd.delay_ms;
                have_connect = 1;
            } else if (cmd.type == GW_CMD_CLOSE) {
                flush_ws(&lk);
                link_drop(&lk);
            }
        }
        while (gw_next_event(&gw, &ev)) {
            record(log, &ev);
            if (ev.type == GW_EV_FATAL) {
                result = 0;
            }
        }
        if (result == 0) {
            break;
        }
        if (have_connect && (int)(t - pending_connect_at) >= 0) {
            WsClient *ws = NULL;

            have_connect = 0;
            if (link_open(&lk, pend_url, port, tls, &ws) == 0) {
                gw_connection_opened(&gw, ws, t);
            } else {
                gw_connection_failed(&gw, t);
            }
            continue;
        }
        gw_tick(&gw, t);
        flush_ws(&lk);

        /* sleep until the next thing is due or bytes arrive */
        wait = gw_next_deadline(&gw, now_ms());
        if (have_connect) {
            long w2 = (long)(int)(pending_connect_at - now_ms());

            if (wait < 0 || w2 < wait) {
                wait = w2;
            }
        }
        if (wait < 0 || wait > 200) {
            wait = 200;
        }
        if (wait < 0) {
            wait = 0;
        }
        if (lk.open && conn_wait(&lk.c, (int)wait)) {
            got = conn_recv(&lk.c, buf, sizeof(buf));
            if (got <= 0) {
                link_drop(&lk);
                gw_connection_lost(&gw, 0, now_ms());
                continue;
            }
            if (ws_input(&lk.ws, buf, (size_t)got) != 0) {
                link_drop(&lk);
                gw_connection_lost(&gw, 0, now_ms());
                continue;
            }
            while (lk.open && ws_next_event(&lk.ws, &wev)) {
                gw_handle_ws_event(&gw, &wev, now_ms());
                /* a DISPATCH's "d" lives until the next frame is handled: drain before feeding more */
                while (gw_next_event(&gw, &ev)) {
                    record(log, &ev);
                    if (ev.type == GW_EV_FATAL) {
                        result = 0;
                    }
                }
                closed_code = (wev.type == WS_EV_CLOSE);
                if (closed_code) {
                    flush_ws(&lk);
                    link_drop(&lk);
                    break;
                }
            }
            flush_ws(&lk);
        } else if (!lk.open && wait > 0) {
            usleep((useconds_t)wait * 1000);
        }
    }
    link_drop(&lk);
    gw_free(&gw);
    return result;
}

static const char *EXPECT[] = {
    /* connection 1; the server then sends RECONNECT, so the client closes with 4000 and resumes */
    "READY 42 Kallisti-bot",
    "DISPATCH READY s=1",
    "DISPATCH GUILD_CREATE s=2",
    "DISPATCH MESSAGE_CREATE s=3 hello \xE2\x98\x83",
    "DISCONNECTED 4000 reconnect",
    /* connection 2: resumed, then told the session is dead and identifies again */
    "DISPATCH MESSAGE_CREATE s=4 replayed",
    "RESUMED",
    "READY 42 Kallisti-bot",
    "DISPATCH READY s=1",
    "DISCONNECTED 4000 reconnect",
    /* connection 3: resumed, never ACKed, so the client drops it */
    "RESUMED",
    "DISCONNECTED 4000 reconnect",
    /* connection 4: resumed, then close 4004 (bad token) ends it */
    "RESUMED",
    "FATAL 4004",
    NULL
};

static void run_suite(int port, int tls, const char *report)
{
    Log log;
    FILE *f;
    char line[256];
    int i, saw_done = 0, saw_fail = 0, nexp = 0;

    memset(&log, 0, sizeof(log));
    CHECK(run_session(port, tls, &log) == 0);
    while (EXPECT[nexp] != NULL) {
        nexp++;
    }
    for (i = 0; i < log.n || i < nexp; i++) {
        const char *got = i < log.n ? log.line[i] : "(nothing)";
        const char *want = i < nexp ? EXPECT[i] : "(nothing)";

        if (getenv("GW_DUMP") != NULL) {
            fprintf(stderr, "  [%d] %s\n", i, got);
        }
        if (strcmp(got, want) != 0) {
            fprintf(stderr, "  event %d: got \"%s\", want \"%s\"\n", i, got, want);
        }
        CHECK(strcmp(got, want) == 0);
    }
    for (i = 0; i < 40; i++) {
        f = fopen(report, "r");
        if (f != NULL) {
            break;
        }
        usleep(250000);
    }
    CHECK(f != NULL);
    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL) {
            if (strncmp(line, "DONE", 4) == 0) {
                saw_done = 1;
            } else {
                saw_fail = 1;
                fprintf(stderr, "  server: %s", line);
            }
        }
        fclose(f);
    }
    CHECK(saw_done && !saw_fail);
}

int main(int argc, char **argv)
{
    pid_t plain, secure;
    int status;
    char rp[300], rs[300];

    if (argc != 8) {
        fprintf(stderr, "usage: gw_smoke SERVER_SCRIPT PYTHON PORT_PLAIN PORT_TLS CERT KEY REPORT_PREFIX\n");
        return 2;
    }
    seed_rng();
    signal(SIGPIPE, SIG_IGN);
    sprintf(rp, "%s.plain", argv[7]);
    sprintf(rs, "%s.tls", argv[7]);
    remove(rp);
    remove(rs);

    plain = fork();
    if (plain == 0) {
        execlp(argv[2], argv[2], argv[1], argv[3], rp, (char *)NULL);
        _exit(127);
    }
    secure = fork();
    if (secure == 0) {
        execlp(argv[2], argv[2], argv[1], argv[4], rs, argv[5], argv[6], (char *)NULL);
        _exit(127);
    }

    run_suite(atoi(argv[3]), 0, rp);
    run_suite(atoi(argv[4]), 1, rs);

    kill(plain, SIGTERM);
    kill(secure, SIGTERM);
    waitpid(plain, &status, 0);
    waitpid(secure, &status, 0);
    printf("gw_smoke: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
