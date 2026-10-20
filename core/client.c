/* The Discord client engine: see client.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client.h"
#include <sys/time.h>
#include "rng.h"

enum { R_IDLE = 0, R_CONNECTING, R_SENT };
enum { G_NONE = 0, G_WAIT, G_CONNECTING, G_UPGRADING, G_OPEN };

#define UPGRADE_TIMEOUT_MS 20000L

/* wall-clock milliseconds, for timing a picture's download and decoding (differences only; wraps harmlessly) */
static unsigned long wall_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (unsigned long)tv.tv_sec * 1000UL + (unsigned long)tv.tv_usec / 1000UL;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);

    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void emit(KClient *kc, int type, int state, const char *text)
{
    KEvent *e;

    if (kc->nevs >= 16) {
        return;
    }
    e = &kc->evs[kc->nevs++];
    e->type = type;
    e->state = state;
    copy_str(e->text, sizeof(e->text), text);
}

static void set_state(KClient *kc, int state, const char *text)
{
    kc->state = state;
    emit(kc, KE_STATE, state, text);
}

int kc_next_event(KClient *kc, KEvent *ev)
{
    int i;

    if (kc->nevs == 0) {
        return 0;
    }
    *ev = kc->evs[0];
    for (i = 1; i < kc->nevs; i++) {
        kc->evs[i - 1] = kc->evs[i];
    }
    kc->nevs--;
    return 1;
}

int kc_next_change(KClient *kc, ModelChange *ch)
{
    return model_next_change(&kc->model, ch);
}

Model *kc_model(KClient *kc)
{
    return &kc->model;
}

/* ---------------------------------------------------------------- lifecycle */

void kc_init(KClient *kc, const char *token, int is_bot)
{
    memset(kc, 0, sizeof(*kc));
    copy_str(kc->token, sizeof(kc->token), token != NULL ? token : "");
    kc->is_bot = is_bot;
    copy_str(kc->rest_host, sizeof(kc->rest_host), "discord.com");
    kc->rest_port = 443;
    kc->rest_tls = 1;
    kc->check_dates = 1;
    net_init(&kc->rconn);
    net_init(&kc->gconn);
    net_init(&kc->fconn);
    rest_init(&kc->rest, kc->token, is_bot);
    gw_init(&kc->gw, kc->token,
            is_bot ? (GW_INTENT_GUILDS | GW_INTENT_GUILD_MESSAGES | GW_INTENT_DIRECT_MESSAGES |
                  GW_INTENT_MESSAGE_CONTENT) : 0);
    model_init(&kc->model);
    kc->state = KC_IDLE;
}

static void drop_rest(KClient *kc)
{
    if (kc->rphase != R_IDLE) {
        http_req_free(&kc->rreq);
        http_resp_free(&kc->rresp);
        kc->rphase = R_IDLE;
    }
    net_close(&kc->rconn);
}

static void drop_gateway(KClient *kc)
{
    if (kc->gphase >= G_UPGRADING) {
        ws_free(&kc->ws);
    }
    if (kc->gphase == G_UPGRADING) {
        http_resp_free(&kc->gresp);
    }
    kc->gphase = G_NONE;
    net_close(&kc->gconn);
}

static void drop_fetch(KClient *kc)
{
    if (kc->fphase != 0) {
        http_req_free(&kc->freq);
        http_resp_free(&kc->fresp);
        kc->fphase = 0;
    }
    net_close(&kc->fconn);
}

void kc_free(KClient *kc)
{
    int i;

    drop_fetch(kc);
    for (i = 0; i < kc->nimgs; i++) {
        free(kc->imgs[i].tiff);
    }
    kc->nimgs = 0;
    drop_rest(kc);
    drop_gateway(kc);
    gw_free(&kc->gw);
    rest_free(&kc->rest);
    model_free(&kc->model);
    memset(kc->token, 0, sizeof(kc->token));
}

