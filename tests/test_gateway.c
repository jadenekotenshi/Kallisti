#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gateway.h"
#include "test.h"

/* ---- deterministic randomness: the gateway's jitter and the WebSocket's masks ---- */
static const unsigned char *gscript;
static size_t gscript_len, gscript_pos;

static int gw_test_rng(void *out, size_t len)
{
    size_t i;
    unsigned char *o = (unsigned char *)out;

    for (i = 0; i < len; i++) {
        o[i] = gscript_pos < gscript_len ? gscript[gscript_pos++] : 0x80;
    }
    return 0;
}

static void jitter(const unsigned char *s, size_t n)
{
    gscript = s;
    gscript_len = n;
    gscript_pos = 0;
}

static int mask_rng(void *out, size_t len)
{
    memset(out, 0x5a, len);
    return 0;
}

static void open_ws(WsClient *ws)
{
    ws_init(ws, "wss://gateway.example/?v=10&encoding=json");
    ws->rng = mask_rng;
    ws->state = WS_OPEN;
}

/* ---- what the gateway sent: decode the masked frames queued on the WsClient ---- */
typedef struct { int opcode; size_t len; unsigned char data[1024]; } Sent;

static int drain(WsClient *ws, Sent *out, int max)
{
    const unsigned char *p;
    size_t avail, used = 0, hl, plen, i;
    int n = 0;

    p = ws_output(ws, &avail);
    while (used < avail && n < max) {
        hl = 2;
        plen = p[used + 1] & 0x7f;
        if (plen == 126) {
            plen = ((size_t)p[used + 2] << 8) | p[used + 3];
            hl = 4;
        }
        out[n].opcode = p[used] & 15;
        out[n].len = plen < sizeof(out[n].data) - 1 ? plen : sizeof(out[n].data) - 1;
        for (i = 0; i < out[n].len; i++) {
            out[n].data[i] = (unsigned char)(p[used + hl + 4 + i] ^ p[used + hl + (i & 3)]);
        }
        out[n].data[out[n].len] = '\0';
        used += hl + 4 + plen;
        n++;
    }
    ws_output_done(ws, avail);
    return n;
}

/* the frame's JSON as a parsed document */
static JNode *parse_sent(const Sent *s, JDoc *d)
{
    return json_parse(d, (const char *)s->data, s->len);
}

static long sent_op(const Sent *s)
{
    JDoc d;
    long op = -1;
    JNode *n = parse_sent(s, &d);

    if (n != NULL) {
        json_int(json_get(n, "op"), &op);
    }
    json_free(&d);
    return op;
}

static void feed(Gateway *gw, const char *json, GwMs now)
{
    WsEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = WS_EV_TEXT;
    ev.data = (const unsigned char *)json;
    ev.len = strlen(json);
    gw_handle_ws_event(gw, &ev, now);
}

static void feed_close(Gateway *gw, int code, GwMs now)
{
    WsEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = WS_EV_CLOSE;
    ev.code = code;
    ev.text = "";
    gw_handle_ws_event(gw, &ev, now);
}

/* Collects every pending command/event into small summaries for assertions. */
typedef struct { int type; char url[320]; GwMs delay; } Cmd;
static int take_cmds(Gateway *gw, Cmd *out, int max)
{
    GwCommand c;
    int n = 0;

    while (gw_next_command(gw, &c) && n < max) {
        out[n].type = c.type;
        strcpy(out[n].url, c.url);
        out[n].delay = c.delay_ms;
        n++;
    }
    return n;
}

typedef struct { int type; char name[48]; char user[32]; char text[160]; long seq; int code, reconnect; char dtext[64]; } Ev;
static int take_events(Gateway *gw, Ev *out, int max)
{
    GwEvent e;
    int n = 0;

    while (gw_next_event(gw, &e) && n < max) {
        out[n].type = e.type;
        strcpy(out[n].name, e.name);
        strcpy(out[n].user, e.user_id);
        strcpy(out[n].text, e.text);
        out[n].seq = e.seq;
        out[n].code = e.code;
        out[n].reconnect = e.will_reconnect;
        out[n].dtext[0] = '\0';
        if (e.d != NULL && e.d->type == J_OBJ) {
            strncpy(out[n].dtext, json_get_str(e.d, "id", json_get_str(e.d, "content", "")), sizeof(out[n].dtext) - 1);
            out[n].dtext[sizeof(out[n].dtext) - 1] = '\0';
        }
        n++;
    }
    return n;
}

