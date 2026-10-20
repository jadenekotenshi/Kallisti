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
    char infos[4][240];
    int ninfos;
    char channels[4][24];
    int nchannels;
    int nimages, image_ok[8], image_frames[8];
    unsigned image_delay0[8];
    unsigned long image_tag[8];
    char image_err[8][160];
    unsigned long image_w[8], image_h[8];
    char image_url[8][512];
    unsigned long image_len[8], image_sum[8];
} Seen;

static void drain_events(KClient *kc, Seen *s)
{
    KEvent ev;
    KImage im;

    while (kc_next_event(kc, &ev)) {
        if (ev.type == KE_STATE && s->nstates < 32) {
            s->states[s->nstates] = ev.state;
            strcpy(s->texts[s->nstates], ev.text);
            s->nstates++;
        } else if (ev.type == KE_ERROR && s->nerrors < 8) {
            strcpy(s->errors[s->nerrors++], ev.text);
        } else if (ev.type == KE_INFO && s->ninfos < 4) {
            strcpy(s->infos[s->ninfos++], ev.text);
        } else if (ev.type == KE_CHANNEL && s->nchannels < 4) {
            strcpy(s->channels[s->nchannels++], ev.text);
        }
    }
    while (kc_next_image(kc, &im)) {
        if (s->nimages < 8) {
            s->image_ok[s->nimages] = im.ok;
            s->image_frames[s->nimages] = im.anim.n;
            s->image_delay0[s->nimages] = im.anim.n > 0 ? im.anim.delay_ms[0] : 0;
            s->image_tag[s->nimages] = im.tag;
            strcpy(s->image_err[s->nimages], im.error);
            s->image_w[s->nimages] = im.width;
            s->image_h[s->nimages] = im.height;
            strcpy(s->image_url[s->nimages], im.url);
            s->image_len[s->nimages] = (unsigned long)im.tiff_len;
            s->image_sum[s->nimages] = 0;
            if (im.tiff != NULL && im.tiff_len < 200000UL) {
                size_t k;

                for (k = 0; k < im.tiff_len; k++) {
                    s->image_sum[s->nimages] = s->image_sum[s->nimages] * 31UL + im.tiff[k];
                }
            }
            s->nimages++;
        }
        kc_image_free(&im);
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
static int c_info1(KClient *kc, Seen *s) { return s->ninfos >= 1; }
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
static int c_has_dm901(KClient *kc, Seen *s)
{
    Channel *c = model_find_channel(&kc->model, "901");

    return c != NULL && c->nmsgs == 1;
}
static int c_has_dmhi(KClient *kc, Seen *s)
{
    Channel *c = model_find_channel(&kc->model, "900");

    return c != NULL && c->nmsgs == 1;
}
static int c_channel_event(KClient *kc, Seen *s) { return s->nchannels >= 1; }
static int c_images4(KClient *kc, Seen *s) { return s->nimages >= 4; }
static int c_images1(KClient *kc, Seen *s) { return s->nimages >= 1; }
static int c_has_text(KClient *kc, const char *text)
{
    Channel *c = model_find_channel(&kc->model, "200");
    int i;

    for (i = 0; c != NULL && i < c->nmsgs; i++) {
        if (strcmp(c->msgs[i].text, text) == 0) {
            return 1;
        }
    }
    return 0;
}
static int c_idle_rest(KClient *kc, Seen *s) { return rest_pending(&kc->rest) == 0 && kc->npend == 0; }
static int count_text(KClient *kc, const char *text);
static int c_has_upload(KClient *kc, Seen *s)
{
    return count_text(kc, "with files [notes.bin:300000:ok tiny.dat:5:ok]\n[attachment: notes.bin]\n[attachment: tiny.dat]") > 0;
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

static int c_has_reactme(KClient *kc, Seen *s)
{
    Channel *c = model_find_channel(&kc->model, "200");
    int i;

    for (i = 0; c != NULL && i < c->nmsgs; i++) {
        if (strcmp(c->msgs[i].text, "reactme") == 0) {
            return 1;
        }
    }
    return 0;
}

static int react_count(KClient *kc)
{
    Channel *c = model_find_channel(&kc->model, "200");
    int i;

    for (i = 0; c != NULL && i < c->nmsgs; i++) {
        if (strcmp(c->msgs[i].text, "reactme") == 0) {
            return c->msgs[i].nreact;
        }
    }
    return -1;
}
static int c_two_reacts(KClient *kc, Seen *s) { return react_count(kc) == 2; }
static int c_one_react(KClient *kc, Seen *s) { return react_count(kc) == 1; }

static int c_reacts(KClient *kc, Seen *s)
{
    Channel *c = model_find_channel(&kc->model, "200");
    int i;

    for (i = 0; c != NULL && i < c->nmsgs; i++) {
        if (strcmp(c->msgs[i].text, "reactme") == 0) {
            return c->msgs[i].nreact;
        }
    }
    return 0;
}

static Message *find_text(KClient *kc, const char *text)
{
    Channel *c = model_find_channel(&kc->model, "200");
    int i;

    for (i = 0; c != NULL && i < c->nmsgs; i++) {
        if (strcmp(c->msgs[i].text, text) == 0) {
            return &c->msgs[i];
        }
    }
    return NULL;
}

static void configure(KClient *kc, int port, int tls, const TrustStore *ts)
{
    strcpy(kc->rest_host, "localhost");
    kc->rest_port = port;
    kc->rest_tls = tls;
    sprintf(kc->gateway_url, "%s://localhost:%d/?v=10&encoding=json", tls ? "wss" : "ws", port + 1);
    kc->trust = ts;
    kc->proxy_env = 0;
    kc->resize_any_host = 1;                             /* the fake server stands in for Discord's media proxy */                                   /* whatever the shell exports must not leak into the tests */
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

    /* a DM from someone we have no channel with appears under Direct Messages, counted as a mention */
    CHECK(kc_send_message(&kc, "200", "dmme") == 0);
    CHECK(run_until(&kc, &seen, c_has_dm901, 10000));
    c = model_find_channel(&kc.model, "901");
    CHECK(c != NULL && c->type == CH_DM && strcmp(c->name, "eight") == 0 && c->unread == 1 && c->mentions == 1 &&
          strcmp(c->msgs[0].text, "psst, a DM") == 0);
    CHECK(kc.model.nguilds == 2 && strcmp(kc.model.guilds[0].id, "@me") == 0);

    /* starting a DM by user id: the channel arrives, can be written in, and a refusal is reported */
    CHECK(kc_open_dm(&kc, "") == REST_E_BAD);
    CHECK(kc_open_dm(&kc, "7") == 0);
    CHECK(run_until(&kc, &seen, c_channel_event, 10000));
    CHECK(strcmp(seen.channels[0], "900") == 0 && model_find_channel(&kc.model, "900") != NULL &&
          strcmp(model_find_channel(&kc.model, "900")->name, "someone") == 0);
    CHECK(kc_send_message(&kc, "900", "hello dm") == 0);
    CHECK(run_until(&kc, &seen, c_has_dmhi, 10000));
    CHECK(strcmp(model_find_channel(&kc.model, "900")->msgs[0].text, "hello dm") == 0);
    seen.nerrors = 0;
    CHECK(kc_open_dm(&kc, "666") == 0);
    CHECK(run_until(&kc, &seen, c_error1, 10000));
    CHECK(strstr(seen.errors[0], "Could not start the conversation") != NULL && strstr(seen.errors[0], "Cannot send messages") != NULL);

    /* pictures: fetched from the "media proxy" (no token sent), redirects followed, failures explained */
    {
        char base[160], u[240];

        sprintf(base, "%s://localhost:%d/img", tls ? "https" : "http", port);
        seen.nimages = 0;
        sprintf(u, "%s/cat.png?ex=1&hm=2", base);
        CHECK(kc_fetch_image(&kc, u, 0, 0) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_w[0] == 300 && seen.image_h[0] == 200 && strcmp(seen.image_url[0], u) == 0);

        seen.nimages = 0;
        sprintf(u, "%s/redirect", base);
        CHECK(kc_fetch_image(&kc, u, 0, 0) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_w[0] == 300);

        /* four at once come back in order, a mix of good and bad */
        seen.nimages = 0;
        sprintf(u, "%s/missing", base);
        CHECK(kc_fetch_image(&kc, u, 0, 0) == 0);
        sprintf(u, "%s/notpng", base);
        CHECK(kc_fetch_image(&kc, u, 0, 0) == 0);
        sprintf(u, "%s/sized", base);
        CHECK(kc_fetch_image(&kc, u, 2000, 1000) == 0);          /* asked for 800x400 */
        sprintf(u, "%s/cat.png", base);
        CHECK(kc_fetch_image(&kc, u, 640, 480) == 0);            /* fits: no size parameters */
        CHECK(kc_fetch_image(&kc, u, 0, 0) == -1);               /* the queue holds four */
        CHECK(run_until(&kc, &seen, c_images4, 30000));
        CHECK(!seen.image_ok[0] && strstr(seen.image_err[0], "404") != NULL);
        CHECK(!seen.image_ok[1] && strstr(seen.image_err[1], "PNG") != NULL);
        CHECK(seen.image_ok[2] && seen.image_ok[3] && seen.image_w[3] == 300);
        /* a thumbnail box: the server insists on width=320&height=213 for a 2000x1333 picture in a 320x240 box;
         * thumb2 sends the whole 300x200 picture whatever was asked, so the client shrinks it into 150x100 */
        seen.nimages = 0;
        sprintf(u, "%s/thumb", base);
        CHECK(kc_fetch_image_boxed(&kc, u, 2000, 1333, 320, 240, 7) == 0);
        CHECK(kc_fetch_image_boxed(&kc, u, 10, 10, 0, 240, 7) == -1 && kc_fetch_image_boxed(&kc, u, 10, 10, 320, 9999, 7) == -1);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_tag[0] == 7 && seen.image_w[0] == 300 && seen.image_h[0] == 200);   /* it fit */
        seen.nimages = 0;
        sprintf(u, "%s/thumb2", base);
        CHECK(kc_fetch_image_boxed(&kc, u, 300, 200, 150, 100, 8) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_tag[0] == 8 && seen.image_w[0] == 150 && seen.image_h[0] == 100);   /* shrunk here */
        /* another site's picture is fetched exactly as addressed: no format or size parameters are added */
        kc.resize_any_host = 0;
        seen.nimages = 0;
        sprintf(u, "%s/plainfetch?token=abc", base);
        CHECK(kc_fetch_image(&kc, u, 2000, 1000) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_w[0] == 300);
        kc.resize_any_host = 1;

        /* KC_FETCH_RAW: the address goes out untouched even on a host that would take parameters */
        seen.nimages = 0;
        sprintf(u, "%s/plainfetch?token=abc", base);
        CHECK(kc_fetch_image_ex(&kc, u, 2000, 1000, 800, 600, 3, KC_FETCH_RAW) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_tag[0] == 3);

        /* a GIF (the first frame of an animation) comes back like any other picture */
        seen.nimages = 0;
        sprintf(u, "%s/animated.gif", base);
        CHECK(kc_fetch_image(&kc, u, 25, 15) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_w[0] == 25 && seen.image_h[0] == 15);

        /* an animation: the GIF itself is asked for (resized, no format), and its frames come back with their delays */
        seen.nimages = 0;
        sprintf(u, "%s/animsized", base);
        CHECK(kc_fetch_image_ex(&kc, u, 1200, 600, 400, 300, 5, KC_FETCH_ANIM) == 0);      /* the server insists on width=400&height=200 */
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_frames[0] == 3 && seen.image_w[0] == 25 && seen.image_h[0] == 15 && seen.image_tag[0] == 5);
        seen.nimages = 0;
        sprintf(u, "%s/many.gif", base);
        CHECK(kc_fetch_image_ex(&kc, u, 16, 12, 400, 300, 6, KC_FETCH_ANIM) == 0);          /* 130 frames: cut at KC_ANIM_MAX_FRAMES */
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_frames[0] == KC_ANIM_MAX_FRAMES && seen.image_delay0[0] == 100);   /* delay 0 -> 100 ms */
        seen.nimages = 0;
        sprintf(u, "%s/plainfetch?token=abc", base);
        CHECK(kc_fetch_image_ex(&kc, u, 0, 0, 400, 300, 7, KC_FETCH_ANIM) == 0);            /* not a GIF: an ordinary picture */
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_frames[0] == 0 && seen.image_w[0] == 300);
        seen.nimages = 0;
        sprintf(u, "%s/single.gif", base);
        CHECK(kc_fetch_image_ex(&kc, u, 9, 9, 400, 300, 8, KC_FETCH_ANIM) == 0);            /* a one-frame GIF: ordinary too */
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_frames[0] == 0 && seen.image_w[0] == 9);

        /* a JPEG original: asked for as a JPEG, decoded as one; a PNG request may also come back as a JPEG */
        seen.nimages = 0;
        sprintf(u, "%s/photo.jpg", base);
        CHECK(kc_fetch_image_ex(&kc, u, 800, 600, 800, 600, 9, KC_FETCH_JPEG) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_tag[0] == 9 && seen.image_w[0] == 800 && seen.image_h[0] == 600);
        /* ... and if the proxy will not make a JPEG, the same picture is asked for again as a PNG */
        seen.nimages = 0;
        sprintf(u, "%s/nojpeg", base);
        CHECK(kc_fetch_image_ex(&kc, u, 300, 200, 800, 600, 10, KC_FETCH_JPEG) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && seen.image_tag[0] == 10 && seen.image_w[0] == 300 && seen.image_h[0] == 200);
        /* all of those went over ONE connection to the picture server; a reply that says Connection: close ends it,
         * and one that closes without saying so costs a quiet retry, never an error */
        CHECK(kc.fopens == 1);
        seen.nimages = 0;
        sprintf(u, "%s/closeme", base);
        CHECK(kc_fetch_image(&kc, u, 0, 0) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && kc.fopens == 1);
        seen.nimages = 0;
        sprintf(u, "%s/cat.png", base);
        CHECK(kc_fetch_image(&kc, u, 0, 0) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && kc.fopens == 2);                    /* a new connection after "close" */
        seen.nimages = 0;
        sprintf(u, "%s/slam", base);
        CHECK(kc_fetch_image(&kc, u, 0, 0) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0]);
        seen.nimages = 0;
        sprintf(u, "%s/cat.png", base);
        CHECK(kc_fetch_image(&kc, u, 0, 0) == 0);                     /* the kept connection is dead now */
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        CHECK(seen.image_ok[0] && kc.fopens >= 3 && kc.fopens <= 4);
        CHECK(kc_fetch_image(&kc, "ftp://nope/x.png", 0, 0) == -1 && kc_fetch_image(&kc, "garbage", 0, 0) == -1);

        /* a download: the bytes exactly as sent, from the address exactly as given (even with a format= in it) */
        seen.nimages = 0;
        sprintf(u, "%s/file.bin?sig=zz&format=png", base);
        CHECK(kc_fetch_file(&kc, u, 11) == 0);
        CHECK(run_until(&kc, &seen, c_images1, 15000));
        {
            unsigned long sum = 0;
            int i;

            for (i = 0; i < 70000; i++) {
                sum = sum * 31UL + (unsigned long)((i * 7 + i / 256) & 255);
            }
            CHECK(seen.image_ok[0] && seen.image_tag[0] == 11 && seen.image_len[0] == 70000UL);
            CHECK(seen.image_sum[0] == sum);
        }
    }

    /* reactions: put on and taken off with REST, shown through the gateway's events; a refusal is an error */
    {
        Message *rm;
        char mid[24];

        CHECK(kc_send_message(&kc, "200", "reactme") == 0);
        CHECK(run_until(&kc, &seen, c_has_reactme, 15000));
        rm = find_text(&kc, "reactme");
        CHECK(rm != NULL);
        strcpy(mid, rm != NULL ? rm->id : "1");
        CHECK(kc_react(&kc, "200", mid, "\xf0\x9f\x91\x8d", 0) == 0);
        CHECK(run_until(&kc, &seen, c_reacts, 15000));
        rm = find_text(&kc, "reactme");
        CHECK(rm != NULL && rm->nreact == 1 && rm->react[0].count == 1 && rm->react[0].me && strcmp(rm->react[0].name, "\xf0\x9f\x91\x8d") == 0);
        CHECK(kc_react(&kc, "200", mid, "blobcat:555", 0) == 0);
        CHECK(run_until(&kc, &seen, c_two_reacts, 15000));
        rm = find_text(&kc, "reactme");
        CHECK(rm != NULL && rm->nreact == 2 && strcmp(rm->react[1].id, "555") == 0 && strcmp(rm->react[1].name, "blobcat") == 0);
        CHECK(kc_react(&kc, "200", mid, "\xf0\x9f\x91\x8d", 1) == 0);
        CHECK(run_until(&kc, &seen, c_one_react, 15000));
        rm = find_text(&kc, "reactme");
        CHECK(rm != NULL && rm->nreact == 1 && strcmp(rm->react[0].id, "555") == 0);
        seen.nerrors = 0;
        CHECK(kc_react(&kc, "200", mid, "\xf0\x9f\x98\x88", 0) == 0);              /* the server refuses this one */
        CHECK(run_until(&kc, &seen, c_error1, 15000));
        CHECK(strstr(seen.errors[0], "Missing Permissions") != NULL);
        CHECK(kc_react(&kc, "200", mid, "", 0) < 0 && kc_react(&kc, "200", "abc", "x", 0) < 0 && kc_react(&kc, "bad", mid, "x", 0) < 0);
    }

    /* older messages: the page before the oldest one held is asked for, merged in order, and the end of the channel noticed */
    {
        Channel *oc = model_find_channel(&kc.model, "200");
        int had = oc->nmsgs;

        seen.ninfos = 0;
        CHECK(kc_load_older(&kc, "201") == -1 && kc_load_older(&kc, "999") == -1);      /* not loaded yet; unknown */
        CHECK(kc_load_older(&kc, "200") == 0);
        CHECK(kc_load_older(&kc, "200") == -1);                                         /* one request at a time */
        CHECK(run_until(&kc, &seen, c_info1, 15000));
        oc = model_find_channel(&kc.model, "200");
        CHECK(strstr(seen.infos[0], "Loaded 9 older messages") != NULL && strstr(seen.infos[0], "beginning") != NULL);
        CHECK(oc->nmsgs == had + 9 && oc->history_end && strcmp(oc->msgs[0].text, "older 1") == 0 && strcmp(oc->msgs[8].text, "older 9") == 0 &&
              strcmp(oc->msgs[9].text, "oldest") == 0);
        CHECK(kc_load_older(&kc, "200") == 1);                                          /* nothing before the beginning */
    }

    /* files attached to a message: sent as multipart, byte for byte (the server checks the pattern and the names) */
    {
        static unsigned char data[300000];
        RestFile f[2];
        size_t k;

        for (k = 0; k < sizeof(data); k++) {
            data[k] = (unsigned char)((k * 7 + k / 256) & 255);
        }
        f[0].name = "notes.bin";
        f[0].data = data;
        f[0].len = 300000;
        f[1].name = "tiny.dat";
        f[1].data = data;
        f[1].len = 5;
        CHECK(kc_send_files(&kc, "200", "with files", f, 2) == 0);
        CHECK(run_until(&kc, &seen, c_has_upload, 20000));
        CHECK(count_text(&kc, "with files [notes.bin:300000:ok tiny.dat:5:ok]\n[attachment: notes.bin]\n[attachment: tiny.dat]") == 1);
        CHECK(kc_send_files(&kc, "x", "t", f, 1) < 0 && kc_send_files(&kc, "200", "t", f, 0) < 0);
    }

    /* typing: a bodyless POST that nobody answers for; bad ids are refused up front */
    CHECK(kc_typing(&kc, "200") == 0 && kc_typing(&kc, "x") < 0 && kc_typing(&kc, "") < 0);
    CHECK(run_until(&kc, &seen, c_idle_rest, 5000));

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

