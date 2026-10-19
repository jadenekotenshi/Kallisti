#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/rest.h"
#include "test.h"

/* ---- a reply as the HTTP parser would hand it over ---- */
static void reply(HttpResp *r, int status, const char *headers, const char *body)
{
    char raw[8192];
    size_t used;

    sprintf(raw, "HTTP/1.1 %d X\r\nContent-Length: %lu\r\n%s\r\n%s", status, (unsigned long)strlen(body),
            headers, body);
    http_resp_init(r, REST_MAX_RESPONSE, 0);
    CHECK(http_resp_feed(r, (const unsigned char *)raw, strlen(raw), &used) == HTTP_DONE);
}

/* Sends the next request (if any), returns its id or 0. The request text is left in *text. */
static long next_req(RestClient *rc, KMs now, char *text, size_t cap)
{
    HttpReq q;
    long id = 0;

    if (!rest_next_request(rc, now, &q, &id)) {
        return 0;
    }
    if (text != NULL) {
        size_t n = q.len < cap - 1 ? q.len : cap - 1;

        memcpy(text, q.buf, n);
        text[n] = '\0';
    }
    http_req_free(&q);
    return id;
}

static long answer(RestClient *rc, KMs now, int status, const char *headers, const char *body)
{
    HttpResp r;

    reply(&r, status, headers, body);
    rest_response(rc, now, &r);
    http_resp_free(&r);
    return 0;
}

static void test_queueing(void)
{
    RestClient rc;
    char big[9000];
    int i;
    long id;

    rest_init(&rc, "tok", 1);
    CHECK(rest_get_channels(&rc, "123456789012345678") > 0);
    CHECK(rest_get_channels(&rc, "") == REST_E_BAD);
    CHECK(rest_get_channels(&rc, NULL) == REST_E_BAD);
    CHECK(rest_get_channels(&rc, "12a") == REST_E_BAD);
    CHECK(rest_get_channels(&rc, "../../users/@me") == REST_E_BAD);
    CHECK(rest_get_channels(&rc, "123456789012345678901") == REST_E_BAD);     /* 21 digits */
    CHECK(rest_get_messages(&rc, "55", 0, NULL) == REST_E_BAD);
    CHECK(rest_get_messages(&rc, "55", 101, NULL) == REST_E_BAD);
    CHECK(rest_get_messages(&rc, "55", 50, "x") == REST_E_BAD);
    CHECK(rest_get_messages(&rc, "55", 100, "77") > 0);
    CHECK(rest_post_message(&rc, "55", NULL) == REST_E_BAD);
    CHECK(rest_post_message(&rc, "55", "") == REST_E_BAD);
    CHECK(rest_post_message(&rc, "55", " \t\r\n ") == REST_E_BAD);
    CHECK(rest_post_message(&rc, "55", "bad \xff utf8") == REST_E_BAD);
    CHECK(rest_post_message(&rc, "55", "bad \xC0\x80 overlong") == REST_E_BAD);
    CHECK(rest_post_message(&rc, "x", "hi") == REST_E_BAD);
    memset(big, 'a', 2000);
    big[2000] = '\0';
    CHECK(rest_post_message(&rc, "55", big) > 0);
    big[2000] = 'a';
    big[2001] = '\0';
    CHECK(rest_post_message(&rc, "55", big) == REST_E_TOO_LONG);
    /* the limit counts characters, not bytes: 2000 snowmen are 6000 bytes */
    for (i = 0; i < 2000; i++) {
        memcpy(big + i * 3, "\xE2\x98\x83", 3);
    }
    big[6000] = '\0';
    CHECK(rest_post_message(&rc, "55", big) > 0);
    memcpy(big + 6000, "\xE2\x98\x83", 4);
    CHECK(rest_post_message(&rc, "55", big) == REST_E_TOO_LONG);
    rest_free(&rc);

    /* the queue is bounded */
    rest_init(&rc, "tok", 1);
    for (i = 0; i < REST_MAX_JOBS; i++) {
        id = rest_get_guilds(&rc);
        CHECK(id == i + 1);
    }
    CHECK(rest_get_guilds(&rc) == REST_E_FULL);
    CHECK(rest_pending(&rc) == REST_MAX_JOBS);
    rest_free(&rc);
}

