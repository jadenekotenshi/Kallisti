/*
 * netconn.h -- one non-blocking TCP connection, optionally carrying the vendored TLS engine with certificate
 * validation, driven by polling: net_open() starts it, net_pump() is called every tick (the app's 20 ms
 * NSTimer, a test's loop) and does whatever can be done without waiting -- finishing the connect, running the
 * TLS handshake, reading, writing.  The application reads decrypted bytes with net_peek()/net_consume() and
 * queues bytes to send with net_write().
 *
 * Name resolution (gethostbyname, IPv4 only) happens inside net_open() and may block briefly.
 */
#ifndef KALLISTI_NETCONN_H
#define KALLISTI_NETCONN_H

#include <stddef.h>
#include "kms.h"
#include "chain.h"
#include "tls.h"

typedef enum {
    NET_IDLE = 0,
    NET_CONNECTING,          /* TCP connect in progress */
    NET_HANDSHAKING,         /* TLS handshake in progress */
    NET_OPEN,                /* ready for data */
    NET_CLOSED,              /* the peer closed it cleanly (unread data may remain) */
    NET_FAILED               /* error: see err */
} NetState;

#define NET_CONNECT_TIMEOUT_MS 30000
#define NET_MAX_BUFFERED (8L * 1024 * 1024)

typedef struct {
    int fd;
    NetState state;
    int use_tls;
    tls_session *tls;
    char host[256];
    int port;
    const TrustStore *trust;
    int check_dates;
    der_time now;
    KMs started;
    unsigned char *out;                  /* bytes waiting for send() */
    size_t out_len, out_cap;
    unsigned char *in;                   /* plaintext waiting for the application */
    size_t in_len, in_cap, in_off;
    char err[240];
    long bytes_in, bytes_out;
} NetConn;

void net_init(NetConn *c);
/* Closes the socket (with a TLS close_notify if it was open) and frees everything; the NetConn can be
 * net_open()ed again. */
void net_close(NetConn *c);

/* Starts a connection.  `trust` is required for TLS and must outlive the connection; `now` is the current
 * UTC time for the certificate date checks (NULL: do not check dates).  Returns 0, or -1 with err set
 * (state NET_FAILED) if the name cannot be resolved or a socket cannot be made. */
int net_open(NetConn *c, const char *host, int port, int use_tls, const TrustStore *trust, const der_time *now,
             KMs t);
/* Makes whatever progress is possible without blocking. */
void net_pump(NetConn *c, KMs t);

/* Queues bytes for the peer (encrypting them if TLS).  Only while NET_OPEN; returns 0 or -1. */
int net_write(NetConn *c, const void *data, size_t len);
/* Bytes received and not yet consumed. */
const unsigned char *net_peek(const NetConn *c, size_t *len);
void net_consume(NetConn *c, size_t n);

/* The current UTC time as the certificate checks want it. */
void net_utc_now(der_time *t);

#endif
