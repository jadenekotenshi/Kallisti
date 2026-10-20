#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/netconn.h"
#include "test.h"

static int parse(const char *spec, NetProxy *p, char *err)
{
    return net_proxy_parse(p, spec, err, 200);
}

static void test_parse(void)
{
    NetProxy p;
    char err[200];

    CHECK(parse("", &p, err) == 0 && p.type == NETPROXY_NONE);
    CHECK(parse("   ", &p, err) == 0 && p.type == NETPROXY_NONE);
    CHECK(parse("proxy.lan", &p, err) == 0 && p.type == NETPROXY_HTTP && strcmp(p.host, "proxy.lan") == 0 && p.port == 3128 &&
          p.user[0] == '\0');
    CHECK(parse("proxy.lan:8080", &p, err) == 0 && p.type == NETPROXY_HTTP && p.port == 8080);
    CHECK(parse("http://proxy.lan:3128/", &p, err) == 0 && p.type == NETPROXY_HTTP && strcmp(p.host, "proxy.lan") == 0 && p.port == 3128);
    CHECK(parse("  http://10.0.0.5  ", &p, err) == 0 && strcmp(p.host, "10.0.0.5") == 0);
    CHECK(parse("socks5://127.0.0.1:9050", &p, err) == 0 && p.type == NETPROXY_SOCKS5 && p.port == 9050);
    CHECK(parse("socks5://tor", &p, err) == 0 && p.type == NETPROXY_SOCKS5 && p.port == 1080);
    CHECK(parse("socks5h://tor:1", &p, err) == 0 && p.type == NETPROXY_SOCKS5 && p.port == 1);
    CHECK(parse("socks://tor", &p, err) == 0 && p.type == NETPROXY_SOCKS5);
    CHECK(parse("http://bob:secret@proxy.lan:3128", &p, err) == 0 && strcmp(p.user, "bob") == 0 && strcmp(p.pass, "secret") == 0 &&
          strcmp(p.host, "proxy.lan") == 0 && p.port == 3128);
    CHECK(parse("bob@proxy.lan", &p, err) == 0 && strcmp(p.user, "bob") == 0 && p.pass[0] == '\0');
    CHECK(parse("socks5://a%40b:p%3Aw%2Fd@h:2", &p, err) == 0 && strcmp(p.user, "a@b") == 0 && strcmp(p.pass, "p:w/d") == 0 && p.port == 2);
    CHECK(parse("http://u:p@w@host:9", &p, err) == 0 && strcmp(p.user, "u") == 0 && strcmp(p.pass, "p@w") == 0 && strcmp(p.host, "host") == 0);

    CHECK(parse("socks4://x", &p, err) == -1 && strstr(err, "SOCKS4") != NULL && p.type == NETPROXY_NONE);
    CHECK(parse("https://x", &p, err) == -1 && strstr(err, "TLS") != NULL);
    CHECK(parse("ftp://x", &p, err) == -1 && strstr(err, "scheme") != NULL);
    CHECK(parse("host:0", &p, err) == -1 && parse("host:65536", &p, err) == -1 && parse("host:abc", &p, err) == -1 &&
          parse("host:", &p, err) == -1 && parse("host:123456", &p, err) == -1);
    CHECK(parse("http://:80", &p, err) == -1 && strstr(err, "host") != NULL);
    CHECK(parse("http://[::1]:80", &p, err) == -1);
    CHECK(parse("a b:80", &p, err) == -1);
    CHECK(parse("u%zz@host", &p, err) == -1 && parse("u%4@host", &p, err) == -1 && parse("u%", &p, err) == -1);
    {
        char big[600];

        memset(big, 'a', sizeof(big));
        big[300] = '\0';
        CHECK(parse(big, &p, err) == -1);                       /* a host name too long to keep */
        strcpy(big, "http://");
        memset(big + 7, 'u', 200);
        strcpy(big + 207, "@host");
        CHECK(parse(big, &p, err) == -1 && p.type == NETPROXY_NONE);   /* a user name too long to keep */
    }
}

static void clear_env(void)
{
    static const char *names[] = { "https_proxy=", "HTTPS_PROXY=", "all_proxy=", "ALL_PROXY=", "http_proxy=", "HTTP_PROXY=",
                                   "no_proxy=", "NO_PROXY=", NULL };
    int i;

    for (i = 0; names[i] != NULL; i++) {
        putenv((char *)names[i]);
    }
}

static void test_env(void)
{
    NetProxy p;
    char err[200];
    static char v1[] = "http_proxy=http://plain.lan:1111";
    static char v2[] = "HTTP_PROXY=http://upper.lan:2222";
    static char v3[] = "all_proxy=socks5://all.lan:3333";
    static char v4[] = "https_proxy=http://secure.lan:4444";
    static char np1[] = "no_proxy=localhost, .lan ,10.0.0.1";
    static char np2[] = "NO_PROXY=*";
    static char bad[] = "https_proxy=socks4://x";

    clear_env();
    CHECK(net_proxy_from_env(&p, "discord.com", err, sizeof(err)) == 0 && p.type == NETPROXY_NONE);
    putenv(v2);
    CHECK(net_proxy_from_env(&p, "discord.com", err, sizeof(err)) == 1 && strcmp(p.host, "upper.lan") == 0 && p.port == 2222);
    putenv(v1);
    CHECK(net_proxy_from_env(&p, "discord.com", err, sizeof(err)) == 1 && strcmp(p.host, "plain.lan") == 0);      /* lower case first */
    putenv(v3);
    CHECK(net_proxy_from_env(&p, "discord.com", err, sizeof(err)) == 1 && p.type == NETPROXY_SOCKS5 && strcmp(p.host, "all.lan") == 0);
    putenv(v4);
    CHECK(net_proxy_from_env(&p, "discord.com", err, sizeof(err)) == 1 && strcmp(p.host, "secure.lan") == 0);    /* https_proxy wins */
    putenv(np1);
    CHECK(net_proxy_from_env(&p, "discord.com", err, sizeof(err)) == 1);
    CHECK(net_proxy_from_env(&p, "localhost", err, sizeof(err)) == 0 && p.type == NETPROXY_NONE);
    CHECK(net_proxy_from_env(&p, "LOCALHOST", err, sizeof(err)) == 0);
    CHECK(net_proxy_from_env(&p, "files.lan", err, sizeof(err)) == 0 && net_proxy_from_env(&p, "a.b.lan", err, sizeof(err)) == 0);
    CHECK(net_proxy_from_env(&p, "lan", err, sizeof(err)) == 0);
    CHECK(net_proxy_from_env(&p, "10.0.0.1", err, sizeof(err)) == 0);
    CHECK(net_proxy_from_env(&p, "10.0.0.11", err, sizeof(err)) == 1);                    /* not a suffix match of "10.0.0.1" */
    CHECK(net_proxy_from_env(&p, "notlocalhost", err, sizeof(err)) == 1);                 /* a domain match needs a dot boundary */
    CHECK(net_proxy_from_env(&p, "wlan", err, sizeof(err)) == 1);
    putenv((char *)"no_proxy=");
    putenv(np2);
    CHECK(net_proxy_from_env(&p, "discord.com", err, sizeof(err)) == 0);                  /* "*" (read from NO_PROXY) */
    clear_env();
    putenv(bad);
    CHECK(net_proxy_from_env(&p, "discord.com", err, sizeof(err)) == -1 && strstr(err, "SOCKS4") != NULL);
    clear_env();
}

int main(void)
{
    test_parse();
    test_env();
    TEST_DONE("netproxy");
}