#define STARTS(t, lit) (strncmp((t), (lit), sizeof(lit) - 1) == 0)

static int has(const char *text, const char *needle)
{
    return strstr(text, needle) != NULL;
}

static void test_requests(void)
{
    RestClient rc;
    char t[2048];
    long id;
    RestResult res;

    rest_init(&rc, "abc.def", 1);
    rest_get_channels(&rc, "123");
    rest_post_message(&rc, "456", "hi \"there\"\n\xE2\x98\x83");
    rest_get_messages(&rc, "456", 25, "789");

    CHECK(next_req(&rc, 1000, t, sizeof(t)) == 1);
    CHECK(STARTS(t, "GET /api/v10/guilds/123/channels HTTP/1.1\r\n"));
    CHECK(has(t, "Host: discord.com\r\n"));
    CHECK(has(t, "Authorization: Bot abc.def\r\n"));
    CHECK(has(t, "User-Agent: DiscordBot (https://github.com/jadenekotenshi/Kallisti, 0.1)\r\n"));
    CHECK(has(t, "Accept: application/json\r\n"));
    CHECK(has(t, "Accept-Encoding: identity\r\n"));
    CHECK(!has(t, "Content-Type"));
    /* one at a time */
    CHECK(next_req(&rc, 1000, t, sizeof(t)) == 0);
    CHECK(rest_next_deadline(&rc, 1000) == -1);
    answer(&rc, 1000, 200, "Content-Type: application/json\r\n", "[{\"id\":\"1\",\"name\":\"general\",\"type\":0}]");
    CHECK(rest_next_result(&rc, &res));
    CHECK(res.id == 1 && res.kind == REST_CHANNELS && res.status == 200 && res.ok && res.error[0] == '\0');
    CHECK(res.body != NULL && res.body->type == J_ARR && strcmp(json_get_str(json_at(res.body, 0), "name", ""), "general") == 0);
    CHECK(!rest_next_result(&rc, &res));

    CHECK(next_req(&rc, 1000, t, sizeof(t)) == 2);
    CHECK(STARTS(t, "POST /api/v10/channels/456/messages HTTP/1.1\r\n"));
    CHECK(has(t, "Content-Type: application/json\r\n"));
    CHECK(has(t, "\r\n\r\n{\"content\":\"hi \\\"there\\\"\\n\xE2\x98\x83\"}"));
    CHECK(has(t, "Content-Length: 31\r\n"));
    answer(&rc, 1000, 200, "", "{\"id\":\"900\",\"content\":\"x\"}");
    CHECK(rest_next_result(&rc, &res) && res.id == 2 && res.ok && res.kind == REST_POST_MESSAGE);

    CHECK(next_req(&rc, 1000, t, sizeof(t)) == 3);
    CHECK(STARTS(t, "GET /api/v10/channels/456/messages?limit=25&before=789 HTTP/1.1\r\n"));
    answer(&rc, 1000, 200, "", "[]");
    CHECK(rest_next_result(&rc, &res) && res.ok && res.body != NULL && res.body->count == 0);
    CHECK(rest_pending(&rc) == 0);
    (void)id;
    rest_free(&rc);

    /* a user token goes out bare; no body, no Content-Type */
    rest_init(&rc, "user.token.xyz", 0);
    rest_get_me(&rc);
    CHECK(next_req(&rc, 0, t, sizeof(t)) == 1);
    CHECK(has(t, "Authorization: user.token.xyz\r\n") && !has(t, "Authorization: Bot"));
    CHECK(STARTS(t, "GET /api/v10/users/@me HTTP/1.1"));
    rest_free(&rc);

    /* a configured host and port */
    rest_init(&rc, "t", 1);
    strcpy(rc.host, "localhost");
    rc.port = 8443;
    rest_get_gateway_bot(&rc);
    CHECK(next_req(&rc, 0, t, sizeof(t)) == 1 && has(t, "Host: localhost:8443\r\n"));
    CHECK(STARTS(t, "GET /api/v10/gateway/bot "));
    rest_free(&rc);
}

