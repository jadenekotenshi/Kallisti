/* Discord gateway session: see gateway.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gateway.h"
#include "rng.h"

#define EVQ 6
#define CMDQ 4
#define QUERY "v=10&encoding=json"

/* ---------------------------------------------------------------- small helpers */

static long ms_since(GwMs later, GwMs earlier)
{
    return (long)(int)(later - earlier);     /* signed difference modulo 2^32: tolerates wraparound */
}

static unsigned long rand16(Gateway *gw)
{
    unsigned char b[2];

    if (gw->rng(b, 2) != 0) {
        return 32768UL;
    }
    return ((unsigned long)b[0] << 8) | b[1];
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

/* ---------------------------------------------------------------- close-code policy */

int gw_close_is_fatal(int code, const char **why)
{
    static const char *tok = "Discord rejected the token (authentication failed): check that the token is correct";
    static const char *shard = "Discord says the shard settings are wrong (this client does not shard)";
    static const char *ver = "Discord says the gateway API version is no longer supported: Kallisti needs updating";
    static const char *inv = "Discord rejected the intents value as invalid";
    static const char *dis = "Discord refused the requested intents: enable the Message Content intent (and "
                             "Server Members/Presence if requested) for this bot in the Discord Developer Portal";

    switch (code) {
    case 4004: *why = tok; return 1;
    case 4010: case 4011: *why = shard; return 1;
    case 4012: *why = ver; return 1;
    case 4013: *why = inv; return 1;
    case 4014: *why = dis; return 1;
    default:
        *why = NULL;
        return 0;
    }
}

int gw_close_invalidates_session(int code)
{
    return code == 4007 || code == 4009 || code == 1000 || code == 1001;
}

/* ---------------------------------------------------------------- queues */

static GwEvent *push_event(Gateway *gw, int type)
{
    GwEvent *e;

    if (gw->nevs >= EVQ) {
        return NULL;                         /* the driver is not draining: drop rather than overwrite */
    }
    e = &gw->evs[gw->nevs++];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->seq = -1;
    return e;
}

static void push_command(Gateway *gw, int type, const char *url, GwMs delay)
{
    GwCommand *c;

    if (gw->ncmds >= CMDQ) {
        return;
    }
    c = &gw->cmds[gw->ncmds++];
    memset(c, 0, sizeof(*c));
    c->type = type;
    if (url != NULL) {
        copy_str(c->url, sizeof(c->url), url);
    }
    c->delay_ms = delay;
}

int gw_next_command(Gateway *gw, GwCommand *cmd)
{
    if (gw->ncmds == 0) {
        return 0;
    }
    *cmd = gw->cmds[0];
    memmove(&gw->cmds[0], &gw->cmds[1], (size_t)(gw->ncmds - 1) * sizeof(GwCommand));
    gw->ncmds--;
    return 1;
}

int gw_next_event(Gateway *gw, GwEvent *ev)
{
    if (gw->nevs == 0) {
        return 0;
    }
    *ev = gw->evs[0];
    memmove(&gw->evs[0], &gw->evs[1], (size_t)(gw->nevs - 1) * sizeof(GwEvent));
    gw->nevs--;
    return 1;
}

/* ---------------------------------------------------------------- sending */

static int send_json(Gateway *gw, JWriter *w)
{
    int rc = -1;

    if (gw->ws != NULL && !w->failed) {
        rc = ws_send_text(gw->ws, w->buf, w->len);
    }
    jw_free(w);
    return rc;
}

static void send_heartbeat(Gateway *gw, GwMs now)
{
    JWriter w;

    jw_init(&w);
    jw_begin_obj(&w);
    jw_key(&w, "op");
    jw_int(&w, 1);
    jw_key(&w, "d");
    if (gw->seq >= 0) {
        jw_int(&w, gw->seq);
    } else {
        jw_null(&w);
    }
    jw_end_obj(&w);
    send_json(gw, &w);
    gw->ack_pending = 1;
    gw->hb_sent_at = now;
}

static void send_auth(Gateway *gw)
{
    JWriter w;

    jw_init(&w);
    jw_begin_obj(&w);
    if (gw->can_resume && gw->session_id[0] != '\0') {
        jw_key(&w, "op");
        jw_int(&w, 6);
        jw_key(&w, "d");
        jw_begin_obj(&w);
        jw_key(&w, "token");
        jw_str(&w, gw->token);
        jw_key(&w, "session_id");
        jw_str(&w, gw->session_id);
        jw_key(&w, "seq");
        jw_int(&w, gw->seq >= 0 ? gw->seq : 0);
        jw_end_obj(&w);
    } else {
        jw_key(&w, "op");
        jw_int(&w, 2);
        jw_key(&w, "d");
        jw_begin_obj(&w);
        jw_key(&w, "token");
        jw_str(&w, gw->token);
        jw_key(&w, "intents");
        jw_int(&w, gw->intents);
        jw_key(&w, "properties");
        jw_begin_obj(&w);
        jw_key(&w, "os");
        jw_str(&w, "openstep");
        jw_key(&w, "browser");
        jw_str(&w, "Kallisti");
        jw_key(&w, "device");
        jw_str(&w, "Kallisti");
        jw_end_obj(&w);
        jw_end_obj(&w);
    }
    jw_end_obj(&w);
    send_json(gw, &w);
    gw->state = GW_AUTHENTICATING;
}

/* ---------------------------------------------------------------- lifecycle */

void gw_init(Gateway *gw, const char *token, long intents)
{
    memset(gw, 0, sizeof(*gw));
    copy_str(gw->token, sizeof(gw->token), token);
    gw->intents = intents;
    gw->rng = ssh_rng_bytes;
    gw->seq = -1;
    gw->latency_ms = -1;
    gw->state = GW_IDLE;
}

static void drop_doc(Gateway *gw)
{
    if (gw->have_doc) {
        json_free(&gw->doc);
        gw->have_doc = 0;
    }
}

void gw_free(Gateway *gw)
{
    drop_doc(gw);
    memset(gw->token, 0, sizeof(gw->token));
}

static void forget_session(Gateway *gw)
{
    gw->can_resume = 0;
    gw->session_id[0] = '\0';
    gw->seq = -1;
}

static const char *connect_url(const Gateway *gw)
{
    if (gw->can_resume && gw->resume_url[0] != '\0') {
        return gw->resume_url;
    }
    return GW_DEFAULT_URL;
}

void gw_start(Gateway *gw, GwMs now)
{
    (void)now;
    if (gw->state != GW_IDLE) {
        return;
    }
    gw->failures = 0;
    gw->state = GW_WAIT_CONNECT;
    push_command(gw, GW_CMD_CONNECT, connect_url(gw), 0);
}

void gw_stop(Gateway *gw, GwMs now)
{
    int had_connection = gw->ws != NULL;

    (void)now;
    if (had_connection) {
        ws_send_close(gw->ws, 1000, NULL);
    }
    gw->ws = NULL;
    gw->ncmds = 0;                           /* a pending connect no longer applies */
    if (had_connection) {
        push_command(gw, GW_CMD_CLOSE, NULL, 0);
    }
    gw->hb_interval = 0;
    gw->auth_pending = 0;
    gw->ack_pending = 0;
    forget_session(gw);
    gw->state = GW_IDLE;
}

static GwMs backoff_ms(int failures)
{
    GwMs d;
    int shift;

    if (failures <= 1) {
        return 0;
    }
    shift = failures - 2;
    if (shift > 6) {
        shift = 6;
    }
    d = 1000U << shift;
    return d > 60000U ? 60000U : d;
}

/* The connection is gone (or about to be): decide between giving up and reconnecting. */
static void conn_down(Gateway *gw, int code, GwMs now, int want_close, int resumable)
{
    const char *why;
    GwEvent *e;

    (void)now;
    if (gw->state == GW_IDLE || gw->state == GW_FATAL) {
        gw->ws = NULL;
        return;
    }
    if (want_close && gw->ws != NULL) {
        push_command(gw, GW_CMD_CLOSE, NULL, 0);
    }
    gw->ws = NULL;
    gw->hb_interval = 0;
    gw->ack_pending = 0;
    gw->auth_pending = 0;

    if (gw_close_is_fatal(code, &why)) {
        gw->state = GW_FATAL;
        copy_str(gw->fatal, sizeof(gw->fatal), why);
        e = push_event(gw, GW_EV_FATAL);
        if (e != NULL) {
            e->code = code;
            copy_str(e->text, sizeof(e->text), why);
        }
        return;
    }
    if (!resumable || gw_close_invalidates_session(code)) {
        forget_session(gw);
    }
    gw->failures++;
    e = push_event(gw, GW_EV_DISCONNECTED);
    if (e != NULL) {
        e->code = code;
        e->will_reconnect = 1;
    }
    gw->state = GW_WAIT_CONNECT;
    push_command(gw, GW_CMD_CONNECT, connect_url(gw), backoff_ms(gw->failures));
}

void gw_connection_opened(Gateway *gw, WsClient *ws, GwMs now)
{
    (void)now;
    if (gw->state != GW_WAIT_CONNECT) {
        return;
    }
    gw->ws = ws;
    gw->hb_interval = 0;
    gw->ack_pending = 0;
    gw->auth_pending = 0;
    gw->state = GW_WAIT_HELLO;
}

void gw_connection_failed(Gateway *gw, GwMs now)
{
    if (gw->state != GW_WAIT_CONNECT) {
        return;
    }
    /* the connect attempt itself failed: same policy as a drop, but nothing to close */
    conn_down(gw, 0, now, 0, 1);
}

void gw_connection_lost(Gateway *gw, int close_code, GwMs now)
{
    conn_down(gw, close_code, now, 0, 1);
}

/* We end the connection ourselves (op 7, a missed ACK, ...): close it and reconnect right away. */
static void reconnect_now(Gateway *gw, GwMs now, int resumable)
{
    if (gw->ws != NULL) {
        ws_send_close(gw->ws, 4000, "reconnecting");
    }
    conn_down(gw, 4000, now, 1, resumable);
    /* an intentional reconnect is not a failure: no backoff */
    if (gw->failures > 0) {
        gw->failures--;
    }
    if (gw->ncmds > 0 && gw->cmds[gw->ncmds - 1].type == GW_CMD_CONNECT) {
        gw->cmds[gw->ncmds - 1].delay_ms = 0;
    }
}

/* ---------------------------------------------------------------- incoming frames */

static void on_ready(Gateway *gw, const JNode *d)
{
    const JNode *user = json_get(d, "user");
    const char *url = json_get_str(d, "resume_gateway_url", NULL);
    const char *sid = json_get_str(d, "session_id", NULL);
    size_t n;

    copy_str(gw->user_id, sizeof(gw->user_id), json_get_str(user, "id", ""));
    copy_str(gw->user_name, sizeof(gw->user_name), json_get_str(user, "username", ""));
    if (sid != NULL) {
        copy_str(gw->session_id, sizeof(gw->session_id), sid);
        gw->can_resume = 1;
    }
    if (url != NULL && url[0] != '\0') {
        copy_str(gw->resume_url, sizeof(gw->resume_url), url);
        if (strchr(gw->resume_url, '?') == NULL) {
            n = strlen(gw->resume_url);
            if (n > 0 && gw->resume_url[n - 1] != '/') {
                strncat(gw->resume_url, "/", sizeof(gw->resume_url) - strlen(gw->resume_url) - 1);
            }
            strncat(gw->resume_url, "?" QUERY, sizeof(gw->resume_url) - strlen(gw->resume_url) - 1);
        }
    }
}

static void on_dispatch(Gateway *gw, const JNode *root, GwMs now)
{
    const char *t = json_get_str(root, "t", NULL);
    const JNode *d = json_get(root, "d");
    long s;
    GwEvent *e;

    (void)now;
    if (json_int(json_get(root, "s"), &s)) {
        gw->seq = s;
    }
    if (t == NULL) {
        gw->bad_frames++;
        return;
    }
    if (strcmp(t, "READY") == 0) {
        on_ready(gw, d);
        gw->state = GW_READY;
        gw->failures = 0;
        e = push_event(gw, GW_EV_READY);
        if (e != NULL) {
            copy_str(e->name, sizeof(e->name), gw->user_name);
            copy_str(e->user_id, sizeof(e->user_id), gw->user_id);
            e->d = d;
            e->seq = gw->seq;
        }
    } else if (strcmp(t, "RESUMED") == 0) {
        gw->state = GW_READY;
        gw->failures = 0;
        e = push_event(gw, GW_EV_RESUMED);
        if (e != NULL) {
            e->seq = gw->seq;
        }
        return;
    }
    e = push_event(gw, GW_EV_DISPATCH);
    if (e != NULL) {
        copy_str(e->name, sizeof(e->name), t);
        e->d = d;
        e->seq = gw->seq;
    }
}

static void on_text(Gateway *gw, const char *text, size_t len, GwMs now)
{
    const JNode *root, *d;
    long op, interval;
    int resumable;

    drop_doc(gw);
    root = json_parse(&gw->doc, text, len);
    gw->have_doc = 1;
    if (root == NULL || root->type != J_OBJ || !json_int(json_get(root, "op"), &op)) {
        gw->bad_frames++;
        return;
    }
    d = json_get(root, "d");
    switch (op) {
    case 0:
        on_dispatch(gw, root, now);
        break;
    case 1:
        send_heartbeat(gw, now);
        break;
    case 7:
        reconnect_now(gw, now, 1);
        break;
    case 9:
        if (json_bool(d, &resumable) && resumable) {
            /* the session may still be resumable */
        } else {
            forget_session(gw);
        }
        gw->auth_pending = 1;
        gw->auth_at = now + 1000U + (GwMs)(rand16(gw) % 4000UL);
        break;
    case 10:
        if (!json_int(json_get(d, "heartbeat_interval"), &interval) || interval < 1000 || interval > 600000L) {
            gw->bad_frames++;
            reconnect_now(gw, now, 0);
            break;
        }
        gw->hb_interval = (GwMs)interval;
        gw->next_hb = now + (GwMs)((double)interval * ((double)rand16(gw) / 65536.0));
        gw->ack_pending = 0;
        send_auth(gw);
        break;
    case 11:
        if (gw->ack_pending) {
            gw->latency_ms = ms_since(now, gw->hb_sent_at);
            gw->ack_pending = 0;
        }
        break;
    default:
        break;
    }
}

void gw_handle_ws_event(Gateway *gw, const WsEvent *ev, GwMs now)
{
    if (gw->ws == NULL) {
        return;
    }
    switch (ev->type) {
    case WS_EV_TEXT:
        on_text(gw, (const char *)ev->data, ev->len, now);
        break;
    case WS_EV_CLOSE:
        conn_down(gw, ev->code == 1005 ? 0 : ev->code, now, 1, 1);
        break;
    case WS_EV_ERROR:
        conn_down(gw, 0, now, 1, 1);
        break;
    default:
        break;                               /* binary frames, pings and pongs mean nothing here */
    }
}

/* ---------------------------------------------------------------- time */

void gw_tick(Gateway *gw, GwMs now)
{
    if (gw->ws == NULL) {
        return;
    }
    if (gw->hb_interval != 0 && ms_since(now, gw->next_hb) >= 0) {
        if (gw->ack_pending) {
            reconnect_now(gw, now, 1);       /* no ACK since the last beat: a zombied connection */
            return;
        }
        send_heartbeat(gw, now);
        gw->next_hb += gw->hb_interval;
        if (ms_since(now, gw->next_hb) >= 0) {
            gw->next_hb = now + gw->hb_interval;
        }
    }
    if (gw->auth_pending && ms_since(now, gw->auth_at) >= 0) {
        gw->auth_pending = 0;
        send_auth(gw);
    }
}

long gw_next_deadline(const Gateway *gw, GwMs now)
{
    long best = -1, d;

    if (gw->ws == NULL) {
        return -1;
    }
    if (gw->hb_interval != 0) {
        d = ms_since(gw->next_hb, now);
        best = d < 0 ? 0 : d;
    }
    if (gw->auth_pending) {
        d = ms_since(gw->auth_at, now);
        if (d < 0) {
            d = 0;
        }
        if (best < 0 || d < best) {
            best = d;
        }
    }
    return best;
}
