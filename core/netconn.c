/* A polled non-blocking TCP/TLS connection: see netconn.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "netconn.h"
#include "base64.h"
#include "oscompat.h"

#ifdef OPENSTEP
#ifndef O_NONBLOCK
#define O_NONBLOCK O_NDELAY
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
extern int fcntl(int fd, int cmd, ...);
typedef int net_socklen;
#else
typedef socklen_t net_socklen;
#endif

#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long)0xffffffff)
#endif

static void fail(NetConn *c, const char *msg)
{
    if (c->state == NET_FAILED) {
        return;
    }
    c->state = NET_FAILED;
    strncpy(c->err, msg, sizeof(c->err) - 1);
    c->err[sizeof(c->err) - 1] = '\0';
}

static void failf(NetConn *c, const char *what, int err)
{
    char b[240];

    sprintf(b, "%.100s: %.100s", what, strerror(err));
    fail(c, b);
}

void net_init(NetConn *c)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

void net_close(NetConn *c)
{
    if (c->tls != NULL) {
        if (c->state == NET_OPEN && c->fd >= 0) {
            const unsigned char *o;
            size_t n;

            tls_close(c->tls);                       /* close_notify, best effort */
            o = tls_output(c->tls, &n);
            if (n > 0) {
                send(c->fd, (char *)o, n, 0);
            }
        }
        tls_free(c->tls);
    }
    if (c->fd >= 0) {
        close(c->fd);
    }
    free(c->out);
    free(c->in);
    net_init(c);
}

void net_utc_now(der_time *t)
{
    time_t now = time(NULL);
    struct tm *g = gmtime(&now);

    t->year = g->tm_year + 1900;
    t->month = g->tm_mon + 1;
    t->day = g->tm_mday;
    t->hour = g->tm_hour;
    t->min = g->tm_min;
    t->sec = g->tm_sec;
}

static int queue_out(NetConn *c, const unsigned char *p, size_t n)
{
    if (n == 0) {
        return 0;
    }
    if (c->out_len + n > c->out_cap) {
        size_t ncap = c->out_len + n + 4096;
        unsigned char *nb = (unsigned char *)realloc(c->out, ncap);

        if (nb == NULL) {
            fail(c, "out of memory");
            return -1;
        }
        c->out = nb;
        c->out_cap = ncap;
    }
    memcpy(c->out + c->out_len, p, n);
    c->out_len += n;
    return 0;
}

static int queue_in(NetConn *c, const unsigned char *p, size_t n)
{
    if (c->in_off > 0 && c->in_off == c->in_len) {
        c->in_off = c->in_len = 0;
    }
    if ((long)(c->in_len - c->in_off + n) > NET_MAX_BUFFERED) {
        fail(c, "the server sent more data than can be buffered");
        return -1;
    }
    if (c->in_len + n > c->in_cap) {
        size_t ncap;
        unsigned char *nb;

        if (c->in_off > 0) {
            memmove(c->in, c->in + c->in_off, c->in_len - c->in_off);
            c->in_len -= c->in_off;
            c->in_off = 0;
        }
        if (c->in_len + n > c->in_cap) {
            ncap = c->in_len + n + 8192;
            nb = (unsigned char *)realloc(c->in, ncap);
            if (nb == NULL) {
                fail(c, "out of memory");
                return -1;
            }
            c->in = nb;
            c->in_cap = ncap;
        }
    }
    memcpy(c->in + c->in_len, p, n);
    c->in_len += n;
    return 0;
}

/* TLS output (handshake records, encrypted data) goes onto the socket queue. */
static void flush_tls(NetConn *c)
{
    const unsigned char *o;
    size_t n;

    if (c->tls == NULL) {
        return;
    }
    o = tls_output(c->tls, &n);
    if (n > 0) {
        if (queue_out(c, o, n) == 0) {
            tls_output_done(c->tls, n);
        }
    }
}