static void test_create_dm(void)
{
    RestClient rc;
    RestResult res;
    char t[2048];
    long id;

    rest_init(&rc, "tok", 1);
    CHECK(rest_create_dm(&rc, "") == REST_E_BAD && rest_create_dm(&rc, "7x") == REST_E_BAD && rest_create_dm(&rc, NULL) == REST_E_BAD);
    id = rest_create_dm(&rc, "123456789012345678");
    CHECK(id > 0);
    CHECK(next_req(&rc, 0, t, sizeof(t)) == id);
    CHECK(STARTS(t, "POST /api/v10/users/@me/channels HTTP/1.1\r\n"));
    CHECK(has(t, "\r\n\r\n{\"recipient_id\":\"123456789012345678\"}"));
    answer(&rc, 0, 200, "", "{\"id\":\"900\",\"type\":1,\"recipients\":[{\"id\":\"123456789012345678\"}]}");
    CHECK(rest_next_result(&rc, &res) && res.ok && res.kind == REST_CREATE_DM && strcmp(json_get_str(res.body, "id", ""), "900") == 0);
    rest_free(&rc);
}

static void test_errors(void)
{
    RestClient rc;
    RestResult res;

    rest_init(&rc, "tok", 1);
    rest_get_me(&rc);
    next_req(&rc, 0, NULL, 0);
    answer(&rc, 0, 401, "", "{\"message\":\"401: Unauthorized\",\"code\":0}");
    CHECK(rest_next_result(&rc, &res));
    CHECK(res.status == 401 && !res.ok && res.code == 0 && strstr(res.error, "token") != NULL);
    CHECK(strstr(res.error, "401: Unauthorized") != NULL);

    rest_post_message(&rc, "5", "hello");
    next_req(&rc, 0, NULL, 0);
    answer(&rc, 0, 403, "", "{\"message\":\"Missing Access\",\"code\":50001}");
    CHECK(rest_next_result(&rc, &res));
    CHECK(res.status == 403 && !res.ok && res.code == 50001 && strstr(res.error, "Missing Access") != NULL);

    rest_post_message(&rc, "5", "hello");
    next_req(&rc, 0, NULL, 0);
    answer(&rc, 0, 400, "", "{\"message\":\"Cannot send an empty message\",\"code\":50006}");
    CHECK(rest_next_result(&rc, &res));
    CHECK(res.code == 50006 && strcmp(res.error, "Cannot send an empty message") == 0);

    /* no JSON body */
    rest_get_me(&rc);
    next_req(&rc, 0, NULL, 0);
    answer(&rc, 0, 404, "", "");
    CHECK(rest_next_result(&rc, &res));
    CHECK(res.status == 404 && !res.ok && res.error[0] != '\0' && res.body == NULL);

    rest_get_me(&rc);
    next_req(&rc, 0, NULL, 0);
    answer(&rc, 0, 418, "", "<html>teapot</html>");
    CHECK(rest_next_result(&rc, &res));
    CHECK(res.status == 418 && !res.ok && strstr(res.error, "418") != NULL && res.body == NULL);

    /* 200 with a body that is not JSON */
    rest_get_me(&rc);
    next_req(&rc, 0, NULL, 0);
    answer(&rc, 0, 200, "", "not json at all");
    CHECK(rest_next_result(&rc, &res));
    CHECK(res.status == 200 && !res.ok && res.error[0] != '\0');

    /* 204 with no body is fine */
    rest_post_message(&rc, "5", "x");
    next_req(&rc, 0, NULL, 0);
    answer(&rc, 0, 204, "", "");
    CHECK(rest_next_result(&rc, &res) && res.ok && res.body == NULL);

    /* an empty token cannot build a request: the call fails, nothing is sent */
    rest_free(&rc);
    rest_init(&rc, "", 1);
    rest_get_me(&rc);
    CHECK(next_req(&rc, 0, NULL, 0) == 0);
    CHECK(rest_next_result(&rc, &res) && res.status == 0 && !res.ok && strstr(res.error, "token") != NULL);
    rest_free(&rc);

    /* a token with a control character likewise */
    rest_init(&rc, "bad\r\nX-Evil: 1", 1);
    rest_get_me(&rc);
    CHECK(next_req(&rc, 0, NULL, 0) == 0);
    CHECK(rest_next_result(&rc, &res) && !res.ok);
    rest_free(&rc);
}

