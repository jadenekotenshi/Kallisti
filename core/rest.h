/*
 * rest.h -- Discord's REST API (v10) as a sans-I/O request queue.
 *
 * Like the gateway it never touches a socket or a clock.  The caller queues calls (rest_get_guilds(),
 * rest_post_message(), ...), then loops:
 *   - rest_next_request(): when it returns 1, send req->buf over a TLS connection to rest->host:rest->port
 *     (opened or reused as the caller likes) and feed the reply to an HttpResp created with REST_MAX_RESPONSE;
 *   - on HTTP_DONE call rest_response(), on a failed transport rest_failed();
 *   - rest_next_result() hands back finished calls (also after rest_next_request(), which can fail a call
 *     that could not even be built);
 *   - rest_next_deadline() says how long until a held-back call may go (rate limit, retry backoff).
 * One request is in flight at a time; calls go out in the order they were queued.
 *
 * Rate limits: the X-RateLimit-Remaining / Reset-After headers of every reply are remembered per route, and
 * a call whose route has no budget left waits for the reset instead of earning a 429.  A 429 itself (global
 * or not) is waited out and retried up to four times, unless Discord asks for more than a minute, which is
 * reported to the caller.  GET calls are retried on 5xx and on transport failures; a POST never is, since
 * the message may have been delivered.
 *
 * The user-token mode sends the token bare, a bot token as "Bot <token>".
 */
#ifndef KALLISTI_REST_H
#define KALLISTI_REST_H

#include <stddef.h>
#include "http.h"
#include "json.h"
#include "kms.h"

#define REST_MAX_RESPONSE (1024 * 1024)       /* max_body for the caller's HttpResp */
#define REST_MAX_JOBS 16
#define REST_MAX_BUCKETS 24
#define REST_RESULTQ 4
#define REST_MAX_CONTENT 2000                 /* characters in a message */
#define REST_MAX_WAIT_MS 60000L               /* longer rate-limit waits are reported instead of waited out */

/* queueing errors (a call's id is always positive) */
enum { REST_E_BAD = -1, REST_E_TOO_LONG = -2, REST_E_FULL = -3 };

enum { REST_ME = 1, REST_GATEWAY_BOT, REST_GUILDS, REST_CHANNELS, REST_MESSAGES, REST_POST_MESSAGE, REST_CREATE_DM };

typedef struct {
    int used;
    long id;
    int kind;
    int is_get;
    char method[8];
    char path[128];              /* below the API prefix, with the query */
    char route[136];             /* the rate-limit key: method and path without the query */
    char *body;
    size_t body_len;
    int attempts;
    int has_wait;
    KMs not_before;
    int in_flight;
} RestJob;

typedef struct {
    int used;
    char route[136];
    long remaining;
    KMs reset_at;
    unsigned long stamp;
} RestBucket;

typedef struct {
    long id;
    int kind;
    int status;                  /* the HTTP status, or 0 if the request never got an answer */
    int ok;                      /* 2xx and, if there was a body, parseable JSON */
    long code;                   /* Discord's JSON error code, 0 if none */
    long retry_after_ms;         /* a 429 that was not retried: how long Discord wants us to wait */
    char error[160];             /* what went wrong, for the user */
    const JNode *body;           /* the parsed JSON reply or NULL; valid until the next rest_next_result() */
} RestResult;

typedef struct {
    char token[200];
    int bot;                     /* 1: "Bot <token>", 0: the token as is (a user token) */
    char host[256];
    int port, tls;
    char prefix[16];             /* "/api/v10" */
    char user_agent[96];

    RestJob jobs[REST_MAX_JOBS];
    RestBucket buckets[REST_MAX_BUCKETS];
    unsigned long stamp;
    long next_id;
    long in_flight;              /* index+1 into jobs, 0 if none */

    int global_blocked;
    KMs global_until;

    RestResult results[REST_RESULTQ];
    JDoc docs[REST_RESULTQ];
    int nresults;
    JDoc out_doc;                /* the document behind the result handed out last */
    int have_out;
} RestClient;

void rest_init(RestClient *rc, const char *token, int is_bot);
void rest_free(RestClient *rc);

/* Each returns the call's id (> 0) or a REST_E_* error.  Ids are Discord snowflakes as digit strings. */
long rest_get_me(RestClient *rc);
long rest_get_gateway_bot(RestClient *rc);
long rest_get_guilds(RestClient *rc);
long rest_get_channels(RestClient *rc, const char *guild_id);
/* The newest `limit` (1..100) messages, or those before message `before` (NULL for the newest). */
long rest_get_messages(RestClient *rc, const char *channel_id, int limit, const char *before);
/* content is UTF-8, 1..2000 characters, not only whitespace. */
long rest_post_message(RestClient *rc, const char *channel_id, const char *content);
/* Opens (or finds) the direct-message channel with a user: POST /users/@me/channels. The reply is a channel
 * object. A bot can only do this for people it shares a server with. */
long rest_create_dm(RestClient *rc, const char *user_id);

/* Builds the next request that may go out now into *req (already http_req_init()ed by this function) and
 * sets *id to its call.  Returns 1, or 0 if there is nothing to send (yet, or a request is in flight). */
int rest_next_request(RestClient *rc, KMs now, HttpReq *req, long *id);
/* The reply to the request in flight, complete (HTTP_DONE). */
void rest_response(RestClient *rc, KMs now, const HttpResp *resp);
/* The request in flight got no usable reply (connect/TLS/read failure, HTTP_FAILED, early EOF). */
void rest_failed(RestClient *rc, KMs now, const char *why);
int rest_next_result(RestClient *rc, RestResult *res);
/* Milliseconds until rest_next_request() may produce something (0 = now), or -1 if nothing is queued or a
 * request is in flight. */
long rest_next_deadline(const RestClient *rc, KMs now);
/* Calls queued or in flight. */
int rest_pending(const RestClient *rc);

#endif