static void flush_out(NetConn *c)
{
    int w;

    while (c->out_len > 0 && c->state != NET_FAILED) {
        w = send(c->fd, (char *)c->out, c->out_len, 0);
        if (w > 0) {
            memmove(c->out, c->out + w, c->out_len - (size_t)w);
            c->out_len -= (size_t)w;
            c->bytes_out += w;
        } else if (w < 0 && (errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        } else {
            failf(c, "Network error", w < 0 ? errno : EPIPE);
            return;
        }
    }
}

/* ---------------------------------------------------------------- proxy settings */

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* copies src[0..n) into dst with %XX escapes decoded; returns 0, or -1 if it does not fit or is malformed */
static int unescape(char *dst, size_t cap, const char *src, size_t n)
{
    size_t i, o = 0;

    for (i = 0; i < n; i++) {
        int ch = (unsigned char)src[i];

        if (ch == '%') {
            int hi = i + 2 < n ? hexval((unsigned char)src[i + 1]) : -1;
            int lo = i + 2 < n ? hexval((unsigned char)src[i + 2]) : -1;

            if (hi < 0 || lo < 0) return -1;
            ch = hi * 16 + lo;
            i += 2;
        }
        if (o + 1 >= cap) return -1;
        dst[o++] = (char)ch;
    }
    dst[o] = '\0';
    return 0;
}

static void proxy_err(char *err, size_t cap, const char *msg)
{
    if (cap > 0) {
        strncpy(err, msg, cap - 1);
        err[cap - 1] = '\0';
    }
}

int net_proxy_parse(NetProxy *p, const char *spec, char *err, size_t errcap)
{
    const char *s, *at, *colon, *hostp, *slash, *q;
    size_t hl, i, sl0;
    char text[700];

    memset(p, 0, sizeof(*p));
    proxy_err(err, errcap, "");
    while (*spec == ' ' || *spec == '\t') spec++;
    sl0 = strlen(spec);
    while (sl0 > 0 && (spec[sl0 - 1] == ' ' || spec[sl0 - 1] == '\t' || spec[sl0 - 1] == '\n' || spec[sl0 - 1] == '\r')) sl0--;
    if (sl0 == 0) {
        return 0;
    }
    if (sl0 >= sizeof(text)) {
        proxy_err(err, errcap, "the proxy address is too long");
        return -1;
    }
    memcpy(text, spec, sl0);
    text[sl0] = '\0';
    s = text;
    p->type = NETPROXY_HTTP;
    p->port = 3128;
    if ((q = strstr(s, "://")) != NULL) {
        size_t sl = (size_t)(q - s);

        if (sl == 4 && strncmp(s, "http", 4) == 0) {
            ;
        } else if ((sl == 6 && strncmp(s, "socks5", 6) == 0) || (sl == 7 && strncmp(s, "socks5h", 7) == 0) ||
                   (sl == 5 && strncmp(s, "socks", 5) == 0)) {
            p->type = NETPROXY_SOCKS5;
            p->port = 1080;
        } else if (sl == 6 && strncmp(s, "socks4", 6) == 0) {
            p->type = NETPROXY_NONE;
            proxy_err(err, errcap, "SOCKS4 proxies are not supported (use socks5://)");
            return -1;
        } else if (sl == 5 && strncmp(s, "https", 5) == 0) {
            p->type = NETPROXY_NONE;
            proxy_err(err, errcap, "proxies that are themselves reached over TLS are not supported");
            return -1;
        } else {
            p->type = NETPROXY_NONE;
            proxy_err(err, errcap, "the proxy address starts with an unknown scheme (use http:// or socks5://)");
            return -1;
        }
        s = q + 3;
    }
    slash = strchr(s, '/');
    hl = slash ? (size_t)(slash - s) : strlen(s);
    at = NULL;
    for (i = 0; i < hl; i++) {
        if (s[i] == '@') at = s + i;                     /* the last @ before the path ends the login */
    }
    if (at != NULL) {
        size_t ll = (size_t)(at - s);
        const char *c = NULL;

        for (i = 0; i < ll; i++) {
            if (s[i] == ':') {
                c = s + i;
                break;
            }
        }
        if (unescape(p->user, sizeof(p->user), s, c ? (size_t)(c - s) : ll) != 0 ||
            (c != NULL && unescape(p->pass, sizeof(p->pass), c + 1, ll - (size_t)(c - s) - 1) != 0)) {
            memset(p, 0, sizeof(*p));
            proxy_err(err, errcap, "the proxy's user name or password is malformed or too long");
            return -1;
        }
        hostp = at + 1;
        hl -= ll + 1;
    } else {
        hostp = s;
    }
    colon = NULL;
    for (i = 0; i < hl; i++) {
        if (hostp[i] == ':') {
            colon = hostp + i;
        }
    }
    if (colon != NULL) {
        size_t pl = hl - (size_t)(colon - hostp) - 1;
        long port = 0;

        if (pl == 0 || pl > 5) {
            memset(p, 0, sizeof(*p));
            proxy_err(err, errcap, "the proxy's port is not a number");
            return -1;
        }
        for (i = 0; i < pl; i++) {
            if (colon[1 + i] < '0' || colon[1 + i] > '9') {
                memset(p, 0, sizeof(*p));
                proxy_err(err, errcap, "the proxy's port is not a number");
                return -1;
            }
            port = port * 10 + (colon[1 + i] - '0');
        }
        if (port < 1 || port > 65535) {
            memset(p, 0, sizeof(*p));
            proxy_err(err, errcap, "the proxy's port must be between 1 and 65535");
            return -1;
        }
        p->port = (int)port;
        hl = (size_t)(colon - hostp);
    }
    if (hl == 0 || hl >= sizeof(p->host)) {
        memset(p, 0, sizeof(*p));
        proxy_err(err, errcap, "the proxy address has no host name");
        return -1;
    }
    for (i = 0; i < hl; i++) {
        int ch = (unsigned char)hostp[i];

        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_')) {
            memset(p, 0, sizeof(*p));
            proxy_err(err, errcap, "the proxy's host name has characters it cannot have (IPv6 addresses are not supported)");
            return -1;
        }
    }
    memcpy(p->host, hostp, hl);
    p->host[hl] = '\0';
    return 0;
}

