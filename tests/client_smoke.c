/*
 * client_smoke.c -- the whole client engine (core/client.c: REST, gateway, WebSocket, TLS, model) against a
 * scripted fake Discord (tests/client_smoke_server.py: REST on one port, the gateway on the next), once in
 * plaintext and once over TLS with certificate validation against a throwaway CA.
 *
 * usage: client_smoke SERVER_SCRIPT PYTHON PORT_PLAIN PORT_TLS CHAIN KEY CA_PEM
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../core/client.h"
#include "../core/rng.h"
#include "../core/roots.h"

static int t_pass, t_fail;
#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static KMs now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (KMs)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
}

typedef struct {
    int states[32];
    char texts[32][240];
    int nstates;
    char errors[8][240];
    int nerrors;
} Seen;

static void drain_events(KClient *kc, Seen *s)
{
    KEvent ev;

    while (kc_next_event(kc, &ev)) {
        if (ev.type == KE_STATE && s->nstates < 32) {
            s->states[s->nstates] = ev.state;
            strcpy(s->texts[s->nstates], ev.text);
            s->nstates++;
        } else if (ev.type == KE_ERROR && s->nerrors < 8) {
            strcpy(s->errors[s->nerrors++], ev.text);
        }
    }
}

/* Ticks until cond() holds or the time runs out. */
typedef int (*Cond)(KClient *kc, Seen *s);

static int run_until(KClient *kc, Seen *s, Cond cond, long limit_ms)
{
    KMs start = now_ms();
    ModelChange ch;

    while (KMS_SINCE(now_ms(), start) < limit_ms) {
        kc_tick(kc, now_ms());
        drain_events(kc, s);
        while (kc_next_change(kc, &ch)) {
            ;
        }
        if (cond(kc, s)) {
            return 1;
        }
        usleep(5000);
    }
    return 0;
}

static int c_online(KClient *kc, Seen *s) { return kc->state == KC_ONLINE; }
static int c_fatal(KClient *kc, Seen *s) { return kc->state == KC_FATAL; }
static int c_history(KClient *kc, Seen *s)
{
    Channel *c = model_find_channel(&kc->model, "200");

    return c != NULL && c->history_loaded;
}
static int c_error1(KClient *kc, Seen *s) { return s->nerrors >= 1; }
static int c_has_hello(KClient *kc, Seen *s)
{
    Channel *c = model_find_channel(&kc->model, "200");
    int i;

    for (i = 0; c != NULL && i < c->nmsgs; i++) {
        if (strcmp(c->msgs[i].text, "hello") == 0) {
            return 1;
        }
    }
    return 0;
}
static int c_has_limited(KClient *kc, Seen *s)
{
    Channel *c = model_find_channel(&kc->model, "200");
    int i;

    for (i = 0; c != NULL && i < c->nmsgs; i++) {
        if (strcmp(c->msgs[i].text, "ratelimit") == 0) {
            return 1;
        }
    }
    return 0;
}
static int c_has_missed(KClient *kc, Seen *s)
{
    Channel *c = model_find_channel(&kc->model, "200");
    int i;

    for (i = 0; c != NULL && i < c->nmsgs; i++) {
        if (strcmp(c->msgs[i].text, "sent while you were away") == 0) {
            return 1;
        }
    }
    return 0;
}
static int c_stopped(KClient *kc, Seen *s) { return kc->state == KC_STOPPED; }

static int count_text(KClient *kc, const char *text)
{
    Channel *c = model_find_channel(&kc->model, "200");
    int i, n = 0;

    for (i = 0; c != NULL && i < c->nmsgs; i++) {
        if (strcmp(c->msgs[i].text, text) == 0) {
            n++;
        }
    }
    return n;
}

static void configure(KClient *kc, int port, int tls, const TrustStore *ts)
{
    strcpy(kc->rest_host, "localhost");
    kc->rest_port = port;
    kc->rest_tls = tls;
    sprintf(kc->gateway_url, "%s://localhost:%d/?v=10&encoding=json", tls ? "wss" : "ws", port + 1);
    kc->trust = ts;
}

