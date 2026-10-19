/*
 * smoke_conn.h -- the socket and TLS plumbing shared by the smoke tests: a Conn that is either a plain
 * TCP socket or a TCP socket carrying the vendored TLS 1.2 engine, with open/send/recv/close.
 */
#ifndef SMOKE_CONN_H
#define SMOKE_CONN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include "../core/tls.h"
#include "../core/rng.h"

typedef struct {
    int fd;
    tls_session *tls;        /* NULL for plaintext */
    unsigned char *pending;  /* decrypted bytes not yet handed to the caller */
    size_t npending, cap;
    int closed;
} Conn;

static void seed_rng(void)
{
    if (ssh_rng_ready()) {
        return;
    }
    if (ssh_rng_seed_system() == 0) {
        ssh_rng_add("http smoke", 10, 256);
    }
}

static int connect_retrying(int port)
{
    int i;

    for (i = 0; i < 80; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        struct timeval tv;

        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons((unsigned short)port);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            tv.tv_sec = 10;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return fd;
        }
        close(fd);
        usleep(100000);
    }
    return -1;
}

static int send_all(int fd, const unsigned char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);

        if (w <= 0) {
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int tls_flush(Conn *c)
{
    const u8 *out;
    size_t len;

    out = tls_output(c->tls, &len);
    if (len > 0) {
        if (send_all(c->fd, out, len) != 0) {
            return -1;
        }
        tls_output_done(c->tls, len);
    }
    return 0;
}

static void pend(Conn *c, const unsigned char *d, size_t n)
{
    if (c->npending + n > c->cap) {
        c->cap = (c->npending + n) * 2;
        c->pending = (unsigned char *)realloc(c->pending, c->cap);
    }
    memcpy(c->pending + c->npending, d, n);
    c->npending += n;
}

/* Handles every queued TLS event: returns 0, or -1 on a fatal one. */
static int tls_events(Conn *c, int *handshake_done)
{
    tls_event ev;

    while (tls_next_event(c->tls, &ev)) {
        if (ev.type == TLS_EV_CERT) {
            tls_cert_accept(c->tls, 1);
        } else if (ev.type == TLS_EV_HANDSHAKE_DONE) {
            if (handshake_done) {
                *handshake_done = 1;
            }
        } else if (ev.type == TLS_EV_DATA) {
            pend(c, ev.data, ev.len);
        } else if (ev.type == TLS_EV_CLOSED) {
            c->closed = 1;
        } else if (ev.type == TLS_EV_ERROR) {
            fprintf(stderr, "TLS error: %s\n", ev.text);
            return -1;
        }
    }
    return tls_flush(c);
}

static int conn_open(Conn *c, int port, int use_tls)
{
    memset(c, 0, sizeof(*c));
    c->fd = connect_retrying(port);
    if (c->fd < 0) {
        return -1;
    }
    if (!use_tls) {
        return 0;
    }
    c->tls = tls_new("localhost");
    if (tls_start(c->tls) != 0 || tls_flush(c) != 0) {
        return -1;
    }
    {
        int done = 0;
        unsigned char buf[8192];

        while (!done) {
            ssize_t n = recv(c->fd, buf, sizeof(buf), 0);

            if (n <= 0) {
                return -1;
            }
            if (tls_input(c->tls, buf, (size_t)n) != 0) {
                tls_events(c, NULL);
                return -1;
            }
            if (tls_events(c, &done) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int conn_send(Conn *c, const unsigned char *d, size_t n)
{
    if (c->tls == NULL) {
        return send_all(c->fd, d, n);
    }
    if (tls_write(c->tls, d, n) != 0) {
        return -1;
    }
    return tls_flush(c);
}

/* Hands over up to n plaintext bytes: >0 bytes, 0 at a clean close, -1 on error. */
static long conn_recv(Conn *c, unsigned char *buf, size_t n)
{
    unsigned char raw[16384];
    ssize_t r;

    for (;;) {
        if (c->npending > 0) {
            size_t k = c->npending < n ? c->npending : n;

            memcpy(buf, c->pending, k);
            memmove(c->pending, c->pending + k, c->npending - k);
            c->npending -= k;
            return (long)k;
        }
        if (c->tls != NULL && c->closed) {
            return 0;
        }
        r = recv(c->fd, raw, sizeof(raw), 0);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;
        }
        if (c->tls == NULL) {
            pend(c, raw, (size_t)r);
        } else {
            if (tls_input(c->tls, raw, (size_t)r) != 0) {
                tls_events(c, NULL);
                return -1;
            }
            if (tls_events(c, NULL) != 0) {
                return -1;
            }
        }
    }
}

/* Waits up to timeout_ms for something to read: 1 if conn_recv() will not block, 0 on timeout. */
static int conn_wait(Conn *c, int timeout_ms)
{
    struct pollfd p;

    if (c->npending > 0) {
        return 1;
    }
    p.fd = c->fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, timeout_ms) > 0 ? 1 : 0;
}

static void conn_close(Conn *c)
{
    if (c->tls != NULL) {
        tls_free(c->tls);
    }
    close(c->fd);
    free(c->pending);
}


#endif
