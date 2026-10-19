/* Discord REST request queue: see rest.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rest.h"
#include "ws.h"

#define MAX_ATTEMPTS 4              /* 429 retries */
#define MAX_GET_RETRIES 2           /* 5xx and transport failures, GET only */

/* ---------------------------------------------------------------- small helpers */

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);

    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int valid_id(const char *s)
{
    size_t n = 0;

    if (s == NULL) {
        return 0;
    }
    while (s[n] >= '0' && s[n] <= '9') {
        n++;
    }
    return n >= 1 && n <= 20 && s[n] == '\0';
}

/* "1.337" -> 1337 (rounded up to the millisecond), or -1 if it is not a plain non-negative decimal. */
static long secs_to_ms(const char *s)
{
    long whole = 0, ms = 0, scale = 100;
    int digits = 0, extra = 0;

    if (s == NULL) {
        return -1;
    }
    while (*s >= '0' && *s <= '9') {
        if (whole > 1000000L) {
            return -1;
        }
        whole = whole * 10 + (*s - '0');
        digits++;
        s++;
    }
    if (digits == 0) {
        return -1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (scale > 0) {
                ms += (*s - '0') * scale;
                scale /= 10;
            } else if (*s != '0') {
                extra = 1;
            }
            s++;
        }
    }
    if (*s != '\0') {
        return -1;
    }
    return whole * 1000L + ms + extra;
}

static long double_to_ms(double secs)
{
    long ms;

    if (!(secs >= 0.0) || secs > 1000000.0) {
        return -1;
    }
    ms = (long)(secs * 1000.0);
    if ((double)ms + 1e-6 < secs * 1000.0) {
        ms++;
    }
    return ms;
}

/* ---------------------------------------------------------------- lifecycle */

void rest_init(RestClient *rc, const char *token, int is_bot)
{
    memset(rc, 0, sizeof(*rc));
    copy_str(rc->token, sizeof(rc->token), token != NULL ? token : "");
    rc->bot = is_bot;
    copy_str(rc->host, sizeof(rc->host), "discord.com");
    rc->port = 443;
    rc->tls = 1;
    copy_str(rc->prefix, sizeof(rc->prefix), "/api/v10");
    copy_str(rc->user_agent, sizeof(rc->user_agent), "DiscordBot (https://github.com/jadenekotenshi/Kallisti, 0.1)");
    rc->next_id = 0;
}

static void job_clear(RestJob *j)
{
    free(j->body);
    memset(j, 0, sizeof(*j));
}

void rest_free(RestClient *rc)
{
    int i;

    for (i = 0; i < REST_MAX_JOBS; i++) {
        job_clear(&rc->jobs[i]);
    }
    for (i = 0; i < rc->nresults; i++) {
        json_free(&rc->docs[i]);
    }
    rc->nresults = 0;
    if (rc->have_out) {
        json_free(&rc->out_doc);
        rc->have_out = 0;
    }
    rc->in_flight = 0;
    memset(rc->token, 0, sizeof(rc->token));
}

int rest_pending(const RestClient *rc)
{
    int i, n = 0;

    for (i = 0; i < REST_MAX_JOBS; i++) {
        if (rc->jobs[i].used) {
            n++;
        }
    }
    return n;
}

/* ---------------------------------------------------------------- queueing */

static long queue_job(RestClient *rc, int kind, const char *method, const char *path, const char *body)
{
    int i;
    RestJob *j = NULL;
    size_t q;

    for (i = 0; i < REST_MAX_JOBS; i++) {
        if (!rc->jobs[i].used) {
            j = &rc->jobs[i];
            break;
        }
    }
    if (j == NULL) {
        return REST_E_FULL;
    }
    memset(j, 0, sizeof(*j));
    if (body != NULL) {
        j->body_len = strlen(body);
        j->body = (char *)malloc(j->body_len + 1);
        if (j->body == NULL) {
            return REST_E_FULL;
        }
        memcpy(j->body, body, j->body_len + 1);
    }
    j->used = 1;
    j->id = ++rc->next_id;
    j->kind = kind;
    j->is_get = strcmp(method, "GET") == 0;
    copy_str(j->method, sizeof(j->method), method);
    copy_str(j->path, sizeof(j->path), path);
    q = strcspn(path, "?");
    sprintf(j->route, "%s ", method);
    strncat(j->route, path, q < sizeof(j->route) - 8 ? q : sizeof(j->route) - 8);
    return j->id;
}