static const char *READY_JSON =
    "{\"op\":0,\"s\":1,\"t\":\"READY\",\"d\":{\"v\":10,\"user\":{\"id\":\"42\",\"username\":\"Kallisti\"},"
    "\"session_id\":\"sess-abc\",\"resume_gateway_url\":\"wss://gateway-us-east1-b.discord.gg\","
    "\"guilds\":[{\"id\":\"1\",\"unavailable\":true}]}}";

static const char *RESUME_URL = "wss://gateway-us-east1-b.discord.gg/?v=10&encoding=json";

/* Brings a gateway from nothing to READY at time `now` (returns with the READY events already drained). */
static void bring_up(Gateway *gw, WsClient *ws, GwMs now, unsigned long interval)
{
    static const unsigned char half[] = { 0x80, 0x00 };
    Cmd c[4];
    Ev e[6];
    Sent s[4];
    char hello[80];

    jitter(half, sizeof(half));
    gw_start(gw, now);
    take_cmds(gw, c, 4);
    open_ws(ws);
    gw_connection_opened(gw, ws, now);
    sprintf(hello, "{\"op\":10,\"d\":{\"heartbeat_interval\":%lu}}", interval);
    feed(gw, hello, now + 5);
    drain(ws, s, 4);
    feed(gw, READY_JSON, now + 100);
    take_events(gw, e, 6);
    take_cmds(gw, c, 4);
}