static void test_429(void)
{
    RestClient rc;
    RestResult res;
    char t[1024];
    int i;
    KMs now = 5000;

    rest_init(&rc, "tok", 1);
    rest_post_message(&rc, "7", "a");
    CHECK(next_req(&rc, now, NULL, 0) == 1);
    answer(&rc, now, 429, "Retry-After: 2\r\n", "{\"message\":\"You are being rate limited.\",\"retry_after\":1.5,\"global\":false}");
    CHECK(!rest_next_result(&rc, &res));                         /* retried, not reported */
    CHECK(rest_pending(&rc) == 1);
    CHECK(rest_next_deadline(&rc, now) == 1500);
    CHECK(next_req(&rc, now + 1499, NULL, 0) == 0);
    CHECK(rest_next_deadline(&rc, now + 1000) == 500);
    CHECK(next_req(&rc, now + 1500, t, sizeof(t)) == 1 && STARTS(t, "POST "));
    answer(&rc, now + 1500, 200, "", "{\"id\":\"1\"}");
    CHECK(rest_next_result(&rc, &res) && res.ok && res.id == 1);

    /* the Retry-After header when the body has no number; header seconds are decimals too */
    rest_get_me(&rc);
    next_req(&rc, now, NULL, 0);
    answer(&rc, now, 429, "Retry-After: 3\r\n", "");
    CHECK(rest_next_deadline(&rc, now) == 3000);
    next_req(&rc, now + 3000, NULL, 0);
    answer(&rc, now + 3000, 429, "X-RateLimit-Reset-After: 0.0011\r\n", "");
    CHECK(rest_next_deadline(&rc, now + 3000) == 2);             /* rounded up */
    next_req(&rc, now + 3002, NULL, 0);
    answer(&rc, now + 3002, 200, "", "{}");
    CHECK(rest_next_result(&rc, &res) && res.ok);

    /* it gives up after four retries and says how long Discord wants */
    rest_get_me(&rc);
    for (i = 0; i < 4; i++) {
        CHECK(next_req(&rc, now + 10000UL * (unsigned)(i + 1), NULL, 0) != 0);
        answer(&rc, now + 10000UL * (unsigned)(i + 1), 429, "", "{\"retry_after\":0.5}");
        CHECK(!rest_next_result(&rc, &res));
    }
    CHECK(next_req(&rc, now + 60000UL, NULL, 0) != 0);
    answer(&rc, now + 60000UL, 429, "", "{\"retry_after\":0.5}");
    CHECK(rest_next_result(&rc, &res) && res.status == 429 && !res.ok && res.retry_after_ms == 500);
    CHECK(strstr(res.error, "rate limiting") != NULL);

    /* a wait longer than a minute is reported straight away */
    now += 200000;
    rest_get_me(&rc);
    next_req(&rc, now, NULL, 0);
    answer(&rc, now, 429, "", "{\"retry_after\":3600.0}");
    CHECK(rest_next_result(&rc, &res) && res.status == 429 && res.retry_after_ms == 3600000L);
    CHECK(strstr(res.error, "3600 seconds") != NULL);
    CHECK(rest_pending(&rc) == 0);
    rest_free(&rc);
}