long rest_get_me(RestClient *rc)
{
    return queue_job(rc, REST_ME, "GET", "/users/@me", NULL);
}

long rest_get_gateway_bot(RestClient *rc)
{
    return queue_job(rc, REST_GATEWAY_BOT, "GET", "/gateway/bot", NULL);
}

long rest_get_guilds(RestClient *rc)
{
    return queue_job(rc, REST_GUILDS, "GET", "/users/@me/guilds", NULL);
}

long rest_get_channels(RestClient *rc, const char *guild_id)
{
    char path[128];

    if (!valid_id(guild_id)) {
        return REST_E_BAD;
    }
    sprintf(path, "/guilds/%s/channels", guild_id);
    return queue_job(rc, REST_CHANNELS, "GET", path, NULL);
}

long rest_get_messages(RestClient *rc, const char *channel_id, int limit, const char *before)
{
    char path[128];

    if (!valid_id(channel_id) || limit < 1 || limit > 100 || (before != NULL && !valid_id(before))) {
        return REST_E_BAD;
    }
    if (before != NULL) {
        sprintf(path, "/channels/%s/messages?limit=%d&before=%s", channel_id, limit, before);
    } else {
        sprintf(path, "/channels/%s/messages?limit=%d", channel_id, limit);
    }
    return queue_job(rc, REST_MESSAGES, "GET", path, NULL);
}

long rest_post_message(RestClient *rc, const char *channel_id, const char *content)
{
    char path[128];
    JWriter w;
    const unsigned char *p;
    size_t n, chars = 0;
    int blank = 1;
    long id;

    if (!valid_id(channel_id) || content == NULL) {
        return REST_E_BAD;
    }
    n = strlen(content);
    if (!ws_utf8_valid((const unsigned char *)content, n)) {
        return REST_E_BAD;
    }
    for (p = (const unsigned char *)content; *p != '\0'; p++) {
        if ((*p & 0xC0) != 0x80) {
            chars++;
        }
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            blank = 0;
        }
    }
    if (blank) {
        return REST_E_BAD;
    }
    if (chars > REST_MAX_CONTENT) {
        return REST_E_TOO_LONG;
    }
    sprintf(path, "/channels/%s/messages", channel_id);
    jw_init(&w);
    jw_begin_obj(&w);
    jw_key(&w, "content");
    jw_str(&w, content);
    jw_end_obj(&w);
    if (w.failed) {
        jw_free(&w);
        return REST_E_FULL;
    }
    id = queue_job(rc, REST_POST_MESSAGE, "POST", path, w.buf);
    jw_free(&w);
    return id;
}

long rest_create_dm(RestClient *rc, const char *user_id)
{
    JWriter w;
    long id;

    if (!valid_id(user_id)) {
        return REST_E_BAD;
    }
    jw_init(&w);
    jw_begin_obj(&w);
    jw_key(&w, "recipient_id");
    jw_str(&w, user_id);
    jw_end_obj(&w);
    if (w.failed) {
        jw_free(&w);
        return REST_E_FULL;
    }
    id = queue_job(rc, REST_CREATE_DM, "POST", "/users/@me/channels", w.buf);
    jw_free(&w);
    return id;
}

/* ---------------------------------------------------------------- rate limits */