static int lc(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
}

/* does a no_proxy list ("localhost, .lan, 10.0.0.1") exclude this host? */
static int no_proxy_matches(const char *list, const char *host)
{
    size_t hl = strlen(host);
    const char *p = list;

    while (*p != '\0') {
        const char *e;
        size_t n;

        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        e = p;
        while (*e != '\0' && *e != ',' && *e != ' ' && *e != '\t') e++;
        n = (size_t)(e - p);
        if (n > 0) {
            size_t i;

            if (n == 1 && *p == '*') {
                return 1;
            }
            if (*p == '.') {
                p++;                                     /* ".lan" and "lan" both mean the domain and its subdomains */
                n--;
            }
            if (n > 0 && n <= hl) {
                int same = 1;

                for (i = 0; i < n; i++) {
                    if (lc((unsigned char)host[hl - n + i]) != lc((unsigned char)p[i])) {
                        same = 0;
                        break;
                    }
                }
                if (same && (n == hl || host[hl - n - 1] == '.')) {
                    return 1;
                }
            }
        }
        p = e;
    }
    return 0;
}

int net_proxy_from_env(NetProxy *p, const char *target_host, char *err, size_t errcap)
{
    static const char *names[] = { "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "http_proxy", "HTTP_PROXY", NULL };
    const char *np = getenv("no_proxy"), *v = NULL;
    int i;

    memset(p, 0, sizeof(*p));
    proxy_err(err, errcap, "");
    if (np == NULL || np[0] == '\0') {
        np = getenv("NO_PROXY");
    }
    if (np != NULL && no_proxy_matches(np, target_host)) {
        return 0;
    }
    for (i = 0; names[i] != NULL; i++) {
        v = getenv(names[i]);
        if (v != NULL && v[0] != '\0') {
            break;
        }
        v = NULL;
    }
    if (v == NULL) {
        return 0;
    }
    if (net_proxy_parse(p, v, err, errcap) != 0) {
        return -1;
    }
    return p->type != NETPROXY_NONE ? 1 : 0;
}

int net_open(NetConn *c, const char *host, int port, int use_tls, const TrustStore *trust, const der_time *now,
             KMs t)
{
    return net_open_via(c, host, port, use_tls, trust, now, t, NULL);
}

int net_open_via(NetConn *c, const char *host, int port, int use_tls, const TrustStore *trust, const der_time *now,
                 KMs t, const NetProxy *proxy)
{
    struct sockaddr_in sa;
    unsigned long addr;
    struct hostent *he;
    int flags, rc;
    const char *dial_host = host;
    int dial_port = port;

    net_close(c);
    if (proxy != NULL && proxy->type != NETPROXY_NONE) {
        c->proxy = *proxy;
        dial_host = c->proxy.host;                     /* we dial the proxy; it resolves and dials the server */
        dial_port = c->proxy.port;
    }
    strncpy(c->host, host, sizeof(c->host) - 1);
    c->port = port;
    c->use_tls = use_tls;
    c->trust = trust;
    c->check_dates = now != NULL;
    if (now != NULL) {
        c->now = *now;
    }
    c->started = t;
    if (use_tls && trust == NULL) {
        fail(c, "no trust store for the TLS connection");
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)dial_port);
    addr = inet_addr(dial_host);
    if (addr != INADDR_NONE) {
        sa.sin_addr.s_addr = addr;
    } else {
        he = gethostbyname(dial_host);
        if (he != NULL) {
            /* Apple's libc hands back a hostent at a misaligned address: copy it before reading its fields */
            struct hostent hc;
            char *first = NULL;

            memcpy(&hc, he, sizeof(hc));
            if (hc.h_addrtype == AF_INET && hc.h_length == 4 && hc.h_addr_list != NULL) {
                memcpy(&first, (const char *)hc.h_addr_list, sizeof(first));
            }
            if (first == NULL) {
                he = NULL;
            } else {
                memcpy(&sa.sin_addr, first, 4);
            }
        }
        if (he == NULL) {
            char b[240];

            sprintf(b, "Cannot find the %s \"%.150s\"", c->proxy.type != NETPROXY_NONE ? "proxy" : "host", dial_host);
            fail(c, b);
            return -1;
        }
    }
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) {
        failf(c, "Cannot create a socket", errno);
        return -1;
    }
    flags = fcntl(c->fd, F_GETFL, 0);
    fcntl(c->fd, F_SETFL, flags | O_NONBLOCK);
    c->state = NET_CONNECTING;
    rc = connect(c->fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0 && errno != EINPROGRESS) {
        failf(c, c->proxy.type != NETPROXY_NONE ? "Connection to the proxy failed" : "Connection failed", errno);
        return -1;
    }
    return 0;
}