void kc_start(KClient *kc, KMs now)
{
    if (kc->started) {
        return;
    }
    kc->started = 1;
    copy_str(kc->rest.host, sizeof(kc->rest.host), kc->rest_host);
    kc->rest.port = kc->rest_port;
    kc->rest.tls = kc->rest_tls;
    if ((kc->rest_tls || strncmp(kc->gateway_url, "wss", 3) == 0) && kc->trust == NULL) {
        set_state(kc, KC_FATAL, "No certificate trust store is available");
        return;
    }
    if (!ssh_rng_ready()) {
        set_state(kc, KC_FATAL, "The random number generator is not seeded yet");
        return;
    }
    kc->verify_id = rest_get_me(&kc->rest);
    if (kc->verify_id <= 0) {
        set_state(kc, KC_FATAL, "Could not queue the login check");
        return;
    }
    set_state(kc, KC_VERIFYING, "Checking the token...");
}

void kc_stop(KClient *kc)
{
    if (kc->state == KC_STOPPED || !kc->started) {
        return;
    }
    gw_stop(&kc->gw, 0);
    drop_gateway(kc);
    drop_rest(kc);
    drop_fetch(kc);
    kc->state = KC_STOPPED;
    emit(kc, KE_STATE, KC_STOPPED, "Disconnected");
}

/* ---------------------------------------------------------------- REST transport */

static void pend_add(KClient *kc, long id, int kind, const char *channel_id)
{
    KcPending *p;

    if (id <= 0 || kc->npend >= KC_MAX_PENDING) {
        return;
    }
    p = &kc->pend[kc->npend++];
    p->id = id;
    p->kind = kind;
    copy_str(p->channel_id, sizeof(p->channel_id), channel_id != NULL ? channel_id : "");
}

static int pend_take(KClient *kc, long id, KcPending *out)
{
    int i;

    for (i = 0; i < kc->npend; i++) {
        if (kc->pend[i].id == id) {
            *out = kc->pend[i];
            kc->pend[i] = kc->pend[kc->npend - 1];
            kc->npend--;
            return 1;
        }
    }
    return 0;
}

