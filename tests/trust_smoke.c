/*
 * trust_smoke.c -- the TLS engine's certificate validation against a real TLS server (Python's ssl, using
 * tests/http_smoke_server.py): a throwaway CA -> intermediate -> leaf chain for "localhost".  The trusting
 * store must complete the handshake; an empty store, another CA, the wrong host name and an expired clock
 * must each end it with a message that says why.
 *
 * usage: trust_smoke SERVER_SCRIPT PORT CHAIN_PEM KEY_PEM CA_PEM OTHER_CA_PEM
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../core/chain.h"
#include "smoke_conn.h"

static int t_pass, t_fail;
#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int load_store(TrustStore *ts, const char *path)
{
    FILE *f = fopen(path, "rb");
    static char buf[16384];
    size_t n;

    trust_init(ts);
    if (f == NULL) {
        return -1;
    }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return trust_add_pem(ts, buf, n);
}

/* 0 if the handshake completed */
static int attempt(int port, const char *host, const TrustStore *ts, const der_time *now)
{
    Conn c;
    int rc;

    smoke_trust = ts;
    smoke_host = host;
    smoke_check_dates = now != NULL;
    if (now != NULL) {
        smoke_now = *now;
    }
    smoke_error[0] = '\0';
    rc = conn_open(&c, port, 1);
    if (rc == 0) {
        conn_close(&c);
    } else if (c.fd > 0) {
        close(c.fd);
    }
    return rc;
}

int main(int argc, char **argv)
{
    pid_t server;
    int port, status;
    TrustStore good, other, empty;
    der_time future = { 2100, 1, 1, 0, 0, 0 }, today = { 2026, 10, 19, 18, 0, 0 };

    if (argc != 7) {
        fprintf(stderr, "usage: trust_smoke SERVER PORT CHAIN KEY CA OTHER_CA\n");
        return 2;
    }
    seed_rng();
    signal(SIGPIPE, SIG_IGN);
    port = atoi(argv[2]);
    server = fork();
    if (server == 0) {
        execlp("python3", "python3", argv[1], argv[2], argv[3], argv[4], (char *)NULL);
        _exit(127);
    }
    CHECK(load_store(&good, argv[5]) == 1);
    CHECK(load_store(&other, argv[6]) == 1);
    trust_init(&empty);

    /* the leaf was made "today" by the setup script, so a date in 2026 or later is fine until it expires */
    CHECK(attempt(port, "localhost", &good, NULL) == 0);
    {
        time_t t = time(NULL);
        struct tm *g = gmtime(&t);
        der_time now;

        now.year = g->tm_year + 1900; now.month = g->tm_mon + 1; now.day = g->tm_mday;
        now.hour = g->tm_hour; now.min = g->tm_min; now.sec = g->tm_sec;
        CHECK(attempt(port, "localhost", &good, &now) == 0);
    }
    CHECK(attempt(port, "localhost", &empty, NULL) != 0 && strstr(smoke_error, "trusted authority") != NULL);
    CHECK(attempt(port, "localhost", &other, NULL) != 0 && strstr(smoke_error, "trusted authority") != NULL);
    CHECK(attempt(port, "wrong.example", &good, NULL) != 0 && strstr(smoke_error, "wrong.example") != NULL);
    CHECK(attempt(port, "localhost", &good, &future) != 0 && strstr(smoke_error, "expired") != NULL);
    (void)today;
    /* and a good one again, to show the failures left the server healthy */
    CHECK(attempt(port, "localhost", &good, NULL) == 0);

    kill(server, SIGTERM);
    waitpid(server, &status, 0);
    trust_free(&good);
    trust_free(&other);
    printf("trust_smoke: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