static RestBucket *find_bucket(RestClient *rc, const char *route)
{
    int i;

    for (i = 0; i < REST_MAX_BUCKETS; i++) {
        if (rc->buckets[i].used && strcmp(rc->buckets[i].route, route) == 0) {
            return &rc->buckets[i];
        }
    }
    return NULL;
}

static void set_bucket(RestClient *rc, const char *route, long remaining, KMs reset_at)
{
    RestBucket *b = find_bucket(rc, route);
    int i, oldest = 0;

    if (b == NULL) {
        for (i = 0; i < REST_MAX_BUCKETS; i++) {
            if (!rc->buckets[i].used) {
                oldest = i;
                break;
            }
            if (rc->buckets[i].stamp < rc->buckets[oldest].stamp) {
                oldest = i;
            }
        }
        b = &rc->buckets[oldest];
        memset(b, 0, sizeof(*b));
        b->used = 1;
        copy_str(b->route, sizeof(b->route), route);
    }
    b->remaining = remaining;
    b->reset_at = reset_at;
    b->stamp = ++rc->stamp;
}

/* Milliseconds until this job may be sent (<= 0: now). */
static long ready_in(const RestClient *rc, const RestJob *j, KMs now)
{
    long w = 0, x;
    const RestBucket *b;
    int i;

    if (j->has_wait) {
        x = KMS_SINCE(j->not_before, now);
        if (x > w) {
            w = x;
        }
    }
    if (rc->global_blocked) {
        x = KMS_SINCE(rc->global_until, now);
        if (x > w) {
            w = x;
        }
    }
    for (i = 0; i < REST_MAX_BUCKETS; i++) {
        b = &rc->buckets[i];
        if (b->used && strcmp(b->route, j->route) == 0) {
            if (b->remaining <= 0) {
                x = KMS_SINCE(b->reset_at, now);
                if (x > w) {
                    w = x;
                }
            }
            break;
        }
    }
    return w;
}

long rest_next_deadline(const RestClient *rc, KMs now)
{
    long best = -1, w;
    int i;

    if (rc->in_flight || rc->nresults >= REST_RESULTQ) {
        return -1;
    }
    for (i = 0; i < REST_MAX_JOBS; i++) {
        if (rc->jobs[i].used) {
            w = ready_in(rc, &rc->jobs[i], now);
            if (w < 0) {
                w = 0;
            }
            if (best < 0 || w < best) {
                best = w;
            }
        }
    }
    return best;
}

/* ---------------------------------------------------------------- results */

/* Finishes a job: queues its result (taking ownership of *doc, which may be empty) and frees the job. */
static void finish(RestClient *rc, RestJob *j, int status, int ok, long code, long retry_after_ms,
                   const char *error, JDoc *doc)
{
    RestResult *r;

    if (rc->nresults >= REST_RESULTQ) {          /* cannot happen: requests only start with room for a result */
        if (doc != NULL) {
            json_free(doc);
        }
        job_clear(j);
        rc->in_flight = 0;
        return;
    }
    r = &rc->results[rc->nresults];
    memset(r, 0, sizeof(*r));
    r->id = j->id;
    r->kind = j->kind;
    r->status = status;
    r->ok = ok;
    r->code = code;
    r->retry_after_ms = retry_after_ms;
    copy_str(r->error, sizeof(r->error), error != NULL ? error : "");
    if (doc != NULL) {
        rc->docs[rc->nresults] = *doc;
    } else {
        memset(&rc->docs[rc->nresults], 0, sizeof(JDoc));
    }
    rc->nresults++;
    job_clear(j);
    rc->in_flight = 0;
}

int rest_next_result(RestClient *rc, RestResult *res)
{
    int i;

    if (rc->have_out) {
        json_free(&rc->out_doc);
        rc->have_out = 0;
    }
    if (rc->nresults == 0) {
        return 0;
    }
    *res = rc->results[0];
    rc->out_doc = rc->docs[0];
    rc->have_out = 1;
    res->body = rc->out_doc.root;
    for (i = 1; i < rc->nresults; i++) {
        rc->results[i - 1] = rc->results[i];
        rc->docs[i - 1] = rc->docs[i];
    }
    rc->nresults--;
    return 1;
}