static int pend_has_channel(const KClient *kc, int kind, const char *channel_id)
{
    int i;

    for (i = 0; i < kc->npend; i++) {
        if (kc->pend[i].kind == kind && strcmp(kc->pend[i].channel_id, channel_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void begin_gateway(KClient *kc, KMs now);

static void handle_result(KClient *kc, const RestResult *res, KMs now)
{
    KcPending p;
    char msg[300];

    if (!pend_take(kc, res->id, &p)) {
        p.kind = res->id == kc->verify_id ? REST_ME : 0;
        p.channel_id[0] = '\0';
    }
    switch (p.kind) {
    case REST_ME:
        if (!res->ok) {
            if (res->status == 401 || res->status == 403) {
                set_state(kc, KC_FATAL, "Discord rejected the token. Check that it is correct and for the right kind of account.");
            } else {
                sprintf(msg, "Could not log in: %.200s", res->error);
                set_state(kc, KC_FATAL, msg);
            }
            return;
        }
        copy_str(kc->user_name, sizeof(kc->user_name), json_get_str(res->body, "username", "?"));
        set_state(kc, KC_CONNECTING, "Connecting to the gateway...");
        begin_gateway(kc, now);
        break;
    case REST_MESSAGES:
        if (res->ok) {
            model_ingest_history(&kc->model, p.channel_id, res->body);
        } else {
            sprintf(msg, "Could not load messages: %.200s", res->error);
            emit(kc, KE_ERROR, kc->state, msg);
        }
        break;
    case REST_POST_MESSAGE:
        if (res->ok) {
            model_ingest_message(&kc->model, res->body, 1);
        } else {
            sprintf(msg, "Message not sent: %.200s", res->error);
            emit(kc, KE_ERROR, kc->state, msg);
        }
        break;
    case REST_CREATE_DM:
        if (res->ok) {
            model_ingest_private_channel(&kc->model, res->body);
            emit(kc, KE_CHANNEL, kc->state, json_get_str(res->body, "id", ""));
        } else {
            sprintf(msg, "Could not start the conversation: %.200s", res->error);
            emit(kc, KE_ERROR, kc->state, msg);
        }
        break;
    default:
        break;
    }
}

static void rest_send_request(KClient *kc)
{
    if (net_write(&kc->rconn, kc->rreq.buf, kc->rreq.len) != 0) {
        return;                                  /* the failure shows up as the connection's state */
    }
    http_resp_init(&kc->rresp, REST_MAX_RESPONSE, 0);
    kc->rphase = R_SENT;
    kc->rgot = 0;
}

static void rest_finish(KClient *kc, KMs now, const char *failure)
{
    /* the request is over, one way or the other */
    if (failure != NULL) {
        rest_failed(&kc->rest, now, failure);
    }
    http_req_free(&kc->rreq);
    if (kc->rphase == R_SENT) {
        http_resp_free(&kc->rresp);
    }
    kc->rphase = R_IDLE;
}

static int open_rest(KClient *kc, KMs now)
{
    der_time utc;

    net_utc_now(&utc);
    return net_open(&kc->rconn, kc->rest_host, kc->rest_port, kc->rest_tls, kc->trust,
                    kc->check_dates ? &utc : NULL, now);
}

static void rest_pump(KClient *kc, KMs now)
{
    long id;
    RestResult res;
    int rc;
    size_t n, used;
    const unsigned char *p;

    if (kc->state == KC_FATAL || kc->state == KC_STOPPED || !kc->started) {
        return;
    }
    if (kc->rphase == R_IDLE && rest_next_request(&kc->rest, now, &kc->rreq, &id)) {
        kc->rreused = 0;
        if (kc->rconn.state == NET_OPEN) {
            kc->rreused = 1;
            rest_send_request(kc);
        } else {
            net_close(&kc->rconn);
            if (open_rest(kc, now) == 0) {
                kc->rphase = R_CONNECTING;
            } else {
                char m[260];

                sprintf(m, "%.240s", kc->rconn.err);
                kc->rphase = R_CONNECTING;
                rest_finish(kc, now, m);
                net_close(&kc->rconn);
            }
        }
    }
    if (kc->rconn.state != NET_IDLE) {
        net_pump(&kc->rconn, now);
    }
    if (kc->rphase == R_CONNECTING) {
        if (kc->rconn.state == NET_OPEN) {
            rest_send_request(kc);
        } else if (kc->rconn.state == NET_FAILED || kc->rconn.state == NET_CLOSED) {
            char m[260];

            sprintf(m, "%.240s", kc->rconn.state == NET_FAILED ? kc->rconn.err : "connection closed");
            rest_finish(kc, now, m);
            net_close(&kc->rconn);
        }
    }
    if (kc->rphase == R_SENT) {
        p = net_peek(&kc->rconn, &n);
        rc = HTTP_NEED_MORE;
        if (n > 0) {
            kc->rgot = 1;
            rc = http_resp_feed(&kc->rresp, p, n, &used);
            net_consume(&kc->rconn, used);
        }
        if (rc == HTTP_NEED_MORE && (kc->rconn.state == NET_CLOSED || kc->rconn.state == NET_FAILED)) {
            net_peek(&kc->rconn, &n);
            if (n == 0) {
                rc = kc->rgot ? http_resp_eof(&kc->rresp) : HTTP_FAILED;
            }
        }
        if (rc == HTTP_DONE) {
            int keep = kc->rresp.keep_alive;

            rest_response(&kc->rest, now, &kc->rresp);
            rest_finish(kc, now, NULL);
            if (!keep) {
                net_close(&kc->rconn);
            }
        } else if (rc == HTTP_FAILED) {
            if (!kc->rgot && kc->rreused) {
                /* the server closed an idle connection under us: once more on a fresh one */
                http_resp_free(&kc->rresp);
                net_close(&kc->rconn);
                kc->rreused = 0;
                if (open_rest(kc, now) == 0) {
                    kc->rphase = R_CONNECTING;
                } else {
                    kc->rphase = R_CONNECTING;
                    rest_finish(kc, now, kc->rconn.err);
                    net_close(&kc->rconn);
                }
            } else {
                char m[260];

                sprintf(m, "%.240s", kc->rresp.err[0] != '\0' ? kc->rresp.err :
                        (kc->rconn.state == NET_FAILED ? kc->rconn.err : "the connection was lost"));
                rest_finish(kc, now, m);
                net_close(&kc->rconn);
            }
        }
    }
    if (kc->rphase == R_IDLE && (kc->rconn.state == NET_CLOSED || kc->rconn.state == NET_FAILED)) {
        net_close(&kc->rconn);
    }
    while (rest_next_result(&kc->rest, &res)) {
        handle_result(kc, &res, now);
    }
}

/* ---------------------------------------------------------------- gateway transport */

static void gw_events(KClient *kc)
{
    GwEvent ev;
    char msg[300];

    while (gw_next_event(&kc->gw, &ev)) {
        switch (ev.type) {
        case GW_EV_READY:
            model_gateway_event(&kc->model, &ev);
            sprintf(msg, "Connected as %.100s", kc->user_name);
            set_state(kc, KC_ONLINE, msg);
            break;
        case GW_EV_RESUMED:
            set_state(kc, KC_ONLINE, "Reconnected");
            break;
        case GW_EV_DISPATCH:
            model_gateway_event(&kc->model, &ev);
            break;
        case GW_EV_DISCONNECTED:
            if (ev.will_reconnect) {
                set_state(kc, KC_CONNECTING, "Connection lost; reconnecting...");
            }
            break;
        case GW_EV_FATAL:
            set_state(kc, KC_FATAL, ev.text);
            break;
        default:
            break;
        }
    }
}

static void begin_gateway(KClient *kc, KMs now)
{
    gw_start(&kc->gw, now);
}

static void gateway_connect(KClient *kc, KMs now)
{
    HttpUrl u;
    der_time utc;

    if (http_parse_url(kc->gurl, &u) != 0) {
        gw_connection_failed(&kc->gw, now);
        kc->gphase = G_NONE;
        return;
    }
    net_utc_now(&utc);
    if (net_open(&kc->gconn, u.host, u.port, u.tls, kc->trust, kc->check_dates ? &utc : NULL, now) != 0) {
        net_close(&kc->gconn);
        kc->gphase = G_NONE;
        gw_connection_failed(&kc->gw, now);
        return;
    }
    kc->gphase = G_CONNECTING;
    kc->gphase_at = now;
}

static void gateway_lost(KClient *kc, KMs now, int was_open)
{
    drop_gateway(kc);
    if (was_open) {
        gw_connection_lost(&kc->gw, 0, now);
    } else {
        gw_connection_failed(&kc->gw, now);
    }
}

static void gateway_pump(KClient *kc, KMs now)
{
    GwCommand cmd;
    WsEvent wev;
    const unsigned char *p, *out;
    size_t n, used, outlen;
    int rc;

    if (kc->state == KC_FATAL || kc->state == KC_STOPPED || !kc->started) {
        return;
    }
    while (gw_next_command(&kc->gw, &cmd)) {
        if (cmd.type == GW_CMD_CONNECT) {
            if (kc->gateway_url[0] != '\0' && strcmp(cmd.url, GW_DEFAULT_URL) == 0) {
                copy_str(kc->gurl, sizeof(kc->gurl), kc->gateway_url);
            } else {
                copy_str(kc->gurl, sizeof(kc->gurl), cmd.url);
            }
            kc->gconnect_at = now + cmd.delay_ms;
            kc->gphase = G_WAIT;
        } else if (cmd.type == GW_CMD_CLOSE) {
            if (kc->gphase == G_OPEN) {
                out = ws_output(&kc->ws, &outlen);
                if (outlen > 0) {
                    net_write(&kc->gconn, out, outlen);
                    ws_output_done(&kc->ws, outlen);
                }
            }
            drop_gateway(kc);
        }
    }
    gw_events(kc);
    if (kc->gphase == G_WAIT && KMS_SINCE(now, kc->gconnect_at) >= 0) {
        gateway_connect(kc, now);
    }
    if (kc->gphase >= G_CONNECTING) {
        net_pump(&kc->gconn, now);
    }
    if (kc->gphase == G_CONNECTING) {
        if (kc->gconn.state == NET_OPEN) {
            HttpReq q;

            if (ws_init(&kc->ws, kc->gurl) != 0) {
                drop_gateway(kc);
                gw_connection_failed(&kc->gw, now);
                return;
            }
            kc->ws.rng = ssh_rng_bytes;
            http_req_init(&q);
            if (ws_handshake_begin(&kc->ws, &q) != 0 || http_req_header(&q, "User-Agent", "Kallisti") != 0 ||
                http_req_end(&q, NULL, 0) != 0 || net_write(&kc->gconn, q.buf, q.len) != 0) {
                http_req_free(&q);
                ws_free(&kc->ws);
                drop_gateway(kc);
                gw_connection_failed(&kc->gw, now);
                return;
            }
            http_req_free(&q);
            http_resp_init(&kc->gresp, 65536, 0);
            kc->gphase = G_UPGRADING;
            kc->gphase_at = now;
        } else if (kc->gconn.state == NET_FAILED || kc->gconn.state == NET_CLOSED ||
                   KMS_SINCE(now, kc->gphase_at) > 2 * NET_CONNECT_TIMEOUT_MS) {
            drop_gateway(kc);
            gw_connection_failed(&kc->gw, now);
        }
    }
    if (kc->gphase == G_UPGRADING) {
        p = net_peek(&kc->gconn, &n);
        rc = HTTP_NEED_MORE;
        if (n > 0) {
            rc = http_resp_feed(&kc->gresp, p, n, &used);
            net_consume(&kc->gconn, used);
        }
        if (rc == HTTP_DONE) {
            if (ws_handshake_check(&kc->ws, &kc->gresp) == 0) {
                http_resp_free(&kc->gresp);
                kc->gphase = G_OPEN;
                gw_connection_opened(&kc->gw, &kc->ws, now);
                p = net_peek(&kc->gconn, &n);        /* frames that arrived with the 101 */
                if (n > 0) {
                    ws_input(&kc->ws, p, n);
                    net_consume(&kc->gconn, n);
                }
            } else {
                http_resp_free(&kc->gresp);
                ws_free(&kc->ws);
                kc->gphase = G_NONE;
                net_close(&kc->gconn);
                gw_connection_failed(&kc->gw, now);
            }
        } else if (rc == HTTP_FAILED || kc->gconn.state == NET_FAILED || kc->gconn.state == NET_CLOSED ||
                   KMS_SINCE(now, kc->gphase_at) > UPGRADE_TIMEOUT_MS) {
            http_resp_free(&kc->gresp);
            ws_free(&kc->ws);
            kc->gphase = G_NONE;
            net_close(&kc->gconn);
            gw_connection_failed(&kc->gw, now);
        }
    }
    if (kc->gphase == G_OPEN) {
        p = net_peek(&kc->gconn, &n);
        if (n > 0) {
            if (ws_input(&kc->ws, p, n) != 0) {
                /* a protocol violation: the engine queued a close frame and an ERROR event */
            }
            net_consume(&kc->gconn, n);
        }
        while (kc->gphase == G_OPEN && ws_next_event(&kc->ws, &wev)) {
            gw_handle_ws_event(&kc->gw, &wev, now);
            gw_events(kc);                           /* before the next frame: a DISPATCH's data is short-lived */
            if (wev.type == WS_EV_CLOSE || ws_is_closed(&kc->ws)) {
                out = ws_output(&kc->ws, &outlen);
                if (outlen > 0) {
                    net_write(&kc->gconn, out, outlen);
                    ws_output_done(&kc->ws, outlen);
                    net_pump(&kc->gconn, now);
                }
                drop_gateway(kc);
            }
        }
        if (kc->gphase == G_OPEN) {
            if (kc->gconn.state == NET_FAILED || kc->gconn.state == NET_CLOSED) {
                gateway_lost(kc, now, 1);
            }
        }
    }
    gw_tick(&kc->gw, now);
    if (kc->gphase == G_OPEN) {
        out = ws_output(&kc->ws, &outlen);
        if (outlen > 0) {
            if (net_write(&kc->gconn, out, outlen) == 0) {
                ws_output_done(&kc->ws, outlen);
            }
        }
    }
    gw_events(kc);
}

/* ---------------------------------------------------------------- picture downloads */

/* The address to request: the attachment's own, plus the media proxy's format and size parameters. */
static void fetch_url(char *out, size_t cap, const char *url, unsigned long w, unsigned long h, unsigned long max_w,
                      unsigned long max_h, int flags)
{
    char extra[96];
    unsigned long nw = w, nh = h;

    if (w > 0 && h > 0 && (w > max_w || h > max_h)) {
        /* shrink to fit, keeping the proportions (the proxy treats width and height as a box) */
        if (w * max_h > h * max_w) {
            nw = max_w;
            nh = (h * max_w + w / 2) / w;
        } else {
            nh = max_h;
            nw = (w * max_h + h / 2) / h;
        }
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        sprintf(extra, "format=%s&width=%lu&height=%lu", (flags & KC_FETCH_JPEG) ? "jpeg" : "png", nw, nh);
    } else {
        strcpy(extra, (flags & KC_FETCH_JPEG) ? "format=jpeg" : "format=png");
    }
    if (strlen(url) + strlen(extra) + 2 >= cap) {
        out[0] = '\0';
        return;
    }
    sprintf(out, "%s%c%s", url, strchr(url, '?') != NULL ? '&' : '?', extra);
}

int kc_fetch_image(KClient *kc, const char *url, unsigned long w, unsigned long h)
{
    return kc_fetch_image_boxed(kc, url, w, h, KC_IMAGE_MAX_W, KC_IMAGE_MAX_H, 0);
}

int kc_fetch_image_boxed(KClient *kc, const char *url, unsigned long w, unsigned long h, unsigned long max_w,
                         unsigned long max_h, unsigned long tag)
{
    return kc_fetch_image_ex(kc, url, w, h, max_w, max_h, tag, 0);
}

int kc_fetch_image_ex(KClient *kc, const char *url, unsigned long w, unsigned long h, unsigned long max_w,
                      unsigned long max_h, unsigned long tag, int flags)
{
    HttpUrl u;

    if (max_w < 1 || max_h < 1 || max_w > KC_IMAGE_MAX_W || max_h > KC_IMAGE_MAX_H) {
        return -1;
    }
    if (kc->nfq >= KC_IMAGE_QUEUE || strlen(url) >= sizeof(kc->fq[0].url) - 100 || http_parse_url(url, &u) != 0 ||
        (u.tls && kc->trust == NULL)) {
        return -1;
    }
    copy_str(kc->fq[kc->nfq].url, sizeof(kc->fq[0].url), url);
    kc->fq[kc->nfq].w = w;
    kc->fq[kc->nfq].h = h;
    kc->fq[kc->nfq].max_w = max_w;
    kc->fq[kc->nfq].max_h = max_h;
    kc->fq[kc->nfq].tag = tag;
    kc->fq[kc->nfq].flags = flags;
    kc->nfq++;
    return 0;
}

int kc_next_image(KClient *kc, KImage *out)
{
    int i;

    if (kc->nimgs == 0) {
        return 0;
    }
    *out = kc->imgs[0];
    for (i = 1; i < kc->nimgs; i++) {
        kc->imgs[i - 1] = kc->imgs[i];
    }
    kc->nimgs--;
    return 1;
}

static void fetch_result(KClient *kc, int ok, const char *error, unsigned char *tiff, size_t tiff_len,
                         unsigned long w, unsigned long h)
{
    KImage *r = &kc->imgs[kc->nimgs];

    memset(r, 0, sizeof(*r));
    copy_str(r->url, sizeof(r->url), kc->forig);
    r->ok = ok;
    r->tag = kc->ftag;
    r->bytes = kc->fbytes;
    r->ms_connect = kc->fwall_open - kc->fwall_start;
    r->ms_download = kc->fwall_done - kc->fwall_open;
    r->ms_decode = kc->fms_decode;
    copy_str(r->error, sizeof(r->error), error);
    r->tiff = tiff;
    r->tiff_len = tiff_len;
    r->width = w;
    r->height = h;
    kc->nimgs++;
    http_req_free(&kc->freq);
    if (kc->fphase == 2) {
        http_resp_free(&kc->fresp);
    }
    kc->fphase = 0;
    net_close(&kc->fconn);
}

static void fetch_failed(KClient *kc, const char *why)
{
    char msg[200];

    sprintf(msg, "Could not load the picture: %.150s", why);
    fetch_result(kc, 0, msg, NULL, 0, 0, 0);
}

/* Opens kc->furl; returns 0, or fails the fetch and returns -1. */
static int fetch_open(KClient *kc, KMs now)
{
    HttpUrl u;
    der_time utc;

    if (http_parse_url(kc->furl, &u) != 0) {
        kc->fphase = 0;
        fetch_failed(kc, "bad address");
        return -1;
    }
    net_utc_now(&utc);
    if (net_open(&kc->fconn, u.host, u.port, u.tls, kc->trust, kc->check_dates ? &utc : NULL, now) != 0) {
        char m[240];

        sprintf(m, "%.200s", kc->fconn.err);
        kc->fphase = 0;
        fetch_failed(kc, m);
        return -1;
    }
    kc->fphase = 1;
    return 0;
}

static void fetch_pump(KClient *kc, KMs now)
{
    const unsigned char *p;
    size_t n, used;
    int rc, i;
    HttpUrl u;

    if (kc->fphase == 0 && kc->nfq > 0 && kc->nimgs < KC_IMAGE_QUEUE) {
        copy_str(kc->forig, sizeof(kc->forig), kc->fq[0].url);
        kc->fq_w_saved = kc->fq[0].w;
        kc->fq_h_saved = kc->fq[0].h;
        kc->fmax_w = kc->fq[0].max_w;
        kc->fmax_h = kc->fq[0].max_h;
        kc->ftag = kc->fq[0].tag;
        kc->fflags = kc->fq[0].flags;
        fetch_url(kc->furl, sizeof(kc->furl), kc->fq[0].url, kc->fq[0].w, kc->fq[0].h, kc->fmax_w, kc->fmax_h, kc->fflags);
        for (i = 1; i < kc->nfq; i++) {
            kc->fq[i - 1] = kc->fq[i];
        }
        kc->nfq--;
        kc->fredirects = 0;
        kc->fretried = 0;
        kc->fstarted = now;
        kc->fwall_start = kc->fwall_open = kc->fwall_done = wall_ms();
        kc->fbytes = 0;
        kc->fms_decode = 0;
        if (kc->furl[0] == '\0') {
            fetch_failed(kc, "address too long");
        } else {
            fetch_open(kc, now);
        }
    }
    if (kc->fphase == 0) {
        return;
    }
    if (KMS_SINCE(now, kc->fstarted) > 60000L) {
        fetch_failed(kc, "timed out");
        return;
    }
    net_pump(&kc->fconn, now);
    if (kc->fphase == 1) {
        if (kc->fconn.state == NET_OPEN) {
            if (http_parse_url(kc->furl, &u) != 0) {
                fetch_failed(kc, "bad address");
                return;
            }
            http_req_init(&kc->freq);
            if (http_req_begin(&kc->freq, "GET", u.path, u.host, u.port, u.tls) != 0 ||
                http_req_header(&kc->freq, "User-Agent", kc->rest.user_agent) != 0 ||
                http_req_header(&kc->freq, "Accept", "image/png, image/jpeg") != 0 || http_req_end(&kc->freq, NULL, 0) != 0 ||
                net_write(&kc->fconn, kc->freq.buf, kc->freq.len) != 0) {
                fetch_failed(kc, "could not send the request");
                return;
            }
            http_resp_init(&kc->fresp, (size_t)KC_IMAGE_BODY_MAX, 0);
            kc->fwall_open = wall_ms();
            kc->fphase = 2;
        } else if (kc->fconn.state == NET_FAILED || kc->fconn.state == NET_CLOSED) {
            char m[240];

            sprintf(m, "%.200s", kc->fconn.state == NET_FAILED ? kc->fconn.err : "connection closed");
            fetch_failed(kc, m);
        }
        return;
    }
    /* phase 2: reading the reply */
    p = net_peek(&kc->fconn, &n);
    rc = HTTP_NEED_MORE;
    if (n > 0) {
        rc = http_resp_feed(&kc->fresp, p, n, &used);
        net_consume(&kc->fconn, used);
    }
    if (rc == HTTP_NEED_MORE && (kc->fconn.state == NET_CLOSED || kc->fconn.state == NET_FAILED)) {
        net_peek(&kc->fconn, &n);
        if (n == 0) {
            rc = http_resp_eof(&kc->fresp);
            if (rc == HTTP_NEED_MORE) {
                rc = HTTP_FAILED;
            }
        }
    }
    if (rc == HTTP_FAILED) {
        char m[240];

        sprintf(m, "%.200s", kc->fresp.err[0] != '\0' ? kc->fresp.err : "the connection was lost");
        fetch_failed(kc, m);
    } else if (rc == HTTP_DONE) {
        int status = kc->fresp.status;

        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
            http_resp_header(&kc->fresp, "Location") != NULL) {
            char loc[512];
            const char *l = http_resp_header(&kc->fresp, "Location");

            if (kc->fredirects >= 3 || strlen(l) >= sizeof(loc)) {
                fetch_failed(kc, "too many redirects");
                return;
            }
            copy_str(loc, sizeof(loc), l);
            if (http_parse_url(kc->furl, &u) != 0) {
                fetch_failed(kc, "bad address");
                return;
            }
            if (loc[0] == '/') {                          /* a path on the same host */
                sprintf(kc->furl, "%s://%.200s:%d%.250s", u.tls ? "https" : "http", u.host, u.port, loc);
            } else {
                copy_str(kc->furl, sizeof(kc->furl), loc);
            }
            kc->fredirects++;
            http_req_free(&kc->freq);
            http_resp_free(&kc->fresp);
            net_close(&kc->fconn);
            fetch_open(kc, now);
        } else if (status != 200) {
            char m[80];

            sprintf(m, "the server answered HTTP %d", status);
            fetch_failed(kc, m);
        } else {
            unsigned char *tiff = NULL;
            size_t tl = 0;
            unsigned long w = 0, h = 0;
            const char *err = "ok";

            unsigned long t0;
            int dec;

            kc->fwall_done = wall_ms();
            kc->fbytes = (unsigned long)kc->fresp.body_len;
            t0 = wall_ms();
            dec = img_to_tiff(kc->fresp.body, kc->fresp.body_len, kc->fmax_w, kc->fmax_h, &tiff, &tl, &w, &h, &err);
            kc->fms_decode = wall_ms() - t0;
            if (dec != 0 && (kc->fflags & KC_FETCH_JPEG) && !kc->fretried && strstr(err, "PNG or JPEG") != NULL) {
                /* the proxy would not make a JPEG (it sent something else): ask for a PNG instead */
                kc->fretried = 1;
                kc->fflags = 0;
                fetch_url(kc->furl, sizeof(kc->furl), kc->forig, kc->fq_w_saved, kc->fq_h_saved, kc->fmax_w, kc->fmax_h, 0);
                http_req_free(&kc->freq);
                http_resp_free(&kc->fresp);
                net_close(&kc->fconn);
                fetch_open(kc, now);
                return;
            }
            if (dec == 0) {
                fetch_result(kc, 1, "", tiff, tl, w, h);
            } else {
                fetch_failed(kc, err);
            }
        }
    }
}

/* ---------------------------------------------------------------- the tick */

void kc_tick(KClient *kc, KMs now)
{
    rest_pump(kc, now);
    gateway_pump(kc, now);
    fetch_pump(kc, now);
}

/* ---------------------------------------------------------------- actions */

void kc_open_channel(KClient *kc, const char *channel_id)
{
    Channel *c = model_find_channel(&kc->model, channel_id);
    long id;

    if (c == NULL || c->history_loaded || pend_has_channel(kc, REST_MESSAGES, channel_id) || !kc->started) {
        return;
    }
    id = rest_get_messages(&kc->rest, channel_id, KC_HISTORY_LIMIT, NULL);
    pend_add(kc, id, REST_MESSAGES, channel_id);
}

int kc_send_message(KClient *kc, const char *channel_id, const char *text)
{
    long id = rest_post_message(&kc->rest, channel_id, text);

    if (id <= 0) {
        return (int)id;
    }
    pend_add(kc, id, REST_POST_MESSAGE, channel_id);
    return 0;
}

int kc_open_dm(KClient *kc, const char *user_id)
{
    long id = rest_create_dm(&kc->rest, user_id);

    if (id <= 0) {
        return (int)id;
    }
    pend_add(kc, id, REST_CREATE_DM, NULL);
    return 0;
}