static void test_lifecycle(void)
{
    static const unsigned char half[] = { 0x80, 0x00 };           /* 0x8000/65536 = jitter 0.5 */
    Gateway gw;
    WsClient ws;
    Cmd c[4];
    Ev e[6];
    Sent s[6];
    JDoc d;
    JNode *n;
    int nc, ne, nf;
    long v;

    gw_init(&gw, "TOKEN123", GW_INTENT_GUILDS | GW_INTENT_GUILD_MESSAGES | GW_INTENT_MESSAGE_CONTENT);
    gw.rng = gw_test_rng;
    jitter(half, sizeof(half));
    CHECK(gw.state == GW_IDLE && gw_next_deadline(&gw, 0) == -1);

    gw_start(&gw, 1000);
    nc = take_cmds(&gw, c, 4);
    CHECK(gw.state == GW_WAIT_CONNECT && nc == 1 && c[0].type == GW_CMD_CONNECT && c[0].delay == 0 &&
          strcmp(c[0].url, GW_DEFAULT_URL) == 0);
    gw_start(&gw, 1000);                                           /* a second start does nothing */
    CHECK(take_cmds(&gw, c, 4) == 0);

    open_ws(&ws);
    gw_connection_opened(&gw, &ws, 1000);
    CHECK(gw.state == GW_WAIT_HELLO && drain(&ws, s, 6) == 0);

    /* HELLO: IDENTIFY goes out at once, the first heartbeat is due after interval * jitter */
    feed(&gw, "{\"op\":10,\"d\":{\"heartbeat_interval\":40000}}", 1005);
    CHECK(gw.state == GW_AUTHENTICATING && gw.hb_interval == 40000U && gw.next_hb == 1005U + 20000U);
    CHECK(gw_next_deadline(&gw, 1005) == 20000);
    nf = drain(&ws, s, 6);
    CHECK(nf == 1 && s[0].opcode == WS_OP_TEXT && sent_op(&s[0]) == 2);
    n = parse_sent(&s[0], &d);
    CHECK(n != NULL && strcmp(json_get_str(json_get(n, "d"), "token", ""), "TOKEN123") == 0);
    CHECK(json_int(json_get(json_get(n, "d"), "intents"), &v) && v == 33281);
    CHECK(strcmp(json_get_str(json_get(json_get(n, "d"), "properties"), "os", ""), "openstep") == 0);
    CHECK(strcmp(json_get_str(json_get(json_get(n, "d"), "properties"), "browser", ""), "Kallisti") == 0);
    json_free(&d);

    /* heartbeats: nothing before the deadline, then null (no sequence yet) */
    gw_tick(&gw, 21004);
    CHECK(drain(&ws, s, 6) == 0);
    gw_tick(&gw, 21005);
    nf = drain(&ws, s, 6);
    CHECK(nf == 1 && sent_op(&s[0]) == 1 && strstr((const char *)s[0].data, "\"d\":null") != NULL);
    CHECK(gw.ack_pending && gw_next_deadline(&gw, 21005) == 40000);
    feed(&gw, "{\"op\":11}", 21050);
    CHECK(!gw.ack_pending && gw.latency_ms == 45);

    /* READY: the session is stored, the resume URL gets its query, two events come out */
    feed(&gw, READY_JSON, 21100);
    CHECK(gw.state == GW_READY && gw.seq == 1 && gw.can_resume && strcmp(gw.session_id, "sess-abc") == 0);
    CHECK(strcmp(gw.resume_url, RESUME_URL) == 0 && strcmp(gw.user_id, "42") == 0);
    ne = take_events(&gw, e, 6);
    CHECK(ne == 2 && e[0].type == GW_EV_READY && strcmp(e[0].name, "Kallisti") == 0 && strcmp(e[0].user, "42") == 0);
    CHECK(e[1].type == GW_EV_DISPATCH && strcmp(e[1].name, "READY") == 0 && e[1].seq == 1);

    /* dispatches pass through with their payloads and sequence numbers */
    feed(&gw, "{\"op\":0,\"s\":2,\"t\":\"GUILD_CREATE\",\"d\":{\"id\":\"777\",\"name\":\"Test\"}}", 21200);
    ne = take_events(&gw, e, 6);
    CHECK(ne == 1 && strcmp(e[0].name, "GUILD_CREATE") == 0 && e[0].seq == 2 && strcmp(e[0].dtext, "777") == 0);
    feed(&gw, "{\"op\":0,\"s\":3,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"9\",\"content\":\"hi \\u2603\"}}", 21300);
    ne = take_events(&gw, e, 6);
    CHECK(ne == 1 && e[0].type == GW_EV_DISPATCH && strcmp(e[0].name, "MESSAGE_CREATE") == 0 && e[0].seq == 3);
    CHECK(strcmp(e[0].dtext, "9") == 0);
    CHECK(gw.seq == 3);

    /* the next beat carries the sequence number */
    gw_tick(&gw, 61005);
    nf = drain(&ws, s, 6);
    CHECK(nf == 1 && sent_op(&s[0]) == 1 && strstr((const char *)s[0].data, "\"d\":3") != NULL);
    feed(&gw, "{\"op\":11}", 61020);

    /* the server may ask for a beat right now */
    feed(&gw, "{\"op\":1,\"d\":null}", 62000);
    nf = drain(&ws, s, 6);
    CHECK(nf == 1 && sent_op(&s[0]) == 1 && strstr((const char *)s[0].data, "\"d\":3") != NULL);
    feed(&gw, "{\"op\":11}", 62010);
    CHECK(gw_next_deadline(&gw, 62010) == 101005L - 62010L);
    ws_free(&ws);
    gw_free(&gw);
}

