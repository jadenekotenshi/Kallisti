/*
 * ws.h -- a sans-I/O WebSocket client (RFC 6455), for Discord's gateway.
 *
 * Like tls.h and http.h it never touches a socket:
 *   1. ws_init(), then ws_handshake_begin() adds the upgrade headers to an HttpReq (the caller may add
 *      its own, then http_req_end()) and the request is sent over the (TLS) connection;
 *   2. the reply is parsed with http_resp_feed(); on HTTP_DONE ws_handshake_check() verifies the 101
 *      and the Sec-WebSocket-Accept value, and the bytes http_resp_feed() left over go to ws_input();
 *   3. from then on: ws_input() whatever arrives, drain events with ws_next_event(), send what
 *      ws_output() exposes and call ws_output_done().
 *
 * No extensions or subprotocols are negotiated (so no permessage-deflate: ask Discord for plain JSON).
 * Incoming messages are reassembled from fragments; control frames may arrive in between; pings are
 * answered automatically; text messages and close reasons are checked to be valid UTF-8; every
 * protocol violation queues a close frame and yields one WS_EV_ERROR event.  Outgoing messages go out
 * as a single masked frame.
 */
#ifndef KALLISTI_WS_H
#define KALLISTI_WS_H

#include <stddef.h>
#include "http.h"

enum { WS_CONNECTING = 0, WS_OPEN, WS_CLOSING, WS_CLOSED };

enum { WS_OP_CONT = 0, WS_OP_TEXT = 1, WS_OP_BINARY = 2, WS_OP_CLOSE = 8, WS_OP_PING = 9, WS_OP_PONG = 10 };

enum {
    WS_EV_NONE = 0,
    WS_EV_TEXT,              /* data/len: a complete text message (valid UTF-8; also NUL-terminated) */
    WS_EV_BINARY,
    WS_EV_PING,              /* already answered with a pong */
    WS_EV_PONG,
    WS_EV_CLOSE,             /* the peer closed: code (1005 if it gave none), text = reason; a close was
                                sent back if ours had not gone out yet.  The connection may now be dropped. */
    WS_EV_ERROR              /* protocol violation: text = description; a close frame is queued */
};

typedef struct {
    int type;
    const unsigned char *data;   /* valid until the next ws_next_event() or ws_input() */
    size_t len;
    int code;
    const char *text;
} WsEvent;

typedef struct {
    char host[256];
    int port;
    int tls;
    char path[2048];

    size_t max_message;      /* reassembled message size limit; larger ones are a 1009 error (default 4 MB) */
    int (*rng)(void *out, size_t len);   /* source of the handshake nonce and frame masks: ssh_rng_bytes */

    char key[32];            /* Sec-WebSocket-Key we sent */
    char accept[32];         /* the Sec-WebSocket-Accept we expect */

    int state;
    unsigned char *in;
    size_t in_pos, in_len, in_cap;
    unsigned char *msg;
    size_t msg_len, msg_cap;
    int msg_opcode, fragmenting, msg_delivered;
    unsigned char *out;
    size_t out_pos, out_len, out_cap;
    unsigned char ctl[128];
    char reason[128];
    int close_sent, failed, oom;
    char err[96];
} WsClient;

/* url must be ws:// or wss://.  Returns 0 or -1. */
int ws_init(WsClient *ws, const char *url);
void ws_free(WsClient *ws);

/* Starts the upgrade request for the URL given to ws_init() and adds the WebSocket headers; the caller
 * may add more headers and must finish with http_req_end(req, NULL, 0).  Returns 0 or -1. */
int ws_handshake_begin(WsClient *ws, HttpReq *req);
/* Checks a complete response: 101, Upgrade/Connection, the exact Sec-WebSocket-Accept, and that no
 * extension or subprotocol was imposed.  Returns 0 (the session is now open) or -1 (ws->err). */
int ws_handshake_check(WsClient *ws, const HttpResp *resp);

/* Incoming bytes (after the handshake).  Returns 0, or -1 if the session has already failed/closed. */
int ws_input(WsClient *ws, const unsigned char *data, size_t len);
/* Next event, or 0 when more input is needed.  Fills *ev. */
int ws_next_event(WsClient *ws, WsEvent *ev);

/* Outgoing bytes: send ws_output(), then ws_output_done() with how many went. */
const unsigned char *ws_output(WsClient *ws, size_t *len);
void ws_output_done(WsClient *ws, size_t n);

/* Queue a message or control frame.  All return 0, or -1 if the session is not open, a close has
 * been sent, the payload is too big for a control frame (125 bytes), or memory ran out.
 * ws_send_text requires valid UTF-8. */
int ws_send_text(WsClient *ws, const char *s, size_t len);
int ws_send_binary(WsClient *ws, const unsigned char *data, size_t len);
int ws_send_ping(WsClient *ws, const unsigned char *data, size_t len);
/* code 0 sends an empty close; otherwise 1000-1003, 1007-1011 or 3000-4999.  reason up to 123 bytes. */
int ws_send_close(WsClient *ws, int code, const char *reason);

int ws_is_closed(const WsClient *ws);

/* Exposed for tests: strict UTF-8 (no overlongs, surrogates or code points past U+10FFFF). */
int ws_utf8_valid(const unsigned char *s, size_t n);

#endif
