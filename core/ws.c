/* Sans-I/O WebSocket client: see ws.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ws.h"
#include "base64.h"
#include "sha1.h"
#include "rng.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define DEFAULT_MAX_MESSAGE (4UL * 1024UL * 1024UL)

/* ---------------------------------------------------------------- setup */

int ws_init(WsClient *ws, const char *url)
{
    HttpUrl u;

    memset(ws, 0, sizeof(*ws));
    if (http_parse_url(url, &u) != 0 || (strcmp(u.scheme, "ws") != 0 && strcmp(u.scheme, "wss") != 0)) {
        return -1;
    }
    strcpy(ws->host, u.host);
    ws->port = u.port;
    ws->tls = u.tls;
    strcpy(ws->path, u.path);
    ws->max_message = DEFAULT_MAX_MESSAGE;
    ws->rng = ssh_rng_bytes;
    ws->state = WS_CONNECTING;
    return 0;
}

void ws_free(WsClient *ws)
{
    free(ws->in);
    free(ws->msg);
    free(ws->out);
    memset(ws, 0, sizeof(*ws));
}

int ws_handshake_begin(WsClient *ws, HttpReq *req)
{
    unsigned char nonce[16], digest[20];
    char both[64];
    sha1_ctx c;

    if (ws->state != WS_CONNECTING) {
        return -1;
    }
    if (ws->rng(nonce, sizeof(nonce)) != 0) {
        return -1;
    }
    base64_encode(nonce, sizeof(nonce), ws->key);
    sprintf(both, "%s%s", ws->key, WS_GUID);
    sha1_init(&c);
    sha1_update(&c, both, strlen(both));
    sha1_final(&c, digest);
    base64_encode(digest, sizeof(digest), ws->accept);

    if (http_req_begin(req, "GET", ws->path, ws->host, ws->port, ws->tls) != 0 ||
        http_req_header(req, "Upgrade", "websocket") != 0 ||
        http_req_header(req, "Connection", "Upgrade") != 0 ||
        http_req_header(req, "Sec-WebSocket-Key", ws->key) != 0 ||
        http_req_header(req, "Sec-WebSocket-Version", "13") != 0) {
        return -1;
    }
    return 0;
}

static int hs_fail(WsClient *ws, const char *msg)
{
    sprintf(ws->err, "%.80s", msg);
    return -1;
}

int ws_handshake_check(WsClient *ws, const HttpResp *resp)
{
    const char *v;

    if (ws->state != WS_CONNECTING) {
        return hs_fail(ws, "handshake already done");
    }
    if (resp->status != 101) {
        sprintf(ws->err, "server answered %d instead of 101", resp->status);
        return -1;
    }
    v = http_resp_header(resp, "Upgrade");
    if (v == NULL || !http_list_has(v, "websocket")) {
        return hs_fail(ws, "missing or wrong Upgrade header");
    }
    v = http_resp_header(resp, "Connection");
    if (v == NULL || !http_list_has(v, "upgrade")) {
        return hs_fail(ws, "missing or wrong Connection header");
    }
    v = http_resp_header(resp, "Sec-WebSocket-Accept");
    if (v == NULL || strcmp(v, ws->accept) != 0) {
        return hs_fail(ws, "wrong Sec-WebSocket-Accept");
    }
    if (http_resp_header(resp, "Sec-WebSocket-Extensions") != NULL) {
        return hs_fail(ws, "server imposed an extension we did not offer");
    }
    if (http_resp_header(resp, "Sec-WebSocket-Protocol") != NULL) {
        return hs_fail(ws, "server imposed a subprotocol we did not offer");
    }
    ws->state = WS_OPEN;
    return 0;
}

/* ---------------------------------------------------------------- UTF-8 */