static void test_resume_paths(void)
{
    Gateway gw;
    WsClient ws, ws2;
    Cmd c[4];
    Ev e[6];
    Sent s[6];
    JDoc d;
    JNode *n;
    int nc, ne, nf;
    long v;

    /* a zombied connection: a heartbeat goes unanswered, the next due beat tears the connection down */
    gw_init(&gw, "TOKEN123", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    bring_up(&gw, &ws, 1000, 40000);                              /* beats due at 21005, 61005, ... */
    feed(&gw, "{\"op\":0,\"s\":2,\"t\":\"GUILD_CREATE\",\"d\":{\"id\":\"1\"}}", 5000);
    take_events(&gw, e, 6);
    gw_tick(&gw, 21005);                                          /* beat sent, never acknowledged */
    drain(&ws, s, 6);
    gw_tick(&gw, 61005);                                          /* due again with the first still unanswered */
    nf = drain(&ws, s, 6);
    CHECK(nf == 1 && s[0].opcode == WS_OP_CLOSE && s[0].data[0] == 0x0f && s[0].data[1] == 0xa0);  /* 4000 */
    nc = take_cmds(&gw, c, 4);
    CHECK(nc == 2 && c[0].type == GW_CMD_CLOSE && c[1].type == GW_CMD_CONNECT && c[1].delay == 0 &&
          strcmp(c[1].url, RESUME_URL) == 0);
    ne = take_events(&gw, e, 6);
    CHECK(ne == 1 && e[0].type == GW_EV_DISCONNECTED && e[0].reconnect == 1);
    CHECK(gw.state == GW_WAIT_CONNECT && gw.can_resume && gw.ws == NULL);
    CHECK(gw_next_deadline(&gw, 61005) == -1);
    gw_tick(&gw, 70000);                                          /* nothing happens while disconnected */
    CHECK(take_cmds(&gw, c, 4) == 0);

    /* reconnect: HELLO is answered with RESUME carrying the session and the last sequence number */
    open_ws(&ws2);
    gw_connection_opened(&gw, &ws2, 61100);
    feed(&gw, "{\"op\":10,\"d\":{\"heartbeat_interval\":41250}}", 61110);
    nf = drain(&ws2, s, 6);
    CHECK(nf == 1 && sent_op(&s[0]) == 6);
    n = parse_sent(&s[0], &d);
    CHECK(n != NULL && strcmp(json_get_str(json_get(n, "d"), "token", ""), "TOKEN123") == 0 &&
          strcmp(json_get_str(json_get(n, "d"), "session_id", ""), "sess-abc") == 0 &&
          json_int(json_get(json_get(n, "d"), "seq"), &v) && v == 2);
    json_free(&d);
    feed(&gw, "{\"op\":0,\"s\":3,\"t\":\"RESUMED\",\"d\":{}}", 61200);
    ne = take_events(&gw, e, 6);
    CHECK(ne == 1 && e[0].type == GW_EV_RESUMED && gw.state == GW_READY && gw.seq == 3);
    ws_free(&ws2);

    /* op 7 RECONNECT: same, immediately, keeping the session */
    ws_free(&ws);
    open_ws(&ws);
    gw_connection_opened(&gw, &ws, 0);                            /* ignored: not waiting for a connect */
    gw_free(&gw);
    gw_init(&gw, "TOKEN123", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    bring_up(&gw, &ws, 1000, 40000);
    feed(&gw, "{\"op\":7,\"d\":null}", 2000);
    nf = drain(&ws, s, 6);
    CHECK(nf == 1 && s[0].opcode == WS_OP_CLOSE);
    nc = take_cmds(&gw, c, 4);
    CHECK(nc == 2 && c[0].type == GW_CMD_CLOSE && c[1].type == GW_CMD_CONNECT && c[1].delay == 0 &&
          strcmp(c[1].url, RESUME_URL) == 0);
    ne = take_events(&gw, e, 6);
    CHECK(ne == 1 && e[0].type == GW_EV_DISCONNECTED && e[0].reconnect && gw.can_resume);
    CHECK(gw.failures == 0);                                      /* an op 7 is not a failure: no backoff */
    gw_free(&gw);
    ws_free(&ws);

    /* op 9 INVALID_SESSION with d=false: the session is dropped, IDENTIFY after 1-5 seconds */
    gw_init(&gw, "TOKEN123", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    bring_up(&gw, &ws, 1000, 40000);
    {
        static const unsigned char seq[] = { 0xff, 0xff };          /* the longest wait: ~5 s */
        jitter(seq, sizeof(seq));
        feed(&gw, "{\"op\":9,\"d\":false}", 3000);
    }
    CHECK(!gw.can_resume && gw.session_id[0] == '\0' && gw.seq == -1 && gw.auth_pending);
    CHECK(gw.auth_at >= 3000U + 1000U && gw.auth_at < 3000U + 5000U);
    gw_tick(&gw, gw.auth_at - 1);
    CHECK(drain(&ws, s, 6) == 0);
    gw_tick(&gw, gw.auth_at);
    nf = drain(&ws, s, 6);
    CHECK(nf == 1 && sent_op(&s[0]) == 2 && !gw.auth_pending && gw.state == GW_AUTHENTICATING);
    gw_free(&gw);
    ws_free(&ws);

    /* op 9 with d=true: the session survives and a RESUME follows the wait */
    gw_init(&gw, "TOKEN123", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    bring_up(&gw, &ws, 1000, 40000);
    {
        static const unsigned char zero[] = { 0x00, 0x00 };         /* the shortest wait: 1 s */
        jitter(zero, sizeof(zero));
        feed(&gw, "{\"op\":9,\"d\":true}", 3000);
    }
    CHECK(gw.can_resume && gw.seq == 1 && gw.auth_at == 4000U);
    gw_tick(&gw, 3999);
    CHECK(drain(&ws, s, 6) == 0);
    gw_tick(&gw, 4000);
    nf = drain(&ws, s, 6);
    CHECK(nf == 1 && sent_op(&s[0]) == 6);
    gw_free(&gw);
    ws_free(&ws);
}

static void test_close_codes(void)
{
    static const struct { int code; int fatal; int invalidates; const char *needle; } table[] = {
        { 4004, 1, 0, "token" },
        { 4010, 1, 0, "shard" },
        { 4011, 1, 0, "shard" },
        { 4012, 1, 0, "version" },
        { 4013, 1, 0, "invalid" },
        { 4014, 1, 0, "Message Content" },
        { 4000, 0, 0, NULL }, { 4001, 0, 0, NULL }, { 4002, 0, 0, NULL }, { 4003, 0, 0, NULL },
        { 4005, 0, 0, NULL }, { 4008, 0, 0, NULL },
        { 4007, 0, 1, NULL }, { 4009, 0, 1, NULL },
        { 1000, 0, 1, NULL }, { 1001, 0, 1, NULL },
        { 1006, 0, 0, NULL }, { 1011, 0, 0, NULL }, { 0, 0, 0, NULL }, { 1012, 0, 0, NULL }
    };
    size_t i;
    int pass;

    for (pass = 0; pass < 2; pass++) {              /* pass 0: the driver reports the loss; pass 1: a close frame arrived */
        for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
            Gateway gw;
            WsClient ws;
            Cmd c[4];
            Ev e[6];
            int nc, ne;
            const char *why;

            gw_init(&gw, "T", GW_INTENT_GUILDS);
            gw.rng = gw_test_rng;
            bring_up(&gw, &ws, 1000, 40000);
            CHECK(gw_close_is_fatal(table[i].code, &why) == table[i].fatal);
            CHECK(gw_close_invalidates_session(table[i].code) == table[i].invalidates);
            if (pass == 0) {
                gw_connection_lost(&gw, table[i].code, 5000);
            } else {
                feed_close(&gw, table[i].code == 0 ? 1005 : table[i].code, 5000);
            }
            nc = take_cmds(&gw, c, 4);
            ne = take_events(&gw, e, 6);
            if (table[i].fatal) {
                CHECK(gw.state == GW_FATAL && ne == 1 && e[0].type == GW_EV_FATAL && e[0].code == table[i].code);
                CHECK(strstr(e[0].text, table[i].needle) != NULL);
                CHECK(nc == (pass == 1 ? 1 : 0));                     /* no reconnect; just the close if a frame arrived */
                if (nc == 1) {
                    CHECK(c[0].type == GW_CMD_CLOSE);
                }
            } else {
                CHECK(gw.state == GW_WAIT_CONNECT && ne == 1 && e[0].type == GW_EV_DISCONNECTED && e[0].reconnect);
                CHECK(nc == (pass == 1 ? 2 : 1) && c[nc - 1].type == GW_CMD_CONNECT);
                CHECK(gw.can_resume == !table[i].invalidates);
                CHECK(strcmp(c[nc - 1].url, table[i].invalidates ? GW_DEFAULT_URL : RESUME_URL) == 0);
                CHECK(c[nc - 1].delay == 0 && gw.failures == 1);
            }
            gw_free(&gw);
            ws_free(&ws);
        }
    }
}

static void test_backoff_and_failures(void)
{
    static const GwMs want[] = { 0, 1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000 };
    Ev tmp[6];
    Gateway gw;
    WsClient ws;
    Cmd c[4];
    size_t i;
    int nc;

    gw_init(&gw, "T", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    gw_start(&gw, 0);
    take_cmds(&gw, c, 4);
    for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        gw_connection_failed(&gw, 0);                              /* TCP/TLS/handshake never worked */
        nc = take_cmds(&gw, c, 4);
        CHECK(nc == 1 && c[0].type == GW_CMD_CONNECT && c[0].delay == want[i]);
        take_events(&gw, tmp, 6);
    }
    /* a good connection resets it */
    {
        Ev e[6];
        Sent s[4];
        static const unsigned char half[] = { 0x80, 0x00 };

        take_events(&gw, e, 6);
        open_ws(&ws);
        jitter(half, sizeof(half));
        gw_connection_opened(&gw, &ws, 100000);
        feed(&gw, "{\"op\":10,\"d\":{\"heartbeat_interval\":40000}}", 100005);
        drain(&ws, s, 4);
        feed(&gw, READY_JSON, 100100);
        CHECK(gw.failures == 0 && gw.state == GW_READY);
        take_events(&gw, e, 6);
        gw_connection_lost(&gw, 1006, 200000);
        nc = take_cmds(&gw, c, 4);
        CHECK(nc == 1 && c[0].delay == 0);
    }
    gw_free(&gw);
    ws_free(&ws);
}

static void test_stop_and_restart(void)
{
    Gateway gw;
    WsClient ws;
    Cmd c[4];
    Ev e[6];
    Sent s[4];
    int nc, nf;

    gw_init(&gw, "T", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    bring_up(&gw, &ws, 1000, 40000);
    gw_stop(&gw, 5000);
    nf = drain(&ws, s, 4);
    CHECK(nf == 1 && s[0].opcode == WS_OP_CLOSE && s[0].data[0] == 0x03 && s[0].data[1] == 0xe8);
    nc = take_cmds(&gw, c, 4);
    CHECK(nc == 1 && c[0].type == GW_CMD_CLOSE && gw.state == GW_IDLE && !gw.can_resume && gw.ws == NULL);
    gw_connection_lost(&gw, 1000, 5100);                          /* the socket then drops: no reconnect */
    CHECK(take_cmds(&gw, c, 4) == 0 && take_events(&gw, e, 6) == 0 && gw.state == GW_IDLE);
    gw_tick(&gw, 99999);
    CHECK(take_cmds(&gw, c, 4) == 0);
    /* stopping while only waiting to connect cancels the pending connect */
    gw_start(&gw, 6000);
    gw_stop(&gw, 6001);
    CHECK(take_cmds(&gw, c, 4) == 0 && gw.state == GW_IDLE);
    ws_free(&ws);
    /* a restart begins from scratch: IDENTIFY, not RESUME */
    gw_start(&gw, 7000);
    nc = take_cmds(&gw, c, 4);
    CHECK(nc == 1 && strcmp(c[0].url, GW_DEFAULT_URL) == 0);
    open_ws(&ws);
    gw_connection_opened(&gw, &ws, 7000);
    feed(&gw, "{\"op\":10,\"d\":{\"heartbeat_interval\":40000}}", 7010);
    nf = drain(&ws, s, 4);
    CHECK(nf == 1 && sent_op(&s[0]) == 2);
    gw_free(&gw);
    ws_free(&ws);

    /* a fatal error stays fatal: no further connection attempts */
    gw_init(&gw, "T", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    bring_up(&gw, &ws, 1000, 40000);
    gw_connection_lost(&gw, 4004, 5000);
    take_cmds(&gw, c, 4);
    take_events(&gw, e, 6);
    gw_connection_lost(&gw, 1006, 5001);
    gw_connection_failed(&gw, 5002);
    CHECK(take_cmds(&gw, c, 4) == 0 && take_events(&gw, e, 6) == 0 && gw.state == GW_FATAL);
    gw_free(&gw);
    ws_free(&ws);
}

static void test_bad_input(void)
{
    static const char *junk[] = {
        "not json", "", "[]", "42", "\"op\"", "{}", "{\"op\":\"x\"}", "{\"op\":null}", "{\"op\":0}",
        "{\"op\":0,\"t\":null,\"s\":1}", "{\"op\":99}", "{\"op\":-3,\"d\":5}", "{\"op\":10}", "{\"op\":10,\"d\":5}",
        "{\"op\":9}", "{\"op\":11}", NULL
    };
    Gateway gw;
    WsClient ws;
    Cmd c[4];
    Ev e[6];
    Sent s[4];
    int i;

    gw_init(&gw, "T", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    bring_up(&gw, &ws, 1000, 40000);
    for (i = 0; junk[i] != NULL; i++) {
        feed(&gw, junk[i], 2000);
        drain(&ws, s, 4);
        take_cmds(&gw, c, 4);
        take_events(&gw, e, 6);
        if (gw.state != GW_READY) {
            /* the HELLO with a nonsense interval is allowed to restart the connection; start over */
            gw_free(&gw);
            gw_init(&gw, "T", GW_INTENT_GUILDS);
            gw.rng = gw_test_rng;
            ws_free(&ws);
            bring_up(&gw, &ws, 1000, 40000);
        }
    }
    CHECK(gw.state == GW_READY);
    /* a nonsense heartbeat interval is rejected rather than trusted */
    gw_free(&gw);
    ws_free(&ws);
    gw_init(&gw, "T", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    gw_start(&gw, 0);
    take_cmds(&gw, c, 4);
    open_ws(&ws);
    gw_connection_opened(&gw, &ws, 0);
    feed(&gw, "{\"op\":10,\"d\":{\"heartbeat_interval\":5}}", 1);
    CHECK(gw.bad_frames == 1 && gw.state == GW_WAIT_CONNECT);
    gw_free(&gw);
    ws_free(&ws);

    /* events for a connection that is gone are ignored */
    gw_init(&gw, "T", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    feed(&gw, READY_JSON, 5);
    CHECK(gw.state == GW_IDLE && take_events(&gw, e, 6) == 0);
    gw_free(&gw);

    /* binary frames, pings and pongs are not the gateway's business */
    gw_init(&gw, "T", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    bring_up(&gw, &ws, 1000, 40000);
    {
        WsEvent ev;
        static const unsigned char x[1] = { 1 };
        int t;

        for (t = WS_EV_BINARY; t <= WS_EV_PONG; t++) {
            memset(&ev, 0, sizeof(ev));
            ev.type = t;
            ev.data = x;
            ev.len = 1;
            gw_handle_ws_event(&gw, &ev, 3000);
        }
        CHECK(gw.state == GW_READY && take_events(&gw, e, 6) == 0 && take_cmds(&gw, c, 4) == 0);
    }
    /* a WebSocket protocol error drops the connection and reconnects */
    {
        WsEvent ev;

        memset(&ev, 0, sizeof(ev));
        ev.type = WS_EV_ERROR;
        ev.code = 1002;
        ev.text = "bad";
        gw_handle_ws_event(&gw, &ev, 3000);
        CHECK(gw.state == GW_WAIT_CONNECT && take_cmds(&gw, c, 4) == 2 && c[0].type == GW_CMD_CLOSE);
    }
    gw_free(&gw);
    ws_free(&ws);
}

static void test_clock_wraparound(void)
{
    static const unsigned char half[] = { 0x80, 0x00 };
    Gateway gw;
    WsClient ws;
    Cmd c[4];
    Sent s[4];
    GwMs start = 0xFFFFFF00U;                                     /* 256 ms before the 32-bit clock wraps */

    gw_init(&gw, "T", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    jitter(half, sizeof(half));
    gw_start(&gw, start);
    take_cmds(&gw, c, 4);
    open_ws(&ws);
    gw_connection_opened(&gw, &ws, start);
    feed(&gw, "{\"op\":10,\"d\":{\"heartbeat_interval\":1000}}", start);
    drain(&ws, s, 4);
    CHECK(gw.next_hb == start + 500U && gw.next_hb < 1000U && gw_next_deadline(&gw, start) == 500);   /* wrapped */
    gw_tick(&gw, start + 499U);
    CHECK(drain(&ws, s, 4) == 0);
    gw_tick(&gw, start + 500U);
    CHECK(drain(&ws, s, 4) == 1);
    CHECK(gw_next_deadline(&gw, start + 500U) == 1000);
    gw_free(&gw);
    ws_free(&ws);
}

/* Random frames and random ops in a random order: the gateway must never crash, and its queues stay bounded. */
static unsigned long lcg_state = 424242UL;

static unsigned long lcg(void)
{
    lcg_state = lcg_state * 1103515245UL + 12345UL;
    return (lcg_state >> 16) & 0x7fffUL;
}

static void test_fuzz(void)
{
    const char *frames[11];
    Gateway gw;
    WsClient ws;
    GwCommand cmd;
    GwEvent ev;
    GwMs now = 0;
    int iter, k;
    char buf[300];

    frames[0] = "{\"op\":10,\"d\":{\"heartbeat_interval\":41250}}";
    frames[1] = "{\"op\":11}";
    frames[2] = "{\"op\":1}";
    frames[3] = "{\"op\":7}";
    frames[4] = "{\"op\":9,\"d\":true}";
    frames[5] = "{\"op\":9,\"d\":false}";
    frames[6] = READY_JSON;
    frames[7] = "{\"op\":0,\"s\":5,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"1\",\"content\":\"x\"}}";
    frames[8] = "{\"op\":0,\"s\":6,\"t\":\"RESUMED\",\"d\":{}}";
    frames[9] = "garbage";
    frames[10] = "{\"op\":0}";
    gw_init(&gw, "T", GW_INTENT_GUILDS);
    gw.rng = gw_test_rng;
    gw_start(&gw, now);
    open_ws(&ws);
    for (iter = 0; iter < 30000; iter++) {
        now += lcg() % 3000;
        switch (lcg() % 8) {
        case 0:
            gw_tick(&gw, now);
            break;
        case 1:
            gw_connection_failed(&gw, now);
            break;
        case 2:
            gw_connection_lost(&gw, (int)(lcg() % 5000), now);
            break;
        case 3:
            ws_free(&ws);
            open_ws(&ws);
            gw_connection_opened(&gw, &ws, now);
            break;
        case 4:
            if (lcg() % 50 == 0) {
                gw_stop(&gw, now);
            } else if (lcg() % 20 == 0) {
                gw_start(&gw, now);
            }
            break;
        default:
            strcpy(buf, frames[lcg() % (sizeof(frames) / sizeof(frames[0]))]);
            if (lcg() % 4 == 0) {
                buf[lcg() % strlen(buf)] = (char)(lcg() & 0x7f);
            }
            feed(&gw, buf, now);
            break;
        }
        for (k = 0; k < 8; k++) {
            ws_output_done(&ws, ws.out_len);                       /* never let the frame queue grow */
        }
        while (gw_next_command(&gw, &cmd)) {
            ;
        }
        while (gw_next_event(&gw, &ev)) {
            ;
        }
        if (gw.ws != &ws && gw.ws != NULL) {
            gw.ws = NULL;
        }
    }
    CHECK(gw.nevs <= 6 && gw.ncmds <= 4);
    gw_free(&gw);
    ws_free(&ws);
}

int main(void)
{
    test_lifecycle();
    test_resume_paths();
    test_close_codes();
    test_backoff_and_failures();
    test_stop_and_restart();
    test_bad_input();
    test_clock_wraparound();
    test_fuzz();
    TEST_DONE("gateway");
}
