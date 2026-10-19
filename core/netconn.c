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

int net_open(NetConn *c, const char *host, int port, int use_tls, const TrustStore *trust, const der_time *now,
             KMs t)
{
    struct sockaddr_in sa;
    unsigned long addr;
    struct hostent *he;
    int flags, rc;

    net_close(c);
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
    sa.sin_port = htons((unsigned short)port);
    addr = inet_addr(host);
    if (addr != INADDR_NONE) {
        sa.sin_addr.s_addr = addr;
    } else {
        he = gethostbyname(host);
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

            sprintf(b, "Cannot find the host \"%.150s\"", host);
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
        failf(c, "Connection failed", errno);
        return -1;
    }
    return 0;
}

static void connected(NetConn *c)
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
                failf(c, "Connection failed", err);
            } else {
                connected(c);
            }
        } else if (KMS_SINCE(t, c->started) > NET_CONNECT_TIMEOUT_MS) {
            fail(c, "Connection timed out");
        }
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