int ws_utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    unsigned char c;

    while (i < n) {
        c = s[i];
        if (c < 0x80) {
            i++;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= n || (s[i + 1] & 0xC0) != 0x80) {
                return 0;
            }
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (i + 2 >= n || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80) {
                return 0;
            }
            if (c == 0xE0 && s[i + 1] < 0xA0) {
                return 0;                       /* overlong */
            }
            if (c == 0xED && s[i + 1] >= 0xA0) {
                return 0;                       /* a surrogate half */
            }
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (i + 3 >= n || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80 ||
                (s[i + 3] & 0xC0) != 0x80) {
                return 0;
            }
            if (c == 0xF0 && s[i + 1] < 0x90) {
                return 0;                       /* overlong */
            }
            if (c == 0xF4 && s[i + 1] >= 0x90) {
                return 0;                       /* past U+10FFFF */
            }
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- output */

static int out_reserve(WsClient *ws, size_t more)
{
    size_t cap;
    unsigned char *nb;

    if (ws->out_pos > 0 && ws->out_pos == ws->out_len) {
        ws->out_pos = ws->out_len = 0;
    }
    if (ws->out_len + more <= ws->out_cap) {
        return 0;
    }
    cap = ws->out_cap ? ws->out_cap : 256;
    while (cap < ws->out_len + more) {
        cap *= 2;
    }
    nb = (unsigned char *)realloc(ws->out, cap);
    if (nb == NULL) {
        ws->oom = 1;
        return -1;
    }
    ws->out = nb;
    ws->out_cap = cap;
    return 0;
}

/* One complete masked frame onto the output queue. */
static int queue_frame(WsClient *ws, int opcode, const unsigned char *payload, size_t len)
{
    unsigned char hdr[14], mask[4];
    size_t hl, i;

    hdr[0] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        hdr[1] = (unsigned char)(0x80 | len);
        hl = 2;
    } else if (len <= 0xFFFFUL) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (unsigned char)(len >> 8);
        hdr[3] = (unsigned char)len;
        hl = 4;
    } else {
        hdr[1] = 0x80 | 127;
        hdr[2] = hdr[3] = hdr[4] = hdr[5] = 0;
        hdr[6] = (unsigned char)(len >> 24);
        hdr[7] = (unsigned char)(len >> 16);
        hdr[8] = (unsigned char)(len >> 8);
        hdr[9] = (unsigned char)len;
        hl = 10;
    }
    if (ws->rng(mask, 4) != 0) {
        return -1;
    }
    if (out_reserve(ws, hl + 4 + len) != 0) {
        return -1;
    }
    memcpy(ws->out + ws->out_len, hdr, hl);
    ws->out_len += hl;
    memcpy(ws->out + ws->out_len, mask, 4);
    ws->out_len += 4;
    for (i = 0; i < len; i++) {
        ws->out[ws->out_len + i] = (unsigned char)(payload[i] ^ mask[i & 3]);
    }
    ws->out_len += len;
    return 0;
}

const unsigned char *ws_output(WsClient *ws, size_t *len)
{
    *len = ws->out_len - ws->out_pos;
    return ws->out + ws->out_pos;
}

void ws_output_done(WsClient *ws, size_t n)
{
    ws->out_pos += n;
    if (ws->out_pos >= ws->out_len) {
        ws->out_pos = ws->out_len = 0;
    }
}

static int can_send(const WsClient *ws)
{
    return ws->state == WS_OPEN && !ws->close_sent && !ws->failed;
}

int ws_send_text(WsClient *ws, const char *s, size_t len)
{
    if (!can_send(ws) || !ws_utf8_valid((const unsigned char *)s, len)) {
        return -1;
    }
    return queue_frame(ws, WS_OP_TEXT, (const unsigned char *)s, len);
}

int ws_send_binary(WsClient *ws, const unsigned char *data, size_t len)
{
    if (!can_send(ws)) {
        return -1;
    }
    return queue_frame(ws, WS_OP_BINARY, data, len);
}

int ws_send_ping(WsClient *ws, const unsigned char *data, size_t len)
{
    if (!can_send(ws) || len > 125) {
        return -1;
    }
    return queue_frame(ws, WS_OP_PING, data, len);
}

static int close_code_ok(int code)
{
    return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999);
}

int ws_send_close(WsClient *ws, int code, const char *reason)
{
    unsigned char payload[125];
    size_t rl = reason ? strlen(reason) : 0, n = 0;

    if (!can_send(ws) || rl > 123 || (code != 0 && !close_code_ok(code)) ||
        (code == 0 && rl > 0) || !ws_utf8_valid((const unsigned char *)(reason ? reason : ""), rl)) {
        return -1;
    }
    if (code != 0) {
        payload[0] = (unsigned char)(code >> 8);
        payload[1] = (unsigned char)code;
        n = 2;
        if (rl > 0) {
            memcpy(payload + 2, reason, rl);
            n += rl;
        }
    }
    if (queue_frame(ws, WS_OP_CLOSE, payload, n) != 0) {
        return -1;
    }
    ws->close_sent = 1;
    ws->state = WS_CLOSING;
    return 0;
}

