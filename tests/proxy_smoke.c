/*
 * proxy_smoke.c -- core/netconn.c's proxy support against real HTTP CONNECT and SOCKS5 proxies (tests/proxy_server.py,
 * written independently in Python): tunnels with and without a login, a wrong login, a forbidden target, a host
 * the proxy cannot find, a port nothing listens on, and a proxy that is not there; the server's first bytes arriving
 * together with the proxy's reply; and that the proxy is the one to resolve the server's name.
 *
 * usage: proxy_smoke SERVER_SCRIPT PYTHON BASE_PORT LOG_FILE
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../core/netconn.h"
#include "../core/rng.h"

static int t_pass, t_fail;
#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static KMs now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (KMs)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
}

/* Pumps until the connection is open, closed or failed (or 10 s pass); returns the state. */
static int settle(NetConn *c)
{
    KMs t0 = now_ms();

    while (KMS_SINCE(now_ms(), t0) < 10000) {
        net_pump(c, now_ms());
        if (c->state == NET_OPEN || c->state == NET_FAILED || c->state == NET_CLOSED) {
            return c->state;
        }
        usleep(2000);
    }
    return c->state;
}

/* waits until at least n bytes can be read */
static int wait_bytes(NetConn *c, size_t n)
{
    KMs t0 = now_ms();
    size_t have;

    while (KMS_SINCE(now_ms(), t0) < 5000) {
        net_pump(c, now_ms());
        net_peek(c, &have);
        if (have >= n) {
            return 1;
        }
        usleep(2000);
    }
    return 0;
}

static void tunnel_ok(const char *proxy_spec, int echo_port, const char *host)
{
    NetConn c;
    NetProxy p;
    char err[200];
    const unsigned char *d;
    size_t n;

    net_init(&c);
    CHECK(net_proxy_parse(&p, proxy_spec, err, sizeof(err)) == 0);
    CHECK(net_open_via(&c, host, echo_port, 0, NULL, NULL, now_ms(), &p) == 0);
    CHECK(settle(&c) == NET_OPEN);
    /* the echo server speaks first, so its banner may have come in with the proxy's reply */
    CHECK(wait_bytes(&c, 16));
    d = net_peek(&c, &n);
    CHECK(n >= 16 && memcmp(d, "hello from echo\n", 16) == 0);
    net_consume(&c, 16);
    CHECK(net_write(&c, "ping 1\n", 7) == 0);
    CHECK(wait_bytes(&c, 7));
    d = net_peek(&c, &n);
    CHECK(n == 7 && memcmp(d, "ping 1\n", 7) == 0);
    net_close(&c);
}

static void tunnel_fails(const char *proxy_spec, const char *host, int port, const char *want)
{
    NetConn c;
    NetProxy p;
    char err[200];

    net_init(&c);
    CHECK(net_proxy_parse(&p, proxy_spec, err, sizeof(err)) == 0);
    if (net_open_via(&c, host, port, 0, NULL, NULL, now_ms(), &p) == 0) {
        settle(&c);
    }
    if (c.state != NET_FAILED || strstr(c.err, want) == NULL) {
        fprintf(stderr, "  expected \"%s\", got state %d \"%s\"\n", want, (int)c.state, c.err);
    }
    CHECK(c.state == NET_FAILED && strstr(c.err, want) != NULL);
    net_close(&c);
}

static int log_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    char line[300];
    int found = 0;

    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, needle) != NULL) {
            found = 1;
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    return found;
}

