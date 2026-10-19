/*
 * http.h -- a sans-I/O HTTP/1.1 client: a request builder, an incremental response parser and a URL
 * parser.  Like tls.h it never touches a socket: the application writes the bytes http_req_* produced
 * to the connection (through tls_write for https) and feeds whatever it reads to http_resp_feed().
 *
 * Responses handled: Content-Length, chunked, and read-until-close bodies; 1xx interim responses are
 * skipped; 204/304 and replies to HEAD carry no body; 101 Switching Protocols finishes at the end of
 * the headers and leaves the rest of the stream (the WebSocket frames) unconsumed.
 *
 * Requests always ask for "Accept-Encoding: identity", so no decompression is ever needed.
 */
#ifndef KALLISTI_HTTP_H
#define KALLISTI_HTTP_H

#include <stddef.h>

#define HTTP_MAX_HEADER_BYTES 65536
#define HTTP_MAX_TRAILER_BYTES 8192

enum { HTTP_NEED_MORE = 0, HTTP_DONE = 1, HTTP_ERROR = -1 };

/* ---------------------------------------------------------------- URLs */

typedef struct {
    char scheme[8];          /* "http", "https", "ws" or "wss" (lower case) */
    char host[256];          /* without brackets for an IPv6 literal */
    int port;                /* explicit, or the scheme's default */
    int tls;                 /* https and wss */
    char path[2048];         /* path and query, at least "/" */
} HttpUrl;

/* Returns 0, or -1 for anything that is not a plain absolute http/https/ws/wss URL. */
int http_parse_url(const char *url, HttpUrl *u);

/* ---------------------------------------------------------------- requests */

typedef struct {
    char *buf;
    size_t len, cap;
    int failed;              /* out of memory, or a header/path with CR, LF or other control characters */
    int finished;
    int wants_len;           /* POST/PUT/PATCH: an empty body still gets Content-Length: 0 */
} HttpReq;

void http_req_init(HttpReq *r);
void http_req_free(HttpReq *r);
/* Request line, Host (the port is omitted when it is the scheme's default) and Accept-Encoding. */
int http_req_begin(HttpReq *r, const char *method, const char *path, const char *host, int port, int tls);
int http_req_header(HttpReq *r, const char *name, const char *value);
/* Adds Content-Length when there is a body (or the method normally has one) and ends the header block.
 * Afterwards r->buf/r->len are the bytes to send.  Returns 0, or -1 if anything failed. */
int http_req_end(HttpReq *r, const void *body, size_t body_len);

/* ---------------------------------------------------------------- responses */

typedef struct {
    const char *name;
    const char *value;
} HttpHeader;

typedef struct {
    int state;
    int status;              /* e.g. 200 */
    int minor;               /* 0 or 1 */
    const char *reason;
    HttpHeader *hdrs;
    int nhdrs;
    int keep_alive;          /* the connection may carry another request after this response */
    int head;                /* the request was HEAD: no body whatever the headers say */

    unsigned char *body;     /* the whole body, NUL-terminated for convenience */
    size_t body_len, body_cap, max_body;

    char *hb;                /* the header block: name/value strings point into it */
    size_t hb_len, hb_cap;
    size_t line_len;         /* length of the header line being read, without CR */

    size_t left;             /* body bytes still to come (Content-Length, or the current chunk) */
    size_t trailer_bytes;
    char sizeline[80];
    size_t sizeline_len;

    char err[96];
} HttpResp;

void http_resp_init(HttpResp *r, size_t max_body, int head_request);
void http_resp_free(HttpResp *r);
/* Consumes data[0..len): *used says how much (less than len only when the response ended earlier;
 * the rest belongs to whatever comes next, e.g. the WebSocket after a 101).  Returns HTTP_NEED_MORE,
 * HTTP_DONE or HTTP_ERROR (message in r->err). */
int http_resp_feed(HttpResp *r, const unsigned char *data, size_t len, size_t *used);
/* The peer closed the connection: finishes a read-until-close body, fails anything else incomplete. */
int http_resp_eof(HttpResp *r);
/* Does a comma-separated header value (e.g. "keep-alive, Upgrade") contain this token, ignoring case? */
int http_list_has(const char *value, const char *token);
/* Case-insensitive lookup of the first header with this name, or NULL. */
const char *http_resp_header(const HttpResp *r, const char *name);

#endif
