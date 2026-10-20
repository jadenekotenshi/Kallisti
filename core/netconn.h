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

/* A proxy to reach the server through: an HTTP proxy asked to CONNECT, or a SOCKS5 proxy.  TLS (when used) runs
 * end to end through the tunnel, so the proxy sees only the destination's name and port. */
enum { NETPROXY_NONE = 0, NETPROXY_HTTP, NETPROXY_SOCKS5 };

typedef struct {
    int type;
    char host[256];
    int port;
    char user[128];              /* optional login (HTTP Basic, or SOCKS5 user/password) */
    char pass[128];
} NetProxy;

/* Parses "[scheme://][user[:password]@]host[:port]": scheme http (the default; port 3128) or socks5 / socks5h /
 * socks (port 1080); percent escapes in the login are decoded. Empty text means no proxy (type NONE, returns 0).
 * Returns 0, or -1 with a message in err. */
int net_proxy_parse(NetProxy *p, const char *spec, char *err, size_t errcap);

/* The proxy the environment names for `target_host` (https_proxy, HTTPS_PROXY, all_proxy, ALL_PROXY, http_proxy,
 * HTTP_PROXY, in that order), unless no_proxy / NO_PROXY excludes the host. Returns 1 and fills *p, 0 for none,
 * -1 if the variable's value is not a usable proxy (message in err). */
int net_proxy_from_env(NetProxy *p, const char *target_host, char *err, size_t errcap);

typedef enum {
    NET_IDLE = 0,
    NET_CONNECTING,          /* TCP connect in progress */
    NET_TUNNELING,           /* connected to the proxy, asking it to open the way to the server */
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
    NetProxy proxy;
    int tstep;                           /* where the proxy conversation is */
    unsigned char tbuf[1200];            /* its replies so far */
    size_t tlen;
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
/* The same, through `proxy` (NULL or type NONE: directly). The server's name is then resolved by the proxy. */
int net_open_via(NetConn *c, const char *host, int port, int use_tls, const TrustStore *trust, const der_time *now,
                 KMs t, const NetProxy *proxy);
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