static int log_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    char b[300];
    int n = 0;

    while (f != NULL && fgets(b, sizeof(b), f) != NULL) {
        n++;
    }
    if (f != NULL) {
        fclose(f);
    }
    return n;
}

static int log_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    char b[300];
    int found = 0;

    while (f != NULL && fgets(b, sizeof(b), f) != NULL) {
        if (strstr(b, needle) != NULL) {
            found = 1;
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    return found;
}

/* a whole session (login, gateway, history, one picture) with every connection going through a proxy */
static void test_proxied(int port, int tls, const TrustStore *ts, const char *spec, int use_env, const char *log)
{
    KClient kc;
    Seen seen;
    char b[160], u[200];

    memset(&seen, 0, sizeof(seen));
    kc_init(&kc, "goodtoken", 1);
    configure(&kc, port, tls, ts);
    if (use_env) {
        kc.proxy_env = 1;
    } else {
        strcpy(kc.proxy_spec, spec);
    }
    kc_start(&kc, now_ms());
    CHECK(run_until(&kc, &seen, c_online, 20000));
    CHECK(seen.nstates >= 3 && strstr(seen.texts[seen.nstates - 1], "through the proxy") != NULL);
    kc_open_channel(&kc, "200");
    CHECK(run_until(&kc, &seen, c_history, 15000));
    CHECK(model_find_channel(&kc.model, "200") != NULL && model_find_channel(&kc.model, "200")->nmsgs >= 3);
    sprintf(u, "%s://localhost:%d/img/cat.png", tls ? "https" : "http", port);
    seen.nimages = 0;
    CHECK(kc_fetch_image(&kc, u, 0, 0) == 0);
    CHECK(run_until(&kc, &seen, c_images1, 20000));
    CHECK(seen.image_ok[0] && seen.image_w[0] == 300);
    CHECK(kc_send_message(&kc, "200", "through a proxy") == 0);
    {
        KMs t0 = now_ms();

        while (KMS_SINCE(now_ms(), t0) < 3000 && !c_has_text(&kc, "through a proxy")) {
            kc_tick(&kc, now_ms());
            usleep(5000);
        }
        CHECK(c_has_text(&kc, "through a proxy"));
    }
    kc_stop(&kc);
    kc_free(&kc);
    sprintf(b, "CONNECT localhost:%d", port);
    CHECK(log_has(log, b));
    sprintf(b, "CONNECT localhost:%d", port + 1);
    CHECK(log_has(log, b));
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
    pid_t plain, secure, proxies;
    int pbase = 0;
    char spec[200];
    int status, pp, pt;
    TrustStore ca, empty;
    FILE *f;
    static char pem[16384];
    size_t n;

    if (argc != 11) {
        fprintf(stderr, "usage: client_smoke SERVER PYTHON PORT_PLAIN PORT_TLS CHAIN KEY CA PROXY_SERVER PROXY_BASE PROXY_LOG\n");
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
    pbase = atoi(argv[9]);
    remove(argv[10]);
    proxies = fork();
    if (proxies == 0) {
        execlp(argv[2], argv[2], argv[8], argv[9], argv[10], (char *)NULL);
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

    /* through proxies: HTTP CONNECT with a login over TLS, SOCKS5 plain, SOCKS5 with a login over TLS */
    printf("proxies\n");
    sprintf(spec, "http://bob:s3cret@127.0.0.1:%d", pbase + 1);
    test_proxied(pt, 1, &ca, spec, 0, argv[10]);
    sprintf(spec, "socks5://127.0.0.1:%d", pbase + 2);
    test_proxied(pp, 0, NULL, spec, 0, argv[10]);
    sprintf(spec, "socks5://bob:s3cret@127.0.0.1:%d", pbase + 3);
    test_proxied(pt, 1, &ca, spec, 0, argv[10]);
    {
        /* the environment: used when nothing is configured, and no_proxy keeps local servers direct */
        char envp[100], before_msg[10];
        int before;

        (void)before_msg;
        sprintf(envp, "http_proxy=http://127.0.0.1:%d", pbase);
        putenv(envp);
        test_proxied(pp, 0, NULL, "", 1, argv[10]);
        putenv((char *)"no_proxy=localhost");
        before = log_lines(argv[10]);
        {
            KClient kc;
            Seen seen;

            memset(&seen, 0, sizeof(seen));
            kc_init(&kc, "goodtoken", 1);
            configure(&kc, pp, 0, NULL);
            kc.proxy_env = 1;
            kc_start(&kc, now_ms());
            CHECK(run_until(&kc, &seen, c_online, 20000));
            CHECK(strcmp(seen.texts[seen.nstates - 1], "Connected as Kallisti-bot") == 0);     /* direct: no proxy note */
            kc_stop(&kc);
            kc_free(&kc);
        }
        CHECK(log_lines(argv[10]) == before);
        putenv((char *)"no_proxy=");
        putenv((char *)"http_proxy=");
    }
    /* bad settings are reported at once, and a proxy that is not there says so */
    {
        KClient kc;
        Seen seen;

        memset(&seen, 0, sizeof(seen));
        kc_init(&kc, "goodtoken", 1);
        configure(&kc, pp, 0, NULL);
        strcpy(kc.proxy_spec, "socks4://nowhere");
        kc_start(&kc, now_ms());
        CHECK(kc.state == KC_FATAL);
        kc_free(&kc);
        kc_init(&kc, "goodtoken", 1);
        configure(&kc, pp, 0, NULL);
        strcpy(kc.proxy_spec, "http://127.0.0.1:1");
        memset(&seen, 0, sizeof(seen));
        kc_start(&kc, now_ms());
        CHECK(run_until(&kc, &seen, c_fatal, 30000));
        CHECK(strstr(seen.texts[seen.nstates - 1], "proxy") != NULL);
        kc_free(&kc);
    }

    kill(proxies, SIGTERM);
    waitpid(proxies, &status, 0);
    kill(plain, SIGTERM);
    kill(secure, SIGTERM);
    waitpid(plain, &status, 0);
    waitpid(secure, &status, 0);
    trust_free(&ca);
    trust_free(&empty);
    printf("client_smoke: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