int ws_is_closed(const WsClient *ws)
{
    return ws->state == WS_CLOSED;
}

/* ---------------------------------------------------------------- input */

int ws_input(WsClient *ws, const unsigned char *data, size_t len)
{
    size_t cap;
    unsigned char *nb;

    if (ws->state == WS_CLOSED || ws->failed) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (ws->in_pos > 0 && ws->in_pos == ws->in_len) {
        ws->in_pos = ws->in_len = 0;
    } else if (ws->in_pos > 0 && ws->in_len + len > ws->in_cap) {
        memmove(ws->in, ws->in + ws->in_pos, ws->in_len - ws->in_pos);
        ws->in_len -= ws->in_pos;
        ws->in_pos = 0;
    }
    if (ws->in_len + len > ws->in_cap) {
        cap = ws->in_cap ? ws->in_cap : 1024;
        while (cap < ws->in_len + len) {
            cap *= 2;
        }
        nb = (unsigned char *)realloc(ws->in, cap);
        if (nb == NULL) {
            ws->oom = 1;
            ws->failed = 1;
            sprintf(ws->err, "out of memory");
            return -1;
        }
        ws->in = nb;
        ws->in_cap = cap;
    }
    memcpy(ws->in + ws->in_len, data, len);
    ws->in_len += len;
    return 0;
}

/* A protocol violation: queue a close frame, stop, and report it once. */
static int proto_fail(WsClient *ws, WsEvent *ev, int code, const char *msg)
{
    unsigned char payload[2];

    if (!ws->close_sent) {
        payload[0] = (unsigned char)(code >> 8);
        payload[1] = (unsigned char)code;
        queue_frame(ws, WS_OP_CLOSE, payload, 2);
        ws->close_sent = 1;
    }
    ws->failed = 1;
    ws->state = WS_CLOSED;
    sprintf(ws->err, "%.80s", msg);
    ev->type = WS_EV_ERROR;
    ev->code = code;
    ev->text = ws->err;
    return 1;
}

static int msg_append(WsClient *ws, const unsigned char *d, size_t n)
{
    size_t cap;
    unsigned char *nb;

    if (n > ws->max_message || ws->msg_len > ws->max_message - n) {
        return -2;
    }
    if (ws->msg_len + n + 1 > ws->msg_cap) {
        cap = ws->msg_cap ? ws->msg_cap : 1024;
        while (cap < ws->msg_len + n + 1) {
            cap *= 2;
        }
        nb = (unsigned char *)realloc(ws->msg, cap);
        if (nb == NULL) {
            return -1;
        }
        ws->msg = nb;
        ws->msg_cap = cap;
    }
    memcpy(ws->msg + ws->msg_len, d, n);
    ws->msg_len += n;
    ws->msg[ws->msg_len] = '\0';
    return 0;
}

static int deliver_message(WsClient *ws, WsEvent *ev)
{
    ws->fragmenting = 0;
    if (ws->msg_opcode == WS_OP_TEXT && !ws_utf8_valid(ws->msg, ws->msg_len)) {
        return proto_fail(ws, ev, 1007, "invalid UTF-8 in a text message");
    }
    ev->type = ws->msg_opcode == WS_OP_TEXT ? WS_EV_TEXT : WS_EV_BINARY;
    ev->data = ws->msg;
    ev->len = ws->msg_len;
    ws->msg_delivered = 1;
    return 1;
}