static void test_good_session(int port, int tls, const TrustStore *ts)
{
    KClient kc;
    Seen seen;
    Channel *c;
    Guild *g;
    KMs t0;

    memset(&seen, 0, sizeof(seen));
    kc_init(&kc, "goodtoken", 1);
    configure(&kc, port, tls, ts);
    kc_start(&kc, now_ms());
    CHECK(kc.state == KC_VERIFYING);
    CHECK(run_until(&kc, &seen, c_online, 15000));
    CHECK(seen.nstates >= 3 && seen.states[0] == KC_VERIFYING && seen.states[1] == KC_CONNECTING &&
          seen.states[2] == KC_ONLINE && strcmp(seen.texts[2], "Connected as Kallisti-bot") == 0);

    /* the gateway filled the model */
    g = model_find_guild(&kc.model, "100");
    CHECK(g != NULL && strcmp(g->name, "Test Guild") == 0 && g->nch == 3);
    CHECK(g != NULL && g->nch == 3 && strcmp(g->ch[0].id, "210") == 0 && strcmp(g->ch[1].id, "200") == 0 &&
          strcmp(g->ch[2].id, "201") == 0);
    c = model_find_channel(&kc.model, "200");
    CHECK(c != NULL && c->nmsgs == 1 && strcmp(c->msgs[0].text, "hi @Kallisti-bot from #secret") == 0);
    CHECK(c != NULL && c->unread == 1 && c->mentions == 1);

    /* history over REST */
    kc_open_channel(&kc, "200");
    CHECK(run_until(&kc, &seen, c_history, 10000));
    c = model_find_channel(&kc.model, "200");
    CHECK(c != NULL && c->nmsgs == 4 && strcmp(c->msgs[0].text, "oldest") == 0 && strcmp(c->msgs[2].text, "newest @Kallisti-bot") == 0 &&
          strcmp(c->msgs[3].id, "20") == 0);
    kc_open_channel(&kc, "200");                         /* already loaded: nothing more is asked */
    CHECK(kc.npend == 0);

    /* a channel we cannot read */
    kc_open_channel(&kc, "201");
    CHECK(run_until(&kc, &seen, c_error1, 10000));
    CHECK(strstr(seen.errors[0], "Missing Access") != NULL && !model_find_channel(&kc.model, "201")->history_loaded);

    /* sending: the POST result and the gateway's echo are one message */
    CHECK(kc_send_message(&kc, "200", "hello") == 0);
    CHECK(run_until(&kc, &seen, c_has_hello, 10000));
    t0 = now_ms();
    while (KMS_SINCE(now_ms(), t0) < 500) {              /* let the echo arrive too */
        kc_tick(&kc, now_ms());
        usleep(5000);
    }
    CHECK(count_text(&kc, "hello") == 1);
    CHECK(kc_send_message(&kc, "200", "") == REST_E_BAD);

    /* a 429 is waited out */
    t0 = now_ms();
    CHECK(kc_send_message(&kc, "200", "ratelimit") == 0);
    CHECK(run_until(&kc, &seen, c_has_limited, 10000));
    CHECK(KMS_SINCE(now_ms(), t0) >= 250);

    /* a rejected message is reported */
    seen.nerrors = 0;
    CHECK(kc_send_message(&kc, "200", "fail") == 0);
    CHECK(run_until(&kc, &seen, c_error1, 10000));
    CHECK(strstr(seen.errors[0], "Message not sent") != NULL && strstr(seen.errors[0], "empty message") != NULL);

    /* the server tells us to reconnect: we resume and get what we missed */
    seen.nstates = 0;
    CHECK(kc_send_message(&kc, "200", "kick") == 0);
    CHECK(run_until(&kc, &seen, c_has_missed, 15000));
    CHECK(run_until(&kc, &seen, c_online, 5000));
    CHECK(count_text(&kc, "sent while you were away") == 1);
    {
        int saw_connecting = 0, i;

        for (i = 0; i < seen.nstates; i++) {
            if (seen.states[i] == KC_CONNECTING) {
                saw_connecting = 1;
            }
        }
        CHECK(saw_connecting);
    }

    kc_stop(&kc);
    CHECK(run_until(&kc, &seen, c_stopped, 1000));
    kc_free(&kc);
}

static void test_bad_token(int port, int tls, const TrustStore *ts)
{
    KClient kc;
    Seen seen;

    memset(&seen, 0, sizeof(seen));
    kc_init(&kc, "badtoken", 1);
    configure(&kc, port, tls, ts);
    kc_start(&kc, now_ms());
    CHECK(run_until(&kc, &seen, c_fatal, 10000));
    CHECK(strstr(seen.texts[seen.nstates - 1], "rejected the token") != NULL);
    kc_free(&kc);
}

static void test_untrusted(int port, const TrustStore *empty)
{
    KClient kc;
    Seen seen;

    memset(&seen, 0, sizeof(seen));
    kc_init(&kc, "goodtoken", 1);
    configure(&kc, port, 1, empty);
    kc_start(&kc, now_ms());
    CHECK(run_until(&kc, &seen, c_fatal, 20000));
    CHECK(strstr(seen.texts[seen.nstates - 1], "trusted authority") != NULL);
    kc_free(&kc);
}

static void test_unreachable(void)
{
    KClient kc;
    Seen seen;

    memset(&seen, 0, sizeof(seen));
    kc_init(&kc, "goodtoken", 1);
    strcpy(kc.rest_host, "localhost");
    kc.rest_port = 1;                                    /* nothing listens there */
    kc.rest_tls = 0;
    kc_start(&kc, now_ms());
    CHECK(run_until(&kc, &seen, c_fatal, 30000));
    CHECK(strstr(seen.texts[seen.nstates - 1], "Could not") != NULL);
    kc_free(&kc);
}

int main(int argc, char **argv)
{
    pid_t plain, secure;
    int status, pp, pt;
    TrustStore ca, empty;
    FILE *f;
    static char pem[16384];
    size_t n;

    if (argc != 8) {
        fprintf(stderr, "usage: client_smoke SERVER PYTHON PORT_PLAIN PORT_TLS CHAIN KEY CA\n");
        return 2;
    }
    if (ssh_rng_seed_system() == 0) {
        ssh_rng_add("client smoke", 12, 256);
    }
    signal(SIGPIPE, SIG_IGN);
    pp = atoi(argv[3]);
    pt = atoi(argv[4]);
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
    trust_init(&ca);
    trust_init(&empty);
    f = fopen(argv[7], "rb");
    n = f != NULL ? fread(pem, 1, sizeof(pem), f) : 0;
    if (f != NULL) {
        fclose(f);
    }
    CHECK(trust_add_pem(&ca, pem, n) == 1);
    usleep(1500000);                                     /* let the servers start */

    printf("plaintext\n");
    test_good_session(pp, 0, NULL);
    test_bad_token(pp, 0, NULL);
    printf("tls\n");
    test_good_session(pt, 1, &ca);
    test_bad_token(pt, 1, &ca);
    test_untrusted(pt, &empty);
    test_unreachable();

    kill(plain, SIGTERM);
    kill(secure, SIGTERM);
    waitpid(plain, &status, 0);
    waitpid(secure, &status, 0);
    trust_free(&ca);
    trust_free(&empty);
    printf("client_smoke: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