int main(int argc, char **argv)
{
    pid_t server;
    int base, status;
    char spec[200];

    if (argc != 5) {
        fprintf(stderr, "usage: proxy_smoke SERVER PYTHON BASE LOG\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    base = atoi(argv[3]);
    remove(argv[4]);
    server = fork();
    if (server == 0) {
        execlp(argv[2], argv[2], argv[1], argv[3], argv[4], (char *)NULL);
        _exit(127);
    }
    usleep(1500000);

    /* no proxy at all still works (the echo server on its own) */
    {
        NetConn c;

        net_init(&c);
        CHECK(net_open_via(&c, "127.0.0.1", base + 4, 0, NULL, NULL, now_ms(), NULL) == 0 && settle(&c) == NET_OPEN);
        CHECK(wait_bytes(&c, 16));
        net_close(&c);
    }

    /* the tunnels; the server's name is given as a name, which only the proxy may resolve */
    sprintf(spec, "http://127.0.0.1:%d", base);
    tunnel_ok(spec, base + 4, "localhost");
    sprintf(spec, "http://bob:s3cret@127.0.0.1:%d", base + 1);
    tunnel_ok(spec, base + 4, "localhost");
    sprintf(spec, "socks5://127.0.0.1:%d", base + 2);
    tunnel_ok(spec, base + 4, "localhost");
    sprintf(spec, "socks5://bob:s3cret@127.0.0.1:%d", base + 3);
    tunnel_ok(spec, base + 4, "localhost");
    sprintf(spec, "socks5://127.0.0.1:%d", base + 2);
    tunnel_ok(spec, base + 4, "127.0.0.1");                   /* an address goes as an address */
    sprintf(spec, "http://127.0.0.1:%d", base);
    tunnel_ok(spec, base + 4, "127.0.0.1");

    /* refusals, each with a message that says what happened */
    sprintf(spec, "http://127.0.0.1:%d", base + 1);
    tunnel_fails(spec, "localhost", base + 4, "user name and password");              /* no login given */
    sprintf(spec, "http://bob:wrong@127.0.0.1:%d", base + 1);
    tunnel_fails(spec, "localhost", base + 4, "user name and password");              /* wrong login */
    sprintf(spec, "http://127.0.0.1:%d", base);
    tunnel_fails(spec, "localhost", base + 8, "refused to connect");                  /* forbidden target: 403 */
    tunnel_fails(spec, "nowhere.invalid", 443, "could not reach");                    /* 502 */
    tunnel_fails(spec, "localhost", 1, "could not reach");                            /* nothing listens */
    sprintf(spec, "socks5://127.0.0.1:%d", base + 2);
    tunnel_fails(spec, "localhost", base + 8, "do not allow");                        /* rule-set refusal */
    tunnel_fails(spec, "nowhere.invalid", 443, "cannot find or reach");               /* SOCKS reply 4 */
    tunnel_fails(spec, "localhost", 1, "nothing is listening");                       /* SOCKS reply 5 */
    sprintf(spec, "socks5://127.0.0.1:%d", base + 3);
    tunnel_fails(spec, "localhost", base + 4, "accepts none");                        /* login method offered: none (no login configured) */
    sprintf(spec, "socks5://bob:nope@127.0.0.1:%d", base + 3);
    tunnel_fails(spec, "localhost", base + 4, "did not accept the user name");
    sprintf(spec, "socks5://127.0.0.1:%d", base);                                    /* an HTTP proxy spoken to as SOCKS */
    {
        NetConn c;
        NetProxy p;
        char err[200];
        KMs t0 = now_ms();

        net_init(&c);
        CHECK(net_proxy_parse(&p, spec, err, sizeof(err)) == 0);
        CHECK(net_open_via(&c, "localhost", base + 4, 0, NULL, NULL, t0, &p) == 0);
        while (KMS_SINCE(now_ms(), t0) < 2000 && c.state != NET_FAILED) {
            net_pump(&c, now_ms());
            usleep(2000);
        }
        CHECK(c.state != NET_OPEN);                           /* the HTTP proxy waits for a request that never comes */
        net_close(&c);
    }
    sprintf(spec, "http://127.0.0.1:%d", base + 2);                                  /* a SOCKS proxy spoken to as HTTP */
    {
        NetConn c;
        NetProxy p;
        char err[200];
        KMs t0 = now_ms();

        net_init(&c);
        CHECK(net_proxy_parse(&p, spec, err, sizeof(err)) == 0);
        CHECK(net_open_via(&c, "localhost", base + 4, 0, NULL, NULL, t0, &p) == 0);
        while (KMS_SINCE(now_ms(), t0) < 3000 && c.state != NET_FAILED) {
            net_pump(&c, now_ms());
            usleep(2000);
        }
        /* the SOCKS server waits for a SOCKS greeting that never comes; the CONNECT line is not one, so it closes us
         * or we time out: either way the connection must not become OPEN */
        CHECK(c.state != NET_OPEN);
        net_close(&c);
    }
    tunnel_fails("http://127.0.0.1:1", "localhost", base + 4, "to the proxy failed");  /* no proxy listening */
    tunnel_fails("http://no-such-proxy.invalid:3128", "localhost", base + 4, "Cannot find the proxy");

    kill(server, SIGTERM);
    waitpid(server, &status, 0);

    /* the proxies were asked for the NAME, not an address we had resolved ourselves; logins were seen */
    {
        char b[100];

        sprintf(b, "http-open CONNECT localhost:%d", base + 4);
        CHECK(log_has(argv[4], b));
        sprintf(b, "http-auth CONNECT localhost:%d user=bob", base + 4);
        CHECK(log_has(argv[4], b));
        sprintf(b, "socks-open CONNECT localhost:%d", base + 4);
        CHECK(log_has(argv[4], b));
        sprintf(b, "socks-auth CONNECT localhost:%d user=bob", base + 4);
        CHECK(log_has(argv[4], b));
        sprintf(b, "socks-open CONNECT 127.0.0.1:%d", base + 4);
        CHECK(log_has(argv[4], b));
        CHECK(log_has(argv[4], "http-auth CONNECT localhost") && log_has(argv[4], "denied-login"));
    }
    printf("proxy_smoke: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