static void test_buckets_and_global(void)
{
    RestClient rc;
    RestResult res;
    KMs now = 100000;

    /* a route with no budget left waits for its reset; other routes carry on */
    rest_init(&rc, "tok", 1);
    rest_post_message(&rc, "7", "one");
    rest_post_message(&rc, "7", "two");
    rest_post_message(&rc, "8", "other channel");
    CHECK(next_req(&rc, now, NULL, 0) == 1);
    answer(&rc, now, 200, "X-RateLimit-Remaining: 0\r\nX-RateLimit-Reset-After: 0.917\r\n", "{}");
    CHECK(rest_next_result(&rc, &res) && res.ok);
    CHECK(next_req(&rc, now + 10, NULL, 0) == 3);                /* channel 8 is not held up, and goes first */
    answer(&rc, now + 10, 200, "X-RateLimit-Remaining: 4\r\nX-RateLimit-Reset-After: 1.0\r\n", "{}");
    CHECK(rest_next_result(&rc, &res) && res.id == 3);
    CHECK(rest_next_deadline(&rc, now + 10) == 907);
    CHECK(next_req(&rc, now + 916, NULL, 0) == 0);
    CHECK(next_req(&rc, now + 917, NULL, 0) == 2);
    answer(&rc, now + 917, 200, "X-RateLimit-Remaining: 1\r\nX-RateLimit-Reset-After: 5\r\n", "{}");
    CHECK(rest_next_result(&rc, &res) && res.id == 2);
    rest_post_message(&rc, "7", "three");
    CHECK(next_req(&rc, now + 918, NULL, 0) == 4);               /* budget left: no wait */
    answer(&rc, now + 918, 200, "", "{}");
    CHECK(rest_next_result(&rc, &res) && res.id == 4);

    /* a global 429 holds back every route */
    rest_get_me(&rc);
    rest_get_guilds(&rc);
    CHECK(next_req(&rc, now, NULL, 0) == 5);
    answer(&rc, now, 429, "X-RateLimit-Global: true\r\n", "{\"retry_after\":2.0,\"global\":true}");
    CHECK(rest_next_deadline(&rc, now) == 2000);
    CHECK(next_req(&rc, now + 1999, NULL, 0) == 0);
    CHECK(next_req(&rc, now + 2000, NULL, 0) == 5);              /* the retried call keeps its place in line */
    answer(&rc, now + 2000, 200, "", "{}");
    CHECK(rest_next_result(&rc, &res) && res.id == 5);
    CHECK(next_req(&rc, now + 2000, NULL, 0) == 6);
    answer(&rc, now + 2000, 200, "", "[]");
    CHECK(rest_next_result(&rc, &res) && res.id == 6);

    /* garbage rate-limit headers are ignored */
    now += 100000;
    rest_get_me(&rc);
    next_req(&rc, now, NULL, 0);
    answer(&rc, now, 200, "X-RateLimit-Remaining: soon\r\nX-RateLimit-Reset-After: -4\r\n", "{}");
    rest_get_me(&rc);
    CHECK(next_req(&rc, now, NULL, 0) != 0);
    rest_free(&rc);
}