/* ---------------------------------------------------------------- requests */

static int build_request(const RestClient *rc, const RestJob *j, HttpReq *req)
{
    char full[160], auth[240];

    sprintf(full, "%s%s", rc->prefix, j->path);
    if (rc->bot) {
        sprintf(auth, "Bot %.200s", rc->token);
    } else {
        copy_str(auth, sizeof(auth), rc->token);
    }
    http_req_init(req);
    if (rc->token[0] == '\0' ||
        http_req_begin(req, j->method, full, rc->host, rc->port, rc->tls) != 0 ||
        http_req_header(req, "Authorization", auth) != 0 ||
        http_req_header(req, "User-Agent", rc->user_agent) != 0 ||
        http_req_header(req, "Accept", "application/json") != 0 ||
        (j->body != NULL && http_req_header(req, "Content-Type", "application/json") != 0) ||
        http_req_end(req, j->body, j->body_len) != 0) {
        http_req_free(req);
        memset(auth, 0, sizeof(auth));
        return -1;
    }
    memset(auth, 0, sizeof(auth));
    return 0;
}

int rest_next_request(RestClient *rc, KMs now, HttpReq *req, long *id)
{
    int i, best;

    for (;;) {
        if (rc->in_flight || rc->nresults >= REST_RESULTQ) {
            return 0;
        }
        best = -1;
        for (i = 0; i < REST_MAX_JOBS; i++) {
            if (rc->jobs[i].used && ready_in(rc, &rc->jobs[i], now) <= 0 &&
                (best < 0 || rc->jobs[i].id < rc->jobs[best].id)) {
                best = i;
            }
        }
        if (best < 0) {
            return 0;
        }
        if (build_request(rc, &rc->jobs[best], req) == 0) {
            rc->jobs[best].in_flight = 1;
            rc->in_flight = best + 1;
            *id = rc->jobs[best].id;
            return 1;
        }
        finish(rc, &rc->jobs[best], 0, 0, 0, 0,
               rc->token[0] == '\0' ? "No token set" : "Could not build the request (the token contains invalid characters)",
               NULL);
    }
}

/* ---------------------------------------------------------------- replies */

static void retry_later(RestClient *rc, RestJob *j, KMs now, long ms)
{
    j->attempts++;
    j->has_wait = 1;
    j->not_before = now + (KMs)ms;
    j->in_flight = 0;
    rc->in_flight = 0;
}

static void note_limits(RestClient *rc, const RestJob *j, const HttpResp *resp, KMs now)
{
    const char *rem = http_resp_header(resp, "X-RateLimit-Remaining");
    long reset = secs_to_ms(http_resp_header(resp, "X-RateLimit-Reset-After"));
    char *end;
    long left;

    if (rem != NULL && reset >= 0) {
        left = strtol(rem, &end, 10);
        if (end != rem && *end == '\0') {
            set_bucket(rc, j->route, left, now + (KMs)reset);
        }
    }
}

static const char *status_hint(int status)
{
    switch (status) {
    case 400: return "Discord rejected the request";
    case 401: return "Discord rejected the token: check that it is correct";
    case 403: return "Discord denied access: the bot lacks permission for this";
    case 404: return "Discord does not know that server, channel or message";
    case 429: return "Discord is rate limiting us";
    default:
        return NULL;
    }
}