static void begin_tunnel(NetConn *c);

/* The connection to the server (directly, or through the proxy's tunnel) is up: start TLS, or just open. */
static void start_session(NetConn *c)
{
    if (!c->use_tls) {
        c->state = NET_OPEN;
        return;
    }
    c->tls = tls_new(c->host);
    if (c->tls == NULL) {
        fail(c, "out of memory");
        return;
    }
    tls_set_trust(c->tls, c->trust, c->check_dates ? &c->now : NULL);
    if (tls_start(c->tls) != 0) {
        fail(c, "Could not start the TLS handshake (the random number generator is not seeded)");
        return;
    }
    c->state = NET_HANDSHAKING;
    flush_tls(c);
}

/* TCP to the first hop is up: ask the proxy for the tunnel, or go straight on. */
static void connected(NetConn *c)
{
    if (c->proxy.type != NETPROXY_NONE) {
        begin_tunnel(c);
    } else {
        start_session(c);
    }
}

/* Handles the events a tls_input() call produced. */
static void tls_events(NetConn *c)
{
    tls_event ev;

    while (c->state != NET_FAILED && tls_next_event(c->tls, &ev)) {
        switch (ev.type) {
        case TLS_EV_HANDSHAKE_DONE:
            c->state = NET_OPEN;
            break;
        case TLS_EV_DATA:
            queue_in(c, ev.data, ev.len);
            break;
        case TLS_EV_ERROR: {
            char b[240];

            sprintf(b, "Secure connection failed: %.180s", ev.text != NULL ? ev.text : "unknown error");
            fail(c, b);
            break;
        }
        case TLS_EV_CLOSED:
            c->state = NET_CLOSED;
            break;
        case TLS_EV_CERT:
            tls_cert_accept(c->tls, 0);              /* cannot happen with a trust store set */
            fail(c, "the server's certificate could not be checked");
            break;
        default:
            break;
        }
    }
}