static void test_retries(void)
{
    RestClient rc;
    RestResult res;
    KMs now = 1000;

    rest_init(&rc, "tok", 1);
    /* a GET is retried on 5xx, 1 s then 2 s, then reported */
    rest_get_guilds(&rc);
    next_req(&rc, now, NULL, 0);
    answer(&rc, now, 502, "", "<html>bad gateway</html>");
    CHECK(!rest_next_result(&rc, &res) && rest_next_deadline(&rc, now) == 1000);
    CHECK(next_req(&rc, now + 1000, NULL, 0) == 1);
    answer(&rc, now + 1000, 503, "", "");
    CHECK(!rest_next_result(&rc, &res) && rest_next_deadline(&rc, now + 1000) == 2000);
    CHECK(next_req(&rc, now + 3000, NULL, 0) == 1);
    answer(&rc, now + 3000, 502, "", "");
    CHECK(rest_next_result(&rc, &res) && res.status == 502 && !res.ok);

    /* a POST is not: the message may have gone through */
    rest_post_message(&rc, "9", "hi");
    next_req(&rc, now, NULL, 0);
    answer(&rc, now, 500, "", "");
    CHECK(rest_next_result(&rc, &res) && res.status == 500);

    /* transport failures */
    rest_get_me(&rc);
    next_req(&rc, now, NULL, 0);
    rest_failed(&rc, now, "connection reset");
    CHECK(!rest_next_result(&rc, &res) && rest_pending(&rc) == 1);
    CHECK(next_req(&rc, now + 1000, NULL, 0) != 0);
    rest_failed(&rc, now + 1000, "connection reset");
    CHECK(next_req(&rc, now + 3000, NULL, 0) != 0);
    rest_failed(&rc, now + 3000, "connection reset");
    CHECK(rest_next_result(&rc, &res) && res.status == 0 && !res.ok && strstr(res.error, "connection reset") != NULL);

    rest_post_message(&rc, "9", "hi");
    next_req(&rc, now, NULL, 0);
    rest_failed(&rc, now, NULL);
    CHECK(rest_next_result(&rc, &res) && res.status == 0 && strstr(res.error, "Could not reach") != NULL);

    /* nothing in flight: both are no-ops */
    answer(&rc, now, 200, "", "{}");
    rest_failed(&rc, now, "x");
    CHECK(!rest_next_result(&rc, &res));
    rest_free(&rc);
}

static void test_backpressure_and_wrap(void)
{
    RestClient rc;
    RestResult res;
    int i;
    KMs now;

    rest_init(&rc, "tok", 1);
    for (i = 0; i < 6; i++) {
        rest_get_me(&rc);
    }
    for (i = 0; i < REST_RESULTQ; i++) {
        CHECK(next_req(&rc, 0, NULL, 0) != 0);
        answer(&rc, 0, 200, "", "{\"n\":1}");
    }
    CHECK(next_req(&rc, 0, NULL, 0) == 0);                       /* the result queue is full: hold off */
    CHECK(rest_next_deadline(&rc, 0) == -1);
    CHECK(rest_next_result(&rc, &res) && res.id == 1 && json_get(res.body, "n") != NULL);
    CHECK(next_req(&rc, 0, NULL, 0) == 5);
    answer(&rc, 0, 200, "", "{}");
    for (i = 0; i < 4; i++) {
        CHECK(rest_next_result(&rc, &res) && res.id == i + 2);
    }
    CHECK(next_req(&rc, 0, NULL, 0) == 6);
    answer(&rc, 0, 200, "", "{}");
    CHECK(rest_next_result(&rc, &res) && res.id == 6 && !rest_next_result(&rc, &res));
    rest_free(&rc);

    /* waits survive the 32-bit clock wrapping */
    rest_init(&rc, "tok", 1);
    now = 0xFFFFFF00u;
    rest_get_me(&rc);
    next_req(&rc, now, NULL, 0);
    answer(&rc, now, 429, "", "{\"retry_after\":1.0}");
    CHECK(rest_next_deadline(&rc, now) == 1000);
    CHECK(next_req(&rc, now + 999, NULL, 0) == 0);
    CHECK(next_req(&rc, now + 1000, NULL, 0) == 1);              /* now+1000 wrapped past zero */
    answer(&rc, now + 1000, 200, "X-RateLimit-Remaining: 0\r\nX-RateLimit-Reset-After: 2\r\n", "{}");
    CHECK(rest_next_result(&rc, &res) && res.ok);
    rest_get_me(&rc);
    CHECK(rest_next_deadline(&rc, now + 1000) == 2000);
    CHECK(next_req(&rc, now + 2999, NULL, 0) == 0 && next_req(&rc, now + 3000, NULL, 0) == 2);
    rest_free(&rc);
}