void rest_response(RestClient *rc, KMs now, const HttpResp *resp)
{
    RestJob *j;
    JDoc doc;
    JNode *root = NULL;
    long code = 0, retry_ms = -1, x;
    int ok, global = 0, status;
    char err[160];
    const char *msg, *h;
    JNode *n;

    if (!rc->in_flight) {
        return;
    }
    j = &rc->jobs[rc->in_flight - 1];
    status = resp->status;
    note_limits(rc, j, resp, now);

    memset(&doc, 0, sizeof(doc));
    if (resp->body_len > 0) {
        root = json_parse(&doc, (const char *)resp->body, resp->body_len);
        if (root == NULL) {
            json_free(&doc);
            memset(&doc, 0, sizeof(doc));
        }
    }

    if (status == 429) {
        n = json_get(root, "retry_after");
        if (n != NULL && n->type == J_NUM) {
            retry_ms = double_to_ms(n->dval);
        }
        if (retry_ms < 0) {
            retry_ms = secs_to_ms(http_resp_header(resp, "Retry-After"));
        }
        if (retry_ms < 0) {
            retry_ms = secs_to_ms(http_resp_header(resp, "X-RateLimit-Reset-After"));
        }
        if (retry_ms < 1) {
            retry_ms = 1000;
        }
        n = json_get(root, "global");
        if (n != NULL) {
            json_bool(n, &global);
        }
        h = http_resp_header(resp, "X-RateLimit-Global");
        if (h != NULL) {
            global = 1;
        }
        if (global) {
            rc->global_blocked = 1;
            rc->global_until = now + (KMs)(retry_ms > REST_MAX_WAIT_MS ? REST_MAX_WAIT_MS : retry_ms);
        } else {
            set_bucket(rc, j->route, 0, now + (KMs)(retry_ms > REST_MAX_WAIT_MS ? REST_MAX_WAIT_MS : retry_ms));
        }
        if (retry_ms <= REST_MAX_WAIT_MS && j->attempts < MAX_ATTEMPTS) {
            json_free(&doc);
            retry_later(rc, j, now, retry_ms);
            return;
        }
        sprintf(err, "Discord is rate limiting us; try again in %ld seconds", (retry_ms + 999) / 1000);
        json_free(&doc);
        finish(rc, j, 429, 0, 0, retry_ms, err, NULL);
        return;
    }

    if (status >= 500 && status <= 599 && j->is_get && j->attempts < MAX_GET_RETRIES) {
        json_free(&doc);
        retry_later(rc, j, now, 1000L << j->attempts);
        return;
    }

    ok = status >= 200 && status <= 299;
    err[0] = '\0';
    if (ok) {
        if (resp->body_len > 0 && root == NULL) {
            ok = 0;
            copy_str(err, sizeof(err), "Discord sent a reply that could not be read");
        }
    } else {
        msg = json_get_str(root, "message", NULL);
        n = json_get(root, "code");
        if (n != NULL && json_int(n, &x)) {
            code = x;
        }
        if (msg != NULL && msg[0] != '\0') {
            copy_str(err, sizeof(err), msg);
        } else if (status_hint(status) != NULL) {
            copy_str(err, sizeof(err), status_hint(status));
        } else {
            sprintf(err, "Discord answered HTTP %d", status);
        }
        if ((status == 401 || status == 403) && status_hint(status) != NULL && msg != NULL) {
            sprintf(err, "%.100s (%.50s)", status_hint(status), msg);
        }
    }
    finish(rc, j, status, ok, code, 0, err, root != NULL ? &doc : NULL);
    if (root == NULL) {
        json_free(&doc);
    }
}

void rest_failed(RestClient *rc, KMs now, const char *why)
{
    RestJob *j;
    char err[160];

    if (!rc->in_flight) {
        return;
    }
    j = &rc->jobs[rc->in_flight - 1];
    if (j->is_get && j->attempts < MAX_GET_RETRIES) {
        retry_later(rc, j, now, 1000L << j->attempts);
        return;
    }
    sprintf(err, "Could not reach Discord: %.120s", why != NULL ? why : "connection failed");
    finish(rc, j, 0, 0, 0, 0, err, NULL);
}
