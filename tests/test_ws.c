#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/ws.h"
#include "../core/base64.h"
#include "test.h"

/* ---- a deterministic stand-in for the RNG: hands out a scripted byte sequence, then a counter ---- */
static const unsigned char *script;
static size_t script_len, script_pos;
static unsigned char counter;

static int test_rng(void *out, size_t len)
{
    size_t i;
    unsigned char *o = (unsigned char *)out;

    for (i = 0; i < len; i++) {
        o[i] = script_pos < script_len ? script[script_pos++] : counter++;
    }
    return 0;
}

static void set_script(const unsigned char *s, size_t n)
{
    script = s;
    script_len = n;
    script_pos = 0;
    counter = 1;
}

static void open_client(WsClient *ws)
{
    ws_init(ws, "wss://gateway.example/?v=10&encoding=json");
    ws->rng = test_rng;
    ws->state = WS_OPEN;
}

/* ---- server-side frame building (unmasked, as a server sends) ---- */
static size_t srv_frame(unsigned char *out, int opcode, int fin, const unsigned char *payload, size_t len)
{
    size_t n = 0;

    out[n++] = (unsigned char)((fin ? 0x80 : 0) | opcode);
    if (len < 126) {
        out[n++] = (unsigned char)len;
    } else if (len <= 0xFFFF) {
        out[n++] = 126;
        out[n++] = (unsigned char)(len >> 8);
        out[n++] = (unsigned char)len;
    } else {
        out[n++] = 127;
        out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
        out[n++] = (unsigned char)(len >> 24);
        out[n++] = (unsigned char)(len >> 16);
        out[n++] = (unsigned char)(len >> 8);
        out[n++] = (unsigned char)len;
    }
    if (len) {
        memcpy(out + n, payload, len);
    }
    return n + len;
}

/* ---- summarise the events produced from an input stream as a string ---- */
static void summarise(WsClient *ws, char *out, size_t cap)
{
    WsEvent ev;
    char piece[400];
    size_t used = 0;

    out[0] = '\0';
    while (ws_next_event(ws, &ev)) {
        switch (ev.type) {
        case WS_EV_TEXT:
            sprintf(piece, "T<%.200s>", ev.data);
            break;
        case WS_EV_BINARY:
            sprintf(piece, "B<%lu>", (unsigned long)ev.len);
            break;
        case WS_EV_PING:
            sprintf(piece, "P<%.*s>", (int)ev.len, (const char *)ev.data);
            break;
        case WS_EV_PONG:
            sprintf(piece, "O<%.*s>", (int)ev.len, (const char *)ev.data);
            break;
        case WS_EV_CLOSE:
            sprintf(piece, "C<%d:%s>", ev.code, ev.text);
            break;
        default:
            sprintf(piece, "E<%d>", ev.code);
            break;
        }
        if (used + strlen(piece) + 1 < cap) {
            strcpy(out + used, piece);
            used += strlen(piece);
        }
    }
}

/* Feeds the stream cut into `step`-byte pieces, summarising after each; the combined summary must be the same
 * for every step. */
static void check_stream(const unsigned char *data, size_t len, const char *want, const char *what)
{
    size_t step, pos;
    int bad = 0;

    for (step = 1; step <= len + 1; step++) {
        WsClient ws;
        char got[2048], part[1024];

        open_client(&ws);
        got[0] = '\0';
        for (pos = 0; pos < len; pos += step) {
            size_t n = len - pos < step ? len - pos : step;

            ws_input(&ws, data + pos, n);
            summarise(&ws, part, sizeof(part));
            strcat(got, part);
        }
        if (strcmp(got, want) != 0) {
            if (!bad) {
                printf("  split failure (%s): step=%lu got \"%s\" want \"%s\"\n", what, (unsigned long)step, got, want);
            }
            bad = 1;
        }
        ws_free(&ws);
    }
    CHECK(!bad);
}

/* ---- client-frame decoding (what a server would do), to check what we send ---- */
typedef struct { int opcode, fin; size_t len; unsigned char data[70000]; } ClientFrame;