/* ---------------------------------------------------------------- proxies */

static void queue_str(NetConn *c, const char *text)
{
    queue_out(c, (const unsigned char *)text, strlen(text));
}

static void begin_tunnel(NetConn *c)
{
    c->state = NET_TUNNELING;
    c->tstep = 1;
    c->tlen = 0;
    if (c->proxy.type == NETPROXY_HTTP) {
        char req[700], auth[420], cred[270], b64[420];

        auth[0] = '\0';
        if (c->proxy.user[0] != '\0') {
            sprintf(cred, "%.127s:%.127s", c->proxy.user, c->proxy.pass);
            base64_encode((const unsigned char *)cred, strlen(cred), b64);
            sprintf(auth, "Proxy-Authorization: Basic %s\r\n", b64);
            memset(cred, 0, sizeof(cred));
            memset(b64, 0, sizeof(b64));
        }
        sprintf(req, "CONNECT %.255s:%d HTTP/1.1\r\nHost: %.255s:%d\r\n%sUser-Agent: Kallisti\r\n\r\n", c->host, c->port,
                c->host, c->port, auth);
        queue_str(c, req);
        memset(auth, 0, sizeof(auth));
    } else {
        unsigned char greeting[4];
        size_t n;

        greeting[0] = 5;
        if (c->proxy.user[0] != '\0') {
            greeting[1] = 2;                             /* two methods: none, user/password */
            greeting[2] = 0;
            greeting[3] = 2;
            n = 4;
        } else {
            greeting[1] = 1;
            greeting[2] = 0;
            n = 3;
        }
        queue_out(c, greeting, n);
    }
    flush_out(c);
}

static const char *socks_reason(int rep)
{
    switch (rep) {
    case 1: return "the SOCKS proxy reports a general failure";
    case 2: return "the SOCKS proxy's rules do not allow that connection";
    case 3: return "the SOCKS proxy cannot reach that network";
    case 4: return "the SOCKS proxy cannot find or reach that host";
    case 5: return "the connection was refused (the SOCKS proxy reached the host, but nothing is listening)";
    case 6: return "the SOCKS proxy timed out reaching the host";
    case 7: return "the SOCKS proxy does not support CONNECT";
    case 8: return "the SOCKS proxy does not support that kind of address";
    default: return "the SOCKS proxy refused the connection";
    }
}

/* SOCKS5 CONNECT request for c->host:c->port */
static int socks_send_connect(NetConn *c)
{
    unsigned char req[300];
    size_t n = 0, hl = strlen(c->host);
    unsigned long ip = inet_addr(c->host);

    req[n++] = 5;
    req[n++] = 1;
    req[n++] = 0;
    if (ip != INADDR_NONE) {
        req[n++] = 1;
        memcpy(req + n, &ip, 4);
        n += 4;
    } else {
        if (hl == 0 || hl > 255) {
            fail(c, "the host name is too long for a SOCKS proxy");
            return -1;
        }
        req[n++] = 3;
        req[n++] = (unsigned char)hl;
        memcpy(req + n, c->host, hl);
        n += hl;
    }
    req[n++] = (unsigned char)(c->port >> 8);
    req[n++] = (unsigned char)c->port;
    queue_out(c, req, n);
    flush_out(c);
    return 0;
}

