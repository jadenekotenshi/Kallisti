/* Sans-I/O HTTP/1.1 client: see http.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"

/* ---------------------------------------------------------------- small helpers */

static int ci_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int ci_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (ci_lower((unsigned char)*a) != ci_lower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int is_token_char(int c)
{
    if (c >= '0' && c <= '9') {
        return 1;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return 1;
    }
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int is_token(const char *s)
{
    if (*s == '\0') {
        return 0;
    }
    for (; *s != '\0'; s++) {
        if (!is_token_char((unsigned char)*s)) {
            return 0;
        }
    }
    return 1;
}

/* no control characters, no DEL; spaces allowed only if allow_space */
static int clean_text(const char *s, int allow_space)
{
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;

        if (c < 0x20 || c == 0x7f || (c == ' ' && !allow_space)) {
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- URLs */

int http_parse_url(const char *url, HttpUrl *u)
{
    const char *p, *q, *hs, *he, *path, *colon;
    size_t n, i;
    long port;

    memset(u, 0, sizeof(*u));
    if (url == NULL || !clean_text(url, 0)) {
        return -1;
    }
    p = strstr(url, "://");
    if (p == NULL) {
        return -1;
    }
    n = (size_t)(p - url);
    if (n == 0 || n >= sizeof(u->scheme)) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        u->scheme[i] = (char)ci_lower((unsigned char)url[i]);
    }
    u->scheme[n] = '\0';
    if (strcmp(u->scheme, "http") == 0 || strcmp(u->scheme, "ws") == 0) {
        u->port = 80;
        u->tls = 0;
    } else if (strcmp(u->scheme, "https") == 0 || strcmp(u->scheme, "wss") == 0) {
        u->port = 443;
        u->tls = 1;
    } else {
        return -1;
    }
    p += 3;
    q = p;
    while (*q != '\0' && *q != '/' && *q != '?' && *q != '#') {
        q++;
    }
    if (q == p || memchr(p, '@', (size_t)(q - p)) != NULL) {
        return -1;
    }
    if (*p == '[') {
        hs = p + 1;
        he = memchr(p, ']', (size_t)(q - p));
        if (he == NULL || he == hs) {
            return -1;
        }
        if (he + 1 < q) {
            if (he[1] != ':') {
                return -1;
            }
            colon = he + 1;
        } else {
            colon = NULL;
        }
    } else {
        hs = p;
        colon = NULL;
        for (he = q; he > p; he--) {
            if (he[-1] == ':') {
                colon = he - 1;
                break;
            }
        }
        he = colon != NULL ? colon : q;
        if (he == hs) {
            return -1;
        }
    }
    n = (size_t)(he - hs);
    if (n >= sizeof(u->host)) {
        return -1;
    }
    memcpy(u->host, hs, n);
    u->host[n] = '\0';
    if (colon != NULL) {
        port = 0;
        if (colon + 1 >= q) {
            return -1;
        }
        for (hs = colon + 1; hs < q; hs++) {
            if (*hs < '0' || *hs > '9') {
                return -1;
            }
            port = port * 10 + (*hs - '0');
            if (port > 65535L) {
                return -1;
            }
        }
        if (port == 0) {
            return -1;
        }
        u->port = (int)port;
    }
    path = q;
    if (*path == '/' || *path == '?') {
        n = 0;
        if (*path == '?') {
            u->path[n++] = '/';
        }
        while (*path != '\0' && *path != '#') {
            if (n + 1 >= sizeof(u->path)) {
                return -1;
            }
            u->path[n++] = *path++;
        }
        u->path[n] = '\0';
    } else {
        strcpy(u->path, "/");
    }
    return 0;
}

/* ---------------------------------------------------------------- requests */

static int req_put(HttpReq *r, const char *s, size_t n)
{
    size_t cap;
    char *nb;

    if (r->failed) {
        return -1;
    }
    if (r->len + n + 1 > r->cap) {
        cap = r->cap ? r->cap : 512;
        while (cap < r->len + n + 1) {
            cap *= 2;
        }
        nb = (char *)realloc(r->buf, cap);
        if (nb == NULL) {
            r->failed = 1;
            return -1;
        }
        r->buf = nb;
        r->cap = cap;
    }
    memcpy(r->buf + r->len, s, n);
    r->len += n;
    r->buf[r->len] = '\0';
    return 0;
}

static int req_puts(HttpReq *r, const char *s)
{
    return req_put(r, s, strlen(s));
}

void http_req_init(HttpReq *r)
{
    memset(r, 0, sizeof(*r));
}

void http_req_free(HttpReq *r)
{
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

int http_req_begin(HttpReq *r, const char *method, const char *path, const char *host, int port, int tls)
{
    char num[16];

    if (method == NULL || strlen(method) > 16 || !is_token(method) ||
        path == NULL || path[0] != '/' || !clean_text(path, 0) ||
        host == NULL || host[0] == '\0' || !clean_text(host, 0) || port < 1 || port > 65535) {
        r->failed = 1;
        return -1;
    }
    req_puts(r, method);
    req_puts(r, " ");
    req_puts(r, path);
    req_puts(r, " HTTP/1.1\r\nHost: ");
    if (strchr(host, ':') != NULL) {
        req_puts(r, "[");
        req_puts(r, host);
        req_puts(r, "]");
    } else {
        req_puts(r, host);
    }
    if (!((tls && port == 443) || (!tls && port == 80))) {
        sprintf(num, ":%d", port);
        req_puts(r, num);
    }
    req_puts(r, "\r\nAccept-Encoding: identity\r\n");
    r->wants_len = (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 ||
                    strcmp(method, "PATCH") == 0);
    return r->failed ? -1 : 0;
}

int http_req_header(HttpReq *r, const char *name, const char *value)
{
    if (name == NULL || value == NULL || !is_token(name) || !clean_text(value, 1)) {
        r->failed = 1;
        return -1;
    }
    req_puts(r, name);
    req_puts(r, ": ");
    req_puts(r, value);
    req_puts(r, "\r\n");
    return r->failed ? -1 : 0;
}

int http_req_end(HttpReq *r, const void *body, size_t body_len)
{
    char num[32];
    if (body_len > 0 || r->wants_len) {
        sprintf(num, "Content-Length: %lu\r\n", (unsigned long)body_len);
        req_puts(r, num);
    }
    req_puts(r, "\r\n");
    if (body_len > 0) {
        req_put(r, (const char *)body, body_len);
    }
    r->finished = 1;
    return r->failed ? -1 : 0;
}

/* ---------------------------------------------------------------- responses */

enum {
    ST_HEAD = 0, ST_BODY_FIXED, ST_BODY_CLOSE, ST_CHUNK_SIZE, ST_CHUNK_DATA, ST_CHUNK_CRLF,
    ST_TRAILER, ST_DONE, ST_ERROR
};

void http_resp_init(HttpResp *r, size_t max_body, int head_request)
{
    memset(r, 0, sizeof(*r));
    r->state = ST_HEAD;
    r->max_body = max_body;
    r->head = head_request;
}

void http_resp_free(HttpResp *r)
{
    free(r->hb);
    free(r->hdrs);
    free(r->body);
    memset(r, 0, sizeof(*r));
}

static int resp_fail(HttpResp *r, const char *msg)
{
    if (r->err[0] == '\0') {
        sprintf(r->err, "%.80s", msg);
    }
    r->state = ST_ERROR;
    return HTTP_FAILED;
}

const char *http_resp_header(const HttpResp *r, const char *name)
{
    int i;

    for (i = 0; i < r->nhdrs; i++) {
        if (ci_equal(r->hdrs[i].name, name)) {
            return r->hdrs[i].value;
        }
    }
    return NULL;
}

/* does the comma-separated header value contain this token (case-insensitive)? */
static int list_has(const char *v, const char *token, int last_only)
{
    size_t tl = strlen(token), n;
    const char *p = v, *e;
    int found = 0;

    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        e = p;
        while (*e != '\0' && *e != ',') {
            e++;
        }
        n = (size_t)(e - p);
        while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) {
            n--;
        }
        if (n > 0) {
            found = (n == tl);
            if (found) {
                size_t k;
                for (k = 0; k < n; k++) {
                    if (ci_lower((unsigned char)p[k]) != ci_lower((unsigned char)token[k])) {
                        found = 0;
                        break;
                    }
                }
            }
            if (found && !last_only) {
                return 1;
            }
        }
        p = e;
    }
    return found;
}

int http_list_has(const char *value, const char *token)
{
    return list_has(value, token, 0);
}

static int body_append(HttpResp *r, const unsigned char *d, size_t n)
{
    size_t cap;
    unsigned char *nb;

    if (n > r->max_body || r->body_len > r->max_body - n) {
        return resp_fail(r, "response body too large");
    }
    if (r->body_len + n + 1 > r->body_cap) {
        cap = r->body_cap ? r->body_cap : 1024;
        while (cap < r->body_len + n + 1) {
            cap *= 2;
        }
        nb = (unsigned char *)realloc(r->body, cap);
        if (nb == NULL) {
            return resp_fail(r, "out of memory");
        }
        r->body = nb;
        r->body_cap = cap;
    }
    memcpy(r->body + r->body_len, d, n);
    r->body_len += n;
    r->body[r->body_len] = '\0';
    return HTTP_NEED_MORE;
}

/* Parses the complete header block in r->hb.  Returns 1: the response ends here (no body, or a 101),
 * 2: an interim 1xx response was discarded, 0: a body follows (state set), -1: error. */
static int head_complete(HttpResp *r)
{
    char *p, *line, *nl, *colon, *v;
    size_t nlines = 0, i, vl;
    int n;
    int have_cl = 0, chunked = 0, has_te = 0;
    unsigned long cl = 0, one;
    const char *conn;

    if (r->hb_len + 1 > r->hb_cap) {
        char *nb = (char *)realloc(r->hb, r->hb_len + 1);

        if (nb == NULL) {
            resp_fail(r, "out of memory");
            return -1;
        }
        r->hb = nb;
        r->hb_cap = r->hb_len + 1;
    }
    r->hb[r->hb_len] = '\0';
    for (i = 0; i < r->hb_len; i++) {
        if (r->hb[i] == '\n') {
            nlines++;
        }
    }
    free(r->hdrs);
    r->hdrs = (HttpHeader *)calloc(nlines + 1, sizeof(HttpHeader));
    if (r->hdrs == NULL) {
        resp_fail(r, "out of memory");
        return -1;
    }
    r->nhdrs = 0;

    /* status line */
    line = r->hb;
    nl = strchr(line, '\n');
    if (nl == NULL) {
        resp_fail(r, "no status line");
        return -1;
    }
    *nl = '\0';
    if (nl > line && nl[-1] == '\r') {
        nl[-1] = '\0';
    }
    if (strncmp(line, "HTTP/1.", 7) != 0 || (line[7] != '0' && line[7] != '1') || line[8] != ' ') {
        resp_fail(r, "bad status line");
        return -1;
    }
    r->minor = line[7] - '0';
    p = line + 9;
    if (!(p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9') ||
        (p[3] != '\0' && p[3] != ' ')) {
        resp_fail(r, "bad status code");
        return -1;
    }
    r->status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
    r->reason = p[3] == ' ' ? p + 4 : p + 3;

    /* header lines */
    p = nl + 1;
    while (*p != '\0') {
        line = p;
        nl = strchr(line, '\n');
        if (nl == NULL) {
            nl = line + strlen(line);
            p = nl;
        } else {
            p = nl + 1;
        }
        if (nl > line && nl[-1] == '\r') {
            nl--;
        }
        *nl = '\0';
        if (line == nl) {
            break;
        }
        if (*line == ' ' || *line == '\t') {
            resp_fail(r, "obsolete header line folding");
            return -1;
        }
        colon = strchr(line, ':');
        if (colon == NULL || colon == line) {
            resp_fail(r, "malformed header line");
            return -1;
        }
        *colon = '\0';
        if (!is_token(line)) {
            resp_fail(r, "bad header name");
            return -1;
        }
        v = colon + 1;
        while (*v == ' ' || *v == '\t') {
            v++;
        }
        vl = strlen(v);
        while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) {
            v[--vl] = '\0';
        }
        r->hdrs[r->nhdrs].name = line;
        r->hdrs[r->nhdrs].value = v;
        r->nhdrs++;
    }

    if (r->status >= 100 && r->status < 200) {
        if (r->status == 101) {
            return 1;
        }
        /* an interim response (100 Continue, ...): forget it and read the real one */
        r->hb_len = 0;
        r->line_len = 0;
        r->nhdrs = 0;
        return 2;
    }

    for (n = 0; n < r->nhdrs; n++) {
        if (ci_equal(r->hdrs[n].name, "Content-Length")) {
            const char *s = r->hdrs[n].value;

            if (*s == '\0') {
                resp_fail(r, "empty Content-Length");
                return -1;
            }
            one = 0;
            for (; *s != '\0'; s++) {
                if (*s < '0' || *s > '9' || one > (0x7FFFFFFFUL - (unsigned long)(*s - '0')) / 10) {
                    resp_fail(r, "bad Content-Length");
                    return -1;
                }
                one = one * 10 + (unsigned long)(*s - '0');
            }
            if (have_cl && one != cl) {
                resp_fail(r, "conflicting Content-Length headers");
                return -1;
            }
            have_cl = 1;
            cl = one;
        } else if (ci_equal(r->hdrs[n].name, "Transfer-Encoding")) {
            has_te = 1;
            if (list_has(r->hdrs[n].value, "chunked", 1)) {
                chunked = 1;
            } else {
                chunked = 0;
            }
        }
    }

    conn = http_resp_header(r, "Connection");
    if (r->minor == 1) {
        r->keep_alive = !(conn != NULL && list_has(conn, "close", 0));
    } else {
        r->keep_alive = (conn != NULL && list_has(conn, "keep-alive", 0));
    }

    if (r->head || r->status == 204 || r->status == 304) {
        return 1;
    }
    if (chunked) {
        r->state = ST_CHUNK_SIZE;
        r->sizeline_len = 0;
        return 0;
    }
    if (has_te) {
        /* a transfer coding that does not end in chunked: the body runs until the connection closes */
        r->keep_alive = 0;
        r->state = ST_BODY_CLOSE;
        return 0;
    }
    if (have_cl) {
        if ((size_t)cl > r->max_body) {
            resp_fail(r, "response body too large");
            return -1;
        }
        if (cl == 0) {
            return 1;
        }
        r->left = (size_t)cl;
        r->state = ST_BODY_FIXED;
        return 0;
    }
    r->keep_alive = 0;
    r->state = ST_BODY_CLOSE;
    return 0;
}

static int parse_chunk_size(HttpResp *r)
{
    const char *s = r->sizeline;
    unsigned long v = 0;
    int digits = 0, c;

    for (;; s++) {
        c = (unsigned char)*s;
        if (c >= '0' && c <= '9') {
            c -= '0';
        } else if (c >= 'a' && c <= 'f') {
            c = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            c = c - 'A' + 10;
        } else {
            break;
        }
        if (v > (0x7FFFFFFFUL - (unsigned long)c) / 16) {
            return resp_fail(r, "chunk size overflow");
        }
        v = v * 16 + (unsigned long)c;
        digits++;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (digits == 0 || (*s != '\0' && *s != ';')) {
        return resp_fail(r, "bad chunk size");
    }
    if ((size_t)v > r->max_body || r->body_len > r->max_body - (size_t)v) {
        return resp_fail(r, "response body too large");
    }
    r->left = (size_t)v;
    r->state = (v == 0) ? ST_TRAILER : ST_CHUNK_DATA;
    r->sizeline_len = 0;
    return HTTP_NEED_MORE;
}

int http_resp_feed(HttpResp *r, const unsigned char *data, size_t len, size_t *used)
{
    size_t i = 0, n;
    int rc;
    unsigned char c;

    *used = 0;
    if (r->state == ST_DONE) {
        return HTTP_DONE;
    }
    if (r->state == ST_ERROR) {
        return HTTP_FAILED;
    }
    while (i < len) {
        switch (r->state) {
        case ST_HEAD:
            c = data[i];
            if (r->hb_len == 0 && (c == '\r' || c == '\n')) {
                i++;                       /* stray blank lines before the status line */
                break;
            }
            if (r->hb_len >= HTTP_MAX_HEADER_BYTES) {
                *used = i;
                return resp_fail(r, "response headers too large");
            }
            if (r->hb_len + 1 > r->hb_cap) {
                size_t cap = r->hb_cap ? r->hb_cap * 2 : 512;
                char *nb = (char *)realloc(r->hb, cap);

                if (nb == NULL) {
                    return resp_fail(r, "out of memory");
                }
                r->hb = nb;
                r->hb_cap = cap;
            }
            r->hb[r->hb_len++] = (char)c;
            i++;
            if (c == '\n') {
                if (r->line_len == 0) {
                    rc = head_complete(r);
                    if (rc < 0) {
                        *used = i;
                        return HTTP_FAILED;
                    }
                    if (rc == 1) {
                        r->state = ST_DONE;
                        *used = i;
                        return HTTP_DONE;
                    }
                } else {
                    r->line_len = 0;
                }
            } else if (c != '\r') {
                r->line_len++;
            }
            break;

        case ST_BODY_FIXED:
            n = len - i;
            if (n > r->left) {
                n = r->left;
            }
            if (body_append(r, data + i, n) != HTTP_NEED_MORE) {
                *used = i;
                return HTTP_FAILED;
            }
            i += n;
            r->left -= n;
            if (r->left == 0) {
                r->state = ST_DONE;
                *used = i;
                return HTTP_DONE;
            }
            break;

        case ST_BODY_CLOSE:
            if (body_append(r, data + i, len - i) != HTTP_NEED_MORE) {
                *used = i;
                return HTTP_FAILED;
            }
            i = len;
            break;

        case ST_CHUNK_SIZE:
            c = data[i++];
            if (c == '\n') {
                r->sizeline[r->sizeline_len] = '\0';
                if (r->sizeline_len > 0 && r->sizeline[r->sizeline_len - 1] == '\r') {
                    r->sizeline[--r->sizeline_len] = '\0';
                }
                if (parse_chunk_size(r) != HTTP_NEED_MORE) {
                    *used = i;
                    return HTTP_FAILED;
                }
            } else {
                if (r->sizeline_len + 1 >= sizeof(r->sizeline)) {
                    *used = i;
                    return resp_fail(r, "chunk size line too long");
                }
                r->sizeline[r->sizeline_len++] = (char)c;
            }
            break;

        case ST_CHUNK_DATA:
            n = len - i;
            if (n > r->left) {
                n = r->left;
            }
            if (body_append(r, data + i, n) != HTTP_NEED_MORE) {
                *used = i;
                return HTTP_FAILED;
            }
            i += n;
            r->left -= n;
            if (r->left == 0) {
                r->state = ST_CHUNK_CRLF;
                r->sizeline_len = 0;
            }
            break;

        case ST_CHUNK_CRLF:
            c = data[i++];
            if (c == '\r' && r->sizeline_len == 0) {
                r->sizeline_len = 1;
            } else if (c == '\n') {
                r->sizeline_len = 0;
                r->state = ST_CHUNK_SIZE;
            } else {
                *used = i;
                return resp_fail(r, "missing CRLF after chunk data");
            }
            break;

        case ST_TRAILER:
            c = data[i++];
            if (++r->trailer_bytes > HTTP_MAX_TRAILER_BYTES) {
                *used = i;
                return resp_fail(r, "chunked trailer too large");
            }
            if (c == '\n') {
                if (r->sizeline_len == 0) {
                    r->state = ST_DONE;
                    *used = i;
                    return HTTP_DONE;
                }
                r->sizeline_len = 0;
            } else if (c != '\r') {
                r->sizeline_len++;
            }
            break;

        default:
            *used = i;
            return HTTP_FAILED;
        }
    }
    *used = i;
    return r->state == ST_DONE ? HTTP_DONE : HTTP_NEED_MORE;
}

int http_resp_eof(HttpResp *r)
{
    if (r->state == ST_DONE) {
        return HTTP_DONE;
    }
    if (r->state == ST_BODY_CLOSE) {
        r->state = ST_DONE;
        return HTTP_DONE;
    }
    if (r->state != ST_ERROR) {
        resp_fail(r, "connection closed before the response was complete");
    }
    return HTTP_FAILED;
}