int ws_next_event(WsClient *ws, WsEvent *ev)
{
    const unsigned char *p;
    size_t avail, hl, plen, i;
    int opcode, fin, len7, rc, code;
    unsigned char payload[2];

    memset(ev, 0, sizeof(*ev));
    if (ws->msg_delivered) {
        ws->msg_len = 0;
        ws->msg_delivered = 0;
    }
    if (ws->state == WS_CLOSED || ws->failed) {
        return 0;
    }
    if (ws->state == WS_CONNECTING) {
        return 0;
    }
    for (;;) {
        avail = ws->in_len - ws->in_pos;
        p = ws->in + ws->in_pos;
        if (avail < 2) {
            return 0;
        }
        if (p[0] & 0x70) {
            return proto_fail(ws, ev, 1002, "reserved bits set");
        }
        fin = (p[0] & 0x80) != 0;
        opcode = p[0] & 0x0F;
        if (p[1] & 0x80) {
            return proto_fail(ws, ev, 1002, "server frame is masked");
        }
        len7 = p[1] & 0x7F;
        hl = 2;
        if (len7 == 126) {
            if (avail < 4) {
                return 0;
            }
            plen = ((size_t)p[2] << 8) | p[3];
            hl = 4;
        } else if (len7 == 127) {
            if (avail < 10) {
                return 0;
            }
            if (p[2] || p[3] || p[4] || p[5]) {
                return proto_fail(ws, ev, 1009, "frame too large");
            }
            plen = ((size_t)p[6] << 24) | ((size_t)p[7] << 16) | ((size_t)p[8] << 8) | p[9];
            hl = 10;
        } else {
            plen = (size_t)len7;
        }
        if (opcode >= 8) {
            if (!fin || len7 > 125) {
                return proto_fail(ws, ev, 1002, "bad control frame");
            }
            if (opcode > 10) {
                return proto_fail(ws, ev, 1002, "unknown control opcode");
            }
        } else if (opcode > 2) {
            return proto_fail(ws, ev, 1002, "unknown opcode");
        }
        if (plen > ws->max_message) {
            return proto_fail(ws, ev, 1009, "message too big");
        }
        if (avail < hl + plen) {
            return 0;                         /* the rest of the frame has not arrived */
        }
        p += hl;
        ws->in_pos += hl + plen;

        switch (opcode) {
        case WS_OP_CONT:
        case WS_OP_TEXT:
        case WS_OP_BINARY:
            if (opcode == WS_OP_CONT) {
                if (!ws->fragmenting) {
                    return proto_fail(ws, ev, 1002, "continuation without a message");
                }
            } else {
                if (ws->fragmenting) {
                    return proto_fail(ws, ev, 1002, "new message inside a fragmented one");
                }
                ws->msg_opcode = opcode;
                ws->msg_len = 0;
            }
            rc = msg_append(ws, p, plen);
            if (rc == -2) {
                return proto_fail(ws, ev, 1009, "message too big");
            }
            if (rc != 0) {
                return proto_fail(ws, ev, 1011, "out of memory");
            }
            if (fin) {
                return deliver_message(ws, ev);
            }
            ws->fragmenting = 1;
            break;                            /* more frames may already be buffered */

        case WS_OP_PING:
            memcpy(ws->ctl, p, plen);
            if (!ws->close_sent && queue_frame(ws, WS_OP_PONG, ws->ctl, plen) != 0) {
                return proto_fail(ws, ev, 1011, "could not queue a pong");
            }
            ev->type = WS_EV_PING;
            ev->data = ws->ctl;
            ev->len = plen;
            return 1;

        case WS_OP_PONG:
            memcpy(ws->ctl, p, plen);
            ev->type = WS_EV_PONG;
            ev->data = ws->ctl;
            ev->len = plen;
            return 1;

        default:                              /* WS_OP_CLOSE */
            if (plen == 0) {
                code = 1005;
                ws->reason[0] = '\0';
            } else if (plen == 1) {
                return proto_fail(ws, ev, 1002, "close frame with a one-byte payload");
            } else {
                code = (p[0] << 8) | p[1];
                if (!close_code_ok(code)) {
                    return proto_fail(ws, ev, 1002, "invalid close code");
                }
                if (!ws_utf8_valid(p + 2, plen - 2)) {
                    return proto_fail(ws, ev, 1007, "invalid UTF-8 in a close reason");
                }
                for (i = 0; i < plen - 2; i++) {
                    ws->reason[i] = (char)p[2 + i];
                }
                ws->reason[plen - 2] = '\0';
            }
            if (!ws->close_sent) {
                if (code == 1005) {
                    queue_frame(ws, WS_OP_CLOSE, NULL, 0);
                } else {
                    payload[0] = (unsigned char)(code >> 8);
                    payload[1] = (unsigned char)code;
                    queue_frame(ws, WS_OP_CLOSE, payload, 2);
                }
                ws->close_sent = 1;
            }
            ws->state = WS_CLOSED;
            ev->type = WS_EV_CLOSE;
            ev->code = code;
            ev->text = ws->reason;
            return 1;
        }
    }
}