/* Reads and answers the proxy's replies until the tunnel is open (then TLS or plain starts) or it fails. */
static void tunnel_pump(NetConn *c, KMs t)
{
    unsigned char buf[600];
    int n;

    if (KMS_SINCE(t, c->started) > 2 * NET_CONNECT_TIMEOUT_MS) {
        fail(c, "The proxy did not answer in time");
        return;
    }
    flush_out(c);
    if (c->state == NET_FAILED) {
        return;
    }
    n = recv(c->fd, (char *)buf, sizeof(buf), 0);
    if (n == 0) {
        fail(c, "The proxy closed the connection");
        return;
    }
    if (n < 0) {
        if (errno != EWOULDBLOCK && errno != EINTR) {
            failf(c, "Network error", errno);
        }
        return;
    }
    if (c->tlen + (size_t)n > sizeof(c->tbuf)) {
        fail(c, "The proxy sent a reply that is too long");
        return;
    }
    memcpy(c->tbuf + c->tlen, buf, (size_t)n);
    c->tlen += (size_t)n;

    if (c->proxy.type == NETPROXY_HTTP) {
        size_t i, end = 0;
        int status;
        char msg[240];

        for (i = 0; i + 3 < c->tlen; i++) {
            if (c->tbuf[i] == '\r' && c->tbuf[i + 1] == '\n' && c->tbuf[i + 2] == '\r' && c->tbuf[i + 3] == '\n') {
                end = i + 4;
                break;
            }
        }
        if (end == 0) {
            return;                                      /* the header is not complete yet */
        }
        if (end < 12 || memcmp(c->tbuf, "HTTP/1.", 7) != 0) {
            fail(c, "The proxy sent something that is not an HTTP reply");
            return;
        }
        status = (c->tbuf[9] - '0') * 100 + (c->tbuf[10] - '0') * 10 + (c->tbuf[11] - '0');
        if (status < 200 || status > 299) {
            if (status == 407) {
                fail(c, "The proxy wants a user name and password (or did not accept the ones given)");
            } else if (status == 403) {
                sprintf(msg, "The proxy refused to connect to %.150s:%d", c->host, c->port);
                fail(c, msg);
            } else if (status == 502 || status == 503 || status == 504) {
                sprintf(msg, "The proxy could not reach %.150s (HTTP %d)", c->host, status);
                fail(c, msg);
            } else {
                sprintf(msg, "The proxy answered HTTP %d to the request for a tunnel", status);
                fail(c, msg);
            }
            return;
        }
        if (c->tlen > end && !c->use_tls) {
            queue_in(c, c->tbuf + end, c->tlen - end);   /* the server spoke first (a plain protocol) */
        }
        c->tlen = 0;
        start_session(c);
        return;
    }

    /* SOCKS5 */
    if (c->tstep == 1) {                                 /* the method the proxy chose */
        if (c->tlen < 2) return;
        if (c->tbuf[0] != 5) {
            fail(c, "The proxy is not a SOCKS5 proxy");
            return;
        }
        if (c->tbuf[1] == 0xff) {
            fail(c, "The SOCKS proxy accepts none of the ways of logging in that were offered");
            return;
        }
        c->tlen = 0;
        if (c->tbuf[1] == 2) {
            unsigned char req[300];
            size_t ul = strlen(c->proxy.user), pl = strlen(c->proxy.pass), k = 0;

            if (ul == 0 || ul > 255 || pl > 255) {
                fail(c, "The SOCKS proxy wants a user name and password");
                return;
            }
            req[k++] = 1;
            req[k++] = (unsigned char)ul;
            memcpy(req + k, c->proxy.user, ul);
            k += ul;
            req[k++] = (unsigned char)pl;
            memcpy(req + k, c->proxy.pass, pl);
            k += pl;
            queue_out(c, req, k);
            memset(req, 0, sizeof(req));
            flush_out(c);
            c->tstep = 2;
        } else if (c->tbuf[1] == 0) {
            c->tstep = 3;
            socks_send_connect(c);
        } else {
            fail(c, "The SOCKS proxy chose a way of logging in that Kallisti does not know");
        }
        return;
    }
    if (c->tstep == 2) {                                 /* the login's verdict */
        if (c->tlen < 2) return;
        if (c->tbuf[1] != 0) {
            fail(c, "The SOCKS proxy did not accept the user name and password");
            return;
        }
        c->tlen = 0;
        c->tstep = 3;
        socks_send_connect(c);
        return;
    }
    {                                                    /* step 3: the reply to CONNECT */
        size_t need;

        if (c->tlen < 2) return;
        if (c->tbuf[0] != 5) {
            fail(c, "The SOCKS proxy sent a reply Kallisti cannot read");
            return;
        }
        if (c->tbuf[1] != 0) {
            char msg[240];

            sprintf(msg, "Cannot reach %.150s:%d: %s", c->host, c->port, socks_reason(c->tbuf[1]));
            fail(c, msg);
            return;
        }
        if (c->tlen < 5) return;
        need = c->tbuf[3] == 1 ? 10 : (c->tbuf[3] == 4 ? 22 : (c->tbuf[3] == 3 ? 7 + (size_t)c->tbuf[4] : 0));
        if (need == 0) {
            fail(c, "The SOCKS proxy sent a reply Kallisti cannot read");
            return;
        }
        if (c->tlen < need) return;
        if (c->tlen > need && !c->use_tls) {
            queue_in(c, c->tbuf + need, c->tlen - need);
        }
        c->tlen = 0;
        start_session(c);
    }
}

