/* The Discord client engine: see client.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client.h"
#include "rng.h"

enum { R_IDLE = 0, R_CONNECTING, R_SENT };
enum { G_NONE = 0, G_WAIT, G_CONNECTING, G_UPGRADING, G_OPEN };

#define UPGRADE_TIMEOUT_MS 20000L

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

void kc_free(KClient *kc)
{
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

/* ---------------------------------------------------------------- the tick */

void kc_tick(KClient *kc, KMs now)
{
    rest_pump(kc, now);
    gateway_pump(kc, now);
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