static int decode_client(const unsigned char *p, size_t avail, ClientFrame *f, size_t *used)
{
    size_t hl = 2, plen, i;
    unsigned char mask[4];

    if (avail < 2) {
        return -1;
    }
    f->fin = (p[0] & 0x80) != 0;
    f->opcode = p[0] & 15;
    if (!(p[1] & 0x80)) {
        return -2;                 /* client frames must be masked */
    }
    plen = p[1] & 0x7f;
    if (plen == 126) {
        plen = ((size_t)p[2] << 8) | p[3];
        hl = 4;
    } else if (plen == 127) {
        plen = ((size_t)p[6] << 24) | ((size_t)p[7] << 16) | ((size_t)p[8] << 8) | p[9];
        hl = 10;
    }
    if (avail < hl + 4 + plen) {
        return -1;
    }
    memcpy(mask, p + hl, 4);
    for (i = 0; i < plen; i++) {
        f->data[i] = (unsigned char)(p[hl + 4 + i] ^ mask[i & 3]);
    }
    f->len = plen;
    *used = hl + 4 + plen;
    return 0;
}

static void test_handshake(void)
{
    /* RFC 6455 section 1.3: this nonce gives exactly this key and accept value */
    static const unsigned char nonce[] = "the sample nonce";
    WsClient ws;
    HttpReq req;
    HttpResp resp;
    size_t used;
    const char *good =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    const char *tail;

    CHECK(ws_init(&ws, "wss://gateway.discord.gg/?v=10&encoding=json") == 0);
    CHECK(strcmp(ws.host, "gateway.discord.gg") == 0 && ws.port == 443 && ws.tls &&
          strcmp(ws.path, "/?v=10&encoding=json") == 0);
    ws.rng = test_rng;
    set_script(nonce, 16);
    http_req_init(&req);
    CHECK(ws_handshake_begin(&ws, &req) == 0);
    CHECK(http_req_header(&req, "User-Agent", "Kallisti") == 0 && http_req_end(&req, NULL, 0) == 0);
    CHECK(strcmp(ws.key, "dGhlIHNhbXBsZSBub25jZQ==") == 0);
    CHECK(strcmp(ws.accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
    CHECK(strcmp(req.buf, "GET /?v=10&encoding=json HTTP/1.1\r\nHost: gateway.discord.gg\r\nAccept-Encoding: identity\r\n"
                          "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                          "Sec-WebSocket-Version: 13\r\nUser-Agent: Kallisti\r\n\r\n") == 0);
    http_req_free(&req);

    /* the reply plus the first frame in one buffer: the HTTP parser stops at the headers */
    {
        char both[300];
        unsigned char f[8];
        size_t fl = srv_frame(f, WS_OP_TEXT, 1, (const unsigned char *)"hi", 2);

        strcpy(both, good);
        memcpy(both + strlen(good), f, fl);
        http_resp_init(&resp, 1000, 0);
        CHECK(http_resp_feed(&resp, (const unsigned char *)both, strlen(good) + fl, &used) == HTTP_DONE);
        CHECK(used == strlen(good));
        CHECK(ws_handshake_check(&ws, &resp) == 0 && ws.state == WS_OPEN);
        ws_input(&ws, (const unsigned char *)both + used, fl);
        {
            char s[64];
            summarise(&ws, s, sizeof(s));
            CHECK(strcmp(s, "T<hi>") == 0);
        }
        http_resp_free(&resp);
    }
    ws_free(&ws);

    /* every way the check can fail */
#define BAD(text, why) do { WsClient w2; HttpResp r2; size_t u2; \
        ws_init(&w2, "ws://h/"); w2.rng = test_rng; set_script(nonce, 16); \
        { HttpReq q2; http_req_init(&q2); ws_handshake_begin(&w2, &q2); http_req_free(&q2); } \
        http_resp_init(&r2, 1000, 0); \
        http_resp_feed(&r2, (const unsigned char *)text, strlen(text), &u2); \
        CHECK(ws_handshake_check(&w2, &r2) == -1 && w2.state == WS_CONNECTING && w2.err[0] != '\0'); \
        (void)why; http_resp_free(&r2); ws_free(&w2); } while (0)
    BAD("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", "status");
    BAD("HTTP/1.1 101 x\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n", "no upgrade");
    BAD("HTTP/1.1 101 x\r\nUpgrade: h2c\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n", "bad upgrade");
    BAD("HTTP/1.1 101 x\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n", "no connection");
    BAD("HTTP/1.1 101 x\r\nUpgrade: websocket\r\nConnection: keep-alive\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n", "bad connection");
    BAD("HTTP/1.1 101 x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n", "no accept");
    BAD("HTTP/1.1 101 x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n", "wrong accept");
    BAD("HTTP/1.1 101 x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n", "extensions");
    BAD("HTTP/1.1 101 x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "Sec-WebSocket-Protocol: chat\r\n\r\n", "subprotocol");
#undef BAD
    /* header names and the Connection list are case-insensitive */
    {
        WsClient w2;
        HttpResp r2;
        size_t u2;
        const char *t = "HTTP/1.1 101 x\r\nupgrade: WebSocket\r\nconnection: keep-alive, UPGRADE\r\n"
                        "sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
        HttpReq q;

        ws_init(&w2, "ws://h/");
        w2.rng = test_rng;
        set_script(nonce, 16);
        http_req_init(&q);
        ws_handshake_begin(&w2, &q);
        http_req_free(&q);
        http_resp_init(&r2, 1000, 0);
        http_resp_feed(&r2, (const unsigned char *)t, strlen(t), &u2);
        CHECK(ws_handshake_check(&w2, &r2) == 0);
        http_resp_free(&r2);
        ws_free(&w2);
    }
    (void)tail;

    /* only ws:// and wss:// URLs */
    CHECK(ws_init(&ws, "https://h/") == -1);
    CHECK(ws_init(&ws, "ws://h:9000/x") == 0 && ws.port == 9000 && !ws.tls);
    ws_free(&ws);
}

static void test_incoming_frames(void)
{
    unsigned char buf[70100], big[70000];
    size_t n, i;

    /* RFC 6455 section 5.7: unmasked "Hello", fragmented "Hel"+"lo", ping, pong */
    check_stream((const unsigned char *)"\x81\x05Hello", 7, "T<Hello>", "single text");
    check_stream((const unsigned char *)"\x01\x03Hel\x80\x02lo", 9, "T<Hello>", "fragmented");
    check_stream((const unsigned char *)"\x89\x05Hello", 7, "P<Hello>", "ping");
    check_stream((const unsigned char *)"\x8a\x05Hello", 7, "O<Hello>", "pong");
    /* a ping in the middle of a fragmented message */
    check_stream((const unsigned char *)"\x01\x03Hel\x89\x02hi\x80\x02lo", 13, "P<hi>T<Hello>", "ping between fragments");
    /* three fragments, an empty middle one, and two messages back to back */
    check_stream((const unsigned char *)"\x01\x01" "a" "\x00\x00" "\x80\x01" "b" "\x81\x02xy", 12, "T<ab>T<xy>", "empty fragment");
    /* an empty text message and an empty binary message */
    check_stream((const unsigned char *)"\x81\x00\x82\x00", 4, "T<>B<0>", "empty messages");
    /* a close with a code and a reason, and an empty close */
    {
        unsigned char c[40];

        n = srv_frame(c, WS_OP_CLOSE, 1, (const unsigned char *)"\x03\xe8" "going away", 12);
        check_stream(c, n, "C<1000:going away>", "close with reason");
        check_stream((const unsigned char *)"\x88\x00", 2, "C<1005:>", "empty close");
    }
    /* 16-bit and 64-bit lengths */
    for (i = 0; i < sizeof(big); i++) {
        big[i] = (unsigned char)(i * 5 + 1);
    }
    n = srv_frame(buf, WS_OP_BINARY, 1, big, 256);
    CHECK(buf[1] == 126);
    check_stream(buf, n, "B<256>", "256 bytes");
    {
        WsClient ws;
        WsEvent ev;

        n = srv_frame(buf, WS_OP_BINARY, 1, big, 65536);
        CHECK(buf[1] == 127);
        open_client(&ws);
        ws_input(&ws, buf, n);
        CHECK(ws_next_event(&ws, &ev) && ev.type == WS_EV_BINARY && ev.len == 65536 && memcmp(ev.data, big, 65536) == 0);
        ws_free(&ws);
    }
    /* a text message with multi-byte characters survives the split test as well: U+2603 and U+1F600 */
    check_stream((const unsigned char *)"\x81\x09" "a\xE2\x98\x83" "b\xF0\x9F\x98\x80", 11,
                 "T<a\xE2\x98\x83" "b\xF0\x9F\x98\x80>", "multi-byte text");
    /* a message that spans frames is delivered once even when the pieces arrive as one buffer */
    {
        WsClient ws;
        char s[64];

        open_client(&ws);
        ws_input(&ws, (const unsigned char *)"\x01\x02" "ab" "\x00\x02" "cd" "\x80\x02" "ef", 12);
        summarise(&ws, s, sizeof(s));
        CHECK(strcmp(s, "T<abcdef>") == 0);
        ws_free(&ws);
    }
}

static void expect_error(const unsigned char *data, size_t len, int want_code, const char *what)
{
    WsClient ws;
    WsEvent ev;
    int got = 0;
    ClientFrame f;
    size_t used;
    const unsigned char *out;
    size_t olen;

    open_client(&ws);
    ws_input(&ws, data, len);
    while (ws_next_event(&ws, &ev)) {
        if (ev.type == WS_EV_ERROR) {
            got = ev.code;
            break;
        }
    }
    if (got != want_code) {
        printf("  expect_error(%s): got %d want %d\n", what, got, want_code);
    }
    CHECK(got == want_code);
    CHECK(ws_is_closed(&ws));
    /* a close frame with that code was queued, and nothing more is accepted */
    out = ws_output(&ws, &olen);
    CHECK(decode_client(out, olen, &f, &used) == 0 && f.opcode == WS_OP_CLOSE && f.len == 2 &&
          f.data[0] == (unsigned char)(want_code >> 8) && f.data[1] == (unsigned char)want_code);
    CHECK(ws_input(&ws, (const unsigned char *)"\x81\x00", 2) == -1);
    CHECK(ws_next_event(&ws, &ev) == 0);
    CHECK(ws_send_text(&ws, "x", 1) == -1);
    ws_free(&ws);
}

static void test_protocol_errors(void)
{
    static unsigned char big[300];
    unsigned char c[300];
    size_t n;

    expect_error((const unsigned char *)"\xC1\x00", 2, 1002, "rsv1");
    expect_error((const unsigned char *)"\xA1\x00", 2, 1002, "rsv2");
    expect_error((const unsigned char *)"\x91\x00", 2, 1002, "rsv3");
    expect_error((const unsigned char *)"\x81\x85" "abcd" "Hello", 11, 1002, "masked server frame");
    expect_error((const unsigned char *)"\x83\x00", 2, 1002, "reserved data opcode 3");
    expect_error((const unsigned char *)"\x87\x00", 2, 1002, "reserved data opcode 7");
    expect_error((const unsigned char *)"\x8b\x00", 2, 1002, "reserved control opcode 11");
    expect_error((const unsigned char *)"\x8f\x00", 2, 1002, "reserved control opcode 15");
    expect_error((const unsigned char *)"\x09\x00", 2, 1002, "fragmented ping");
    expect_error((const unsigned char *)"\x08\x00", 2, 1002, "fragmented close");
    memset(big, 'a', sizeof(big));
    n = srv_frame(c, WS_OP_PING, 1, big, 126);
    expect_error(c, n, 1002, "ping over 125 bytes");
    expect_error((const unsigned char *)"\x80\x00", 2, 1002, "continuation without start");
    expect_error((const unsigned char *)"\x01\x01" "a" "\x81\x01" "b", 6, 1002, "new text inside a fragmented message");
    expect_error((const unsigned char *)"\x01\x01" "a" "\x82\x01" "b", 6, 1002, "new binary inside a fragmented message");
    /* UTF-8 */
    expect_error((const unsigned char *)"\x81\x02\xC3\x28", 4, 1007, "bad continuation byte");
    expect_error((const unsigned char *)"\x81\x02\xC0\x80", 4, 1007, "overlong two-byte");
    expect_error((const unsigned char *)"\x81\x03\xE0\x80\x80", 5, 1007, "overlong three-byte");
    expect_error((const unsigned char *)"\x81\x03\xED\xA0\x80", 5, 1007, "surrogate half");
    expect_error((const unsigned char *)"\x81\x04\xF4\x90\x80\x80", 6, 1007, "past U+10FFFF");
    expect_error((const unsigned char *)"\x81\x01\xFF", 3, 1007, "0xFF");
    expect_error((const unsigned char *)"\x81\x02\xE2\x98", 4, 1007, "truncated sequence");
    expect_error((const unsigned char *)"\x01\x01\xE2\x80\x02\x98" "A", 7, 1007, "sequence split across fragments, then broken");
    /* close frames */
    expect_error((const unsigned char *)"\x88\x01\x03", 3, 1002, "one-byte close");
    expect_error((const unsigned char *)"\x88\x02\x03\xe7", 4, 1002, "close code 999");
    expect_error((const unsigned char *)"\x88\x02\x03\xed", 4, 1002, "close code 1005 on the wire");
    expect_error((const unsigned char *)"\x88\x02\x03\xee", 4, 1002, "close code 1006 on the wire");
    expect_error((const unsigned char *)"\x88\x02\x03\xf7", 4, 1002, "close code 1015 on the wire");
    expect_error((const unsigned char *)"\x88\x02\x0b\xb7", 4, 1002, "close code 2999");
    expect_error((const unsigned char *)"\x88\x02\x13\x88", 4, 1002, "close code 5000");
    expect_error((const unsigned char *)"\x88\x04\x03\xe8\xC3\x28", 6, 1007, "bad UTF-8 in close reason");
    /* sizes */
    expect_error((const unsigned char *)"\x82\x7f\x00\x00\x00\x01\x00\x00\x00\x00", 10, 1009, "length with high bytes set");
    expect_error((const unsigned char *)"\x82\x7f\x00\x00\x00\x00\xff\xff\xff\xff", 10, 1009, "4 GB frame");
    {
        WsClient ws;
        WsEvent ev;
        unsigned char frame[200];

        /* the limit is on the reassembled message, not only on single frames */
        open_client(&ws);
        ws.max_message = 100;
        n = srv_frame(frame, WS_OP_BINARY, 0, big, 60);
        ws_input(&ws, frame, n);
        CHECK(ws_next_event(&ws, &ev) == 0);
        n = srv_frame(frame, WS_OP_CONT, 1, big, 60);
        ws_input(&ws, frame, n);
        CHECK(ws_next_event(&ws, &ev) == 1 && ev.type == WS_EV_ERROR && ev.code == 1009);
        ws_free(&ws);
        /* exactly at the limit is fine */
        open_client(&ws);
        ws.max_message = 100;
        n = srv_frame(frame, WS_OP_BINARY, 1, big, 100);
        ws_input(&ws, frame, n);
        CHECK(ws_next_event(&ws, &ev) == 1 && ev.type == WS_EV_BINARY && ev.len == 100);
        ws_free(&ws);
    }
    /* valid UTF-8 at the edges is accepted: U+0080, U+07FF, U+0800, U+FFFF, U+10000, U+10FFFF */
    check_stream((const unsigned char *)"\x81\x15\xC2\x80\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF" "ok", 23,
                 "T<\xC2\x80\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF" "ok>", "utf8 edges");
}

static void test_sending(void)
{
    /* RFC 6455 section 5.7: "Hello" masked with 37 fa 21 3d is 81 85 37 fa 21 3d 7f 9f 4d 51 58 */
    static const unsigned char m1[] = { 0x37, 0xfa, 0x21, 0x3d };
    static const unsigned char want[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
    WsClient ws;
    const unsigned char *out;
    size_t olen, used;
    ClientFrame *f = (ClientFrame *)malloc(sizeof(ClientFrame));
    static unsigned char blob[70000];
    size_t i;

    open_client(&ws);
    set_script(m1, 4);
    CHECK(ws_send_text(&ws, "Hello", 5) == 0);
    out = ws_output(&ws, &olen);
    CHECK(olen == sizeof(want) && memcmp(out, want, olen) == 0);
    ws_output_done(&ws, 3);
    out = ws_output(&ws, &olen);
    CHECK(olen == sizeof(want) - 3 && memcmp(out, want + 3, olen) == 0);
    ws_output_done(&ws, olen);
    out = ws_output(&ws, &olen);
    CHECK(olen == 0);

    /* every length form decodes back, with a fresh mask each time */
    for (i = 0; i < sizeof(blob); i++) {
        blob[i] = (unsigned char)(i ^ (i >> 8));
    }
    {
        static const size_t sizes[] = { 0, 1, 125, 126, 127, 65535, 65536, 69999 };
        size_t k;

        for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            set_script(NULL, 0);
            CHECK(ws_send_binary(&ws, blob, sizes[k]) == 0);
            out = ws_output(&ws, &olen);
            CHECK(decode_client(out, olen, f, &used) == 0 && used == olen && f->opcode == WS_OP_BINARY &&
                  f->fin && f->len == sizes[k] && memcmp(f->data, blob, sizes[k]) == 0);
            CHECK(sizes[k] < 126 ? out[1] == (0x80 | sizes[k]) : sizes[k] < 65536 ? out[1] == (0x80 | 126) : out[1] == (0x80 | 127));
            ws_output_done(&ws, olen);
        }
    }
    /* masks differ from frame to frame (counter-driven here) */
    {
        unsigned char a[4], b[4];

        set_script(NULL, 0);
        ws_send_text(&ws, "x", 1);
        out = ws_output(&ws, &olen);
        memcpy(a, out + 2, 4);
        ws_output_done(&ws, olen);
        ws_send_text(&ws, "x", 1);
        out = ws_output(&ws, &olen);
        memcpy(b, out + 2, 4);
        ws_output_done(&ws, olen);
        CHECK(memcmp(a, b, 4) != 0);
    }
    /* several queued frames come out back to back, in order */
    ws_send_text(&ws, "one", 3);
    ws_send_ping(&ws, (const unsigned char *)"p", 1);
    ws_send_text(&ws, "two", 3);
    out = ws_output(&ws, &olen);
    {
        size_t pos = 0;

        CHECK(decode_client(out + pos, olen - pos, f, &used) == 0 && f->opcode == WS_OP_TEXT && f->len == 3 && memcmp(f->data, "one", 3) == 0);
        pos += used;
        CHECK(decode_client(out + pos, olen - pos, f, &used) == 0 && f->opcode == WS_OP_PING && f->len == 1);
        pos += used;
        CHECK(decode_client(out + pos, olen - pos, f, &used) == 0 && f->opcode == WS_OP_TEXT && memcmp(f->data, "two", 3) == 0);
        pos += used;
        CHECK(pos == olen);
    }
    ws_output_done(&ws, olen);

    /* the checks on what may be sent */
    CHECK(ws_send_text(&ws, "\xC3\x28", 2) == -1);                      /* invalid UTF-8 */
    CHECK(ws_send_ping(&ws, blob, 126) == -1);                           /* control frame too long */
    CHECK(ws_send_ping(&ws, blob, 125) == 0);
    ws_output_done(&ws, ws.out_len);
    CHECK(ws_send_close(&ws, 999, NULL) == -1);                          /* not a sendable code */
    CHECK(ws_send_close(&ws, 1005, NULL) == -1);
    CHECK(ws_send_close(&ws, 0, "reason without code") == -1);
    CHECK(ws_send_close(&ws, 1000, "0123456789012345678901234567890123456789012345678901234567890123456789"
                                   "01234567890123456789012345678901234567890123456789012345678901234567890123456789") == -1);
    ws_free(&ws);
    free(f);

    /* not open yet: nothing can be sent */
    ws_init(&ws, "ws://h/");
    CHECK(ws_send_text(&ws, "x", 1) == -1 && ws_send_binary(&ws, blob, 1) == -1 && ws_send_ping(&ws, blob, 1) == -1);
    ws_free(&ws);
}

static void test_close_handshake(void)
{
    WsClient ws;
    WsEvent ev;
    const unsigned char *out;
    size_t olen, used;
    ClientFrame *f = (ClientFrame *)malloc(sizeof(ClientFrame));
    unsigned char c[40];
    size_t n;

    /* we close first: the frame goes out, sending stops, the peer's reply completes the handshake */
    open_client(&ws);
    CHECK(ws_send_close(&ws, 1000, "bye") == 0 && ws.state == WS_CLOSING && !ws_is_closed(&ws));
    out = ws_output(&ws, &olen);
    CHECK(decode_client(out, olen, f, &used) == 0 && f->opcode == WS_OP_CLOSE && f->len == 5 &&
          f->data[0] == 0x03 && f->data[1] == 0xe8 && memcmp(f->data + 2, "bye", 3) == 0);
    ws_output_done(&ws, olen);
    CHECK(ws_send_text(&ws, "x", 1) == -1 && ws_send_close(&ws, 1000, NULL) == -1);
    /* data still arriving before the peer's close is delivered */
    n = srv_frame(c, WS_OP_TEXT, 1, (const unsigned char *)"late", 4);
    ws_input(&ws, c, n);
    CHECK(ws_next_event(&ws, &ev) && ev.type == WS_EV_TEXT);
    n = srv_frame(c, WS_OP_CLOSE, 1, (const unsigned char *)"\x03\xe8", 2);
    ws_input(&ws, c, n);
    CHECK(ws_next_event(&ws, &ev) && ev.type == WS_EV_CLOSE && ev.code == 1000 && ws_is_closed(&ws));
    out = ws_output(&ws, &olen);
    CHECK(olen == 0);                                   /* already sent ours: no second close */
    ws_free(&ws);

    /* the peer closes first: we echo the code and the session ends */
    open_client(&ws);
    n = srv_frame(c, WS_OP_CLOSE, 1, (const unsigned char *)"\x0f\xa3" "kicked", 8);   /* 4003 */
    ws_input(&ws, c, n);
    CHECK(ws_next_event(&ws, &ev) && ev.type == WS_EV_CLOSE && ev.code == 4003 && strcmp(ev.text, "kicked") == 0);
    CHECK(ws_is_closed(&ws));
    out = ws_output(&ws, &olen);
    CHECK(decode_client(out, olen, f, &used) == 0 && f->opcode == WS_OP_CLOSE && f->len == 2 &&
          f->data[0] == 0x0f && f->data[1] == 0xa3);
    CHECK(ws_next_event(&ws, &ev) == 0 && ws_send_text(&ws, "x", 1) == -1);
    ws_free(&ws);

    /* an empty close from the peer is answered with an empty close */
    open_client(&ws);
    ws_input(&ws, (const unsigned char *)"\x88\x00", 2);
    CHECK(ws_next_event(&ws, &ev) && ev.type == WS_EV_CLOSE && ev.code == 1005);
    out = ws_output(&ws, &olen);
    CHECK(decode_client(out, olen, f, &used) == 0 && f->opcode == WS_OP_CLOSE && f->len == 0);
    ws_free(&ws);

    /* an empty close of our own */
    open_client(&ws);
    CHECK(ws_send_close(&ws, 0, NULL) == 0);
    out = ws_output(&ws, &olen);
    CHECK(decode_client(out, olen, f, &used) == 0 && f->opcode == WS_OP_CLOSE && f->len == 0);
    ws_free(&ws);

    /* a ping is answered automatically with the same payload, unless we have already closed */
    open_client(&ws);
    ws_input(&ws, (const unsigned char *)"\x89\x04" "beat", 6);
    CHECK(ws_next_event(&ws, &ev) && ev.type == WS_EV_PING && ev.len == 4);
    out = ws_output(&ws, &olen);
    CHECK(decode_client(out, olen, f, &used) == 0 && f->opcode == WS_OP_PONG && f->len == 4 && memcmp(f->data, "beat", 4) == 0);
    ws_output_done(&ws, olen);
    ws_send_close(&ws, 1000, NULL);
    ws_output_done(&ws, ws.out_len);
    ws_input(&ws, (const unsigned char *)"\x89\x01x", 3);
    CHECK(ws_next_event(&ws, &ev) && ev.type == WS_EV_PING);
    out = ws_output(&ws, &olen);
    CHECK(olen == 0);
    ws_free(&ws);
    free(f);
}

static void test_base64(void)
{
    char out[64];
    unsigned char dec[64];
    size_t n;

    CHECK(base64_encode((const unsigned char *)"", 0, out) == 0 && out[0] == '\0');
    CHECK(base64_encode((const unsigned char *)"f", 1, out) == 4 && strcmp(out, "Zg==") == 0);
    CHECK(base64_encode((const unsigned char *)"fo", 2, out) == 4 && strcmp(out, "Zm8=") == 0);
    CHECK(base64_encode((const unsigned char *)"foo", 3, out) == 4 && strcmp(out, "Zm9v") == 0);
    CHECK(base64_encode((const unsigned char *)"foob", 4, out) == 8 && strcmp(out, "Zm9vYg==") == 0);
    CHECK(base64_encode((const unsigned char *)"fooba", 5, out) == 8 && strcmp(out, "Zm9vYmE=") == 0);
    CHECK(base64_encode((const unsigned char *)"foobar", 6, out) == 8 && strcmp(out, "Zm9vYmFy") == 0);
    CHECK(base64_encode((const unsigned char *)"\xfb\xff\xbf", 3, out) == 4 && strcmp(out, "+/+/") == 0);
    CHECK(base64_decode("Zm9vYmFy", dec, &n) == 0 && n == 6 && memcmp(dec, "foobar", 6) == 0);
    CHECK(base64_decode("Zm9vYg==", dec, &n) == 0 && n == 4 && memcmp(dec, "foob", 4) == 0);
    CHECK(base64_decode("Zm9vYmE=", dec, &n) == 0 && n == 5 && memcmp(dec, "fooba", 5) == 0);
    CHECK(base64_decode("", dec, &n) == 0 && n == 0);
    CHECK(base64_decode("+/+/", dec, &n) == 0 && n == 3 && dec[0] == 0xfb && dec[1] == 0xff && dec[2] == 0xbf);
    CHECK(base64_decode("Zm9", dec, &n) == -1);               /* length not a multiple of 4 */
    CHECK(base64_decode("Zm9v=", dec, &n) == -1);
    CHECK(base64_decode("Zg=A", dec, &n) == -1);
    CHECK(base64_decode("Z===", dec, &n) == -1);
    CHECK(base64_decode("Zm9vYg==Zm9v", dec, &n) == -1);      /* padding in the middle */
    CHECK(base64_decode("Zh==", dec, &n) == -1);              /* nonzero unused bits */
    CHECK(base64_decode("Zm9=", dec, &n) == -1);
    CHECK(base64_decode("Zm 9", dec, &n) == -1 && base64_decode("Zm9\n", dec, &n) == -1);
    CHECK(base64_decode("Zm-_", dec, &n) == -1);              /* the URL-safe alphabet is not accepted */
    /* round trip for every length */
    {
        unsigned char src[40];
        char enc[64];
        size_t len, i;

        for (i = 0; i < sizeof(src); i++) {
            src[i] = (unsigned char)(i * 37 + 11);
        }
        for (len = 0; len <= sizeof(src); len++) {
            base64_encode(src, len, enc);
            CHECK(base64_decode(enc, dec, &n) == 0 && n == len && memcmp(dec, src, len) == 0);
        }
    }
}

/* Random bytes and mutated frames must never crash or loop, whatever the split. */
static unsigned long lcg_state = 987654321UL;

static unsigned long lcg(void)
{
    lcg_state = lcg_state * 1103515245UL + 12345UL;
    return (lcg_state >> 16) & 0x7fffUL;
}

static void test_fuzz(void)
{
    unsigned char seed[400], buf[400], payload200[200];
    size_t seedlen = 0, len, pos, n;
    int iter, k;
    WsClient ws;
    WsEvent ev;

    memset(payload200, 'z', sizeof(payload200));
    seedlen += srv_frame(seed + seedlen, WS_OP_TEXT, 0, (const unsigned char *)"Hel", 3);
    seedlen += srv_frame(seed + seedlen, WS_OP_PING, 1, (const unsigned char *)"hi", 2);
    seedlen += srv_frame(seed + seedlen, WS_OP_CONT, 1, (const unsigned char *)"lo \xE2\x98\x83", 6);
    seedlen += srv_frame(seed + seedlen, WS_OP_BINARY, 1, payload200, 200);
    seedlen += srv_frame(seed + seedlen, WS_OP_CLOSE, 1, (const unsigned char *)"\x03\xe8ok", 4);
    for (iter = 0; iter < 20000; iter++) {
        len = seedlen < sizeof(buf) ? seedlen : sizeof(buf);
        memcpy(buf, seed, len);
        for (k = 0; k < 1 + (int)(lcg() % 5); k++) {
            switch (lcg() % 3) {
            case 0:
                buf[lcg() % len] = (unsigned char)(lcg() & 0xff);
                break;
            case 1:
                buf[lcg() % len] ^= (unsigned char)(1u << (lcg() % 8));
                break;
            default:
                if (len > 4) {
                    len -= lcg() % 3;
                }
                break;
            }
        }
        open_client(&ws);
        ws.max_message = 1000;
        for (pos = 0; pos < len;) {
            n = 1 + lcg() % 40;
            if (n > len - pos) {
                n = len - pos;
            }
            ws_input(&ws, buf + pos, n);
            pos += n;
            while (ws_next_event(&ws, &ev)) {
                ;
            }
        }
        ws_free(&ws);
    }
    CHECK(1);
}

int main(void)
{
    test_base64();
    test_handshake();
    test_incoming_frames();
    test_protocol_errors();
    test_sending();
    test_close_handshake();
    test_fuzz();
    TEST_DONE("ws");
}