void net_pump(NetConn *c, KMs t)
{
    unsigned char buf[8192];
    int i, n;

    if (c->state == NET_CONNECTING) {
        fd_set wf;
        struct timeval tv;
        int err = 0;
        net_socklen len = sizeof(err);

        FD_ZERO(&wf);
        FD_SET(c->fd, &wf);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select(c->fd + 1, NULL, &wf, NULL, &tv) > 0) {
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            if (err != 0) {
                failf(c, c->proxy.type != NETPROXY_NONE ? "Connection to the proxy failed" : "Connection failed", err);
            } else {
                connected(c);
            }
        } else if (KMS_SINCE(t, c->started) > NET_CONNECT_TIMEOUT_MS) {
            fail(c, c->proxy.type != NETPROXY_NONE ? "Connection to the proxy timed out" : "Connection timed out");
        }
        return;
    }
    if (c->state == NET_TUNNELING) {
        tunnel_pump(c, t);
        return;
    }
    if (c->state != NET_HANDSHAKING && c->state != NET_OPEN) {
        return;
    }
    if (c->state == NET_HANDSHAKING && KMS_SINCE(t, c->started) > 2 * NET_CONNECT_TIMEOUT_MS) {
        fail(c, "The secure connection could not be set up in time");
        return;
    }
    flush_out(c);
    for (i = 0; i < 8 && (c->state == NET_HANDSHAKING || c->state == NET_OPEN); i++) {
        n = recv(c->fd, (char *)buf, sizeof(buf), 0);
        if (n > 0) {
            c->bytes_in += n;
            if (c->use_tls) {
                if (tls_input(c->tls, buf, (size_t)n) != 0) {
                    tls_events(c);
                    if (c->state != NET_FAILED) {
                        fail(c, "Secure connection failed");
                    }
                    break;
                }
                tls_events(c);
                flush_tls(c);
            } else if (queue_in(c, buf, (size_t)n) != 0) {
                break;
            }
        } else if (n == 0) {
            if (c->state != NET_FAILED) {
                c->state = NET_CLOSED;
            }
            break;
        } else if (errno == EWOULDBLOCK || errno == EINTR) {
            break;
        } else {
            failf(c, "Network error", errno);
            break;
        }
    }
    if (c->state == NET_HANDSHAKING || c->state == NET_OPEN) {
        flush_out(c);
    } else if (c->state == NET_FAILED || c->state == NET_CLOSED) {
        flush_out(c);
    }
}

int net_write(NetConn *c, const void *data, size_t len)
{
    if (c->state != NET_OPEN) {
        return -1;
    }
    if (c->use_tls) {
        if (tls_write(c->tls, (const unsigned char *)data, len) != 0) {
            return -1;
        }
        flush_tls(c);
    } else if (queue_out(c, (const unsigned char *)data, len) != 0) {
        return -1;
    }
    flush_out(c);
    return c->state == NET_FAILED ? -1 : 0;
}

const unsigned char *net_peek(const NetConn *c, size_t *len)
{
    *len = c->in_len - c->in_off;
    return c->in + c->in_off;
}

void net_consume(NetConn *c, size_t n)
{
    c->in_off += n;
    if (c->in_off >= c->in_len) {
        c->in_off = c->in_len = 0;
    }
}