/* ---- random operations and random replies: invariants, and ASan/UBSan do the rest ---- */
static unsigned long rnd_state = 12345;
static unsigned rnd(void)
{
    rnd_state = rnd_state * 1103515245UL + 12345UL;
    return (unsigned)((rnd_state >> 16) & 0x7fff);
}

static void test_fuzz(void)
{
    RestClient rc;
    RestResult res;
    static const char *bodies[] = {
        "", "{}", "[]", "{\"message\":\"m\",\"code\":10003}", "{\"retry_after\":0.25}", "{\"retry_after\":-1}",
        "{\"retry_after\":\"x\",\"global\":1}", "garbage", "{\"id\":\"1\"", "[[[[[[[[[[1]]]]]]]]]]", "\"str\"", "null"
    };
    static const char *hdrs[] = {
        "", "Retry-After: 1\r\n", "Retry-After: abc\r\n", "X-RateLimit-Remaining: 0\r\nX-RateLimit-Reset-After: 0.5\r\n",
        "X-RateLimit-Remaining: 9999999999999999\r\nX-RateLimit-Reset-After: 99999999999\r\n",
        "X-RateLimit-Global: true\r\n", "X-RateLimit-Remaining: 1\r\n"
    };
    static const int statuses[] = { 200, 200, 204, 400, 401, 403, 404, 429, 429, 500, 502, 503, 301, 418 };
    int i, inflight = 0, results = 0;
    KMs now = 0xFFFFF000u;
    HttpReq q;
    long id, w;
    HttpResp r;

    rest_init(&rc, "tok", 1);
    for (i = 0; i < 6000; i++) {
        switch (rnd() % 8) {
        case 0: rest_get_guilds(&rc); break;
        case 1: rest_post_message(&rc, "12", "fuzz"); break;
        case 2: rest_get_messages(&rc, "1000000000000", 1 + (int)(rnd() % 100), NULL); break;
        case 3: rest_get_channels(&rc, "13"); break;
        case 4:
            if (!inflight && rest_next_request(&rc, now, &q, &id)) {
                http_req_free(&q);
                inflight = 1;
            }
            break;
        case 5:
            if (inflight) {
                reply(&r, statuses[rnd() % (sizeof(statuses) / sizeof(statuses[0]))],
                      hdrs[rnd() % (sizeof(hdrs) / sizeof(hdrs[0]))], bodies[rnd() % (sizeof(bodies) / sizeof(bodies[0]))]);
                rest_response(&rc, now, &r);
                http_resp_free(&r);
                inflight = 0;
            }
            break;
        case 6:
            if (inflight && rnd() % 3 == 0) {
                rest_failed(&rc, now, "boom");
                inflight = 0;
            }
            break;
        default:
            now += rnd() % 4000;
            break;
        }
        if (rnd() % 3 == 0) {
            while (rest_next_result(&rc, &res)) {
                results++;
                CHECK(res.id > 0 && (res.ok == 0 || res.ok == 1));
                CHECK(!res.ok || (res.status >= 200 && res.status < 300));
            }
        }
        w = rest_next_deadline(&rc, now);
        CHECK(w >= -1 && w <= 3600000L + 60000L);
        CHECK(rc.nresults <= REST_RESULTQ);
    }
    CHECK(results > 100);
    rest_free(&rc);
}

int main(void)
{
    test_queueing();
    test_requests();
    test_create_dm();
    test_errors();
    test_429();
    test_buckets_and_global();
    test_retries();
    test_backpressure_and_wrap();
    test_fuzz();
    TEST_DONE("rest");
}
