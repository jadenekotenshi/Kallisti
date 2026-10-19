/* Small JSON reader/writer: see json.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

#define CHUNK_MIN 4096

/* ---------------------------------------------------------------- arena */

static void *arena_alloc(JDoc *d, size_t n)
{
    JChunk *c;
    size_t cap;
    void *p;

    n = (n + 7) & ~(size_t)7;
    c = d->chunks;
    if (c == NULL || c->cap - c->used < n) {
        cap = n > CHUNK_MIN ? n : CHUNK_MIN;
        c = (JChunk *)malloc(sizeof(JChunk) + cap);
        if (c == NULL) {
            return NULL;
        }
        c->next = d->chunks;
        c->used = 0;
        c->cap = cap + 8;
        d->chunks = c;
    }
    p = c->data + c->used;
    c->used += n;
    return p;
}

void json_free(JDoc *d)
{
    JChunk *c = d->chunks, *n;

    while (c != NULL) {
        n = c->next;
        free(c);
        c = n;
    }
    d->chunks = NULL;
    d->root = NULL;
}

/* ---------------------------------------------------------------- parser */

typedef struct {
    JDoc *doc;
    const char *start, *p, *end;
    int depth;
} Parser;

static JNode *parse_value(Parser *ps);

static JNode *fail(Parser *ps, const char *msg)
{
    if (ps->doc->err[0] == '\0') {
        sprintf(ps->doc->err, "%.60s at byte %lu", msg, (unsigned long)(ps->p - ps->start));
    }
    return NULL;
}

static void skip_ws(Parser *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
        ps->p++;
    }
}

static JNode *new_node(Parser *ps, JType type)
{
    JNode *n = (JNode *)arena_alloc(ps->doc, sizeof(JNode));

    if (n == NULL) {
        return fail(ps, "out of memory");
    }
    memset(n, 0, sizeof(JNode));
    n->type = type;
    return n;
}

static int hex4(const char *p, unsigned long *out)
{
    unsigned long v = 0;
    int i, c;

    for (i = 0; i < 4; i++) {
        c = (unsigned char)p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (unsigned long)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= (unsigned long)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= (unsigned long)(c - 'A' + 10);
        } else {
            return 0;
        }
    }
    *out = v;
    return 1;
}

static size_t put_utf8(char *o, unsigned long cp)
{
    if (cp < 0x80UL) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800UL) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000UL) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parses a string starting at the opening quote.  On success *out is the decoded string in the arena. */
static int parse_string(Parser *ps, const char **out, size_t *outlen)
{
    const char *q;
    char *buf, *o;
    unsigned long cp, lo;
    int c;

    /* find the closing quote to size the buffer (decoding never grows the text) */
    q = ps->p + 1;
    while (q < ps->end && *q != '"') {
        if (*q == '\\') {
            q++;
        }
        q++;
    }
    if (q >= ps->end) {
        fail(ps, "unterminated string");
        return 0;
    }
    buf = (char *)arena_alloc(ps->doc, (size_t)(q - ps->p) + 1);
    if (buf == NULL) {
        fail(ps, "out of memory");
        return 0;
    }
    o = buf;
    ps->p++;
    while (ps->p < q) {
        c = (unsigned char)*ps->p++;
        if (c == '\\') {
            c = (unsigned char)*ps->p++;
            switch (c) {
            case '"': case '\\': case '/':
                *o++ = (char)c;
                break;
            case 'b': *o++ = '\b'; break;
            case 'f': *o++ = '\f'; break;
            case 'n': *o++ = '\n'; break;
            case 'r': *o++ = '\r'; break;
            case 't': *o++ = '\t'; break;
            case 'u':
                if (q - ps->p < 4 || !hex4(ps->p, &cp)) {
                    fail(ps, "bad \\u escape");
                    return 0;
                }
                ps->p += 4;
                if (cp >= 0xD800UL && cp <= 0xDBFFUL) {
                    /* a high surrogate needs a following \uDC00..DFFF low one */
                    if (q - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u' &&
                        hex4(ps->p + 2, &lo) && lo >= 0xDC00UL && lo <= 0xDFFFUL) {
                        ps->p += 6;
                        cp = 0x10000UL + ((cp - 0xD800UL) << 10) + (lo - 0xDC00UL);
                    } else {
                        cp = 0xFFFDUL;
                    }
                } else if (cp >= 0xDC00UL && cp <= 0xDFFFUL) {
                    cp = 0xFFFDUL;
                }
                o += put_utf8(o, cp);
                break;
            default:
                fail(ps, "bad escape");
                return 0;
            }
        } else {
            *o++ = (char)c;
        }
    }
    *o = '\0';
    ps->p = q + 1;
    *out = buf;
    *outlen = (size_t)(o - buf);
    return 1;
}

static JNode *parse_number(Parser *ps)
{
    const char *s = ps->p;
    const char *q = s;
    JNode *n;
    char tmp[64];
    size_t len;
    int digits = 0, frac = 0;
    long iv = 0;
    int overflow = 0, neg = 0;

    if (q < ps->end && *q == '-') {
        neg = 1;
        q++;
    }
    if (q < ps->end && *q == '0') {
        digits++;
        q++;
    } else {
        while (q < ps->end && *q >= '0' && *q <= '9') {
            if (iv > (0x7FFFFFFFL - (*q - '0')) / 10) {
                overflow = 1;
            } else {
                iv = iv * 10 + (*q - '0');
            }
            digits++;
            q++;
        }
    }
    if (digits == 0) {
        ps->p = s;
        return fail(ps, "bad number");
    }
    if (q < ps->end && *q == '.') {
        frac = 1;
        q++;
        digits = 0;
        while (q < ps->end && *q >= '0' && *q <= '9') {
            digits++;
            q++;
        }
        if (digits == 0) {
            ps->p = s;
            return fail(ps, "bad number");
        }
    }
    if (q < ps->end && (*q == 'e' || *q == 'E')) {
        frac = 1;
        q++;
        if (q < ps->end && (*q == '+' || *q == '-')) {
            q++;
        }
        digits = 0;
        while (q < ps->end && *q >= '0' && *q <= '9') {
            digits++;
            q++;
        }
        if (digits == 0) {
            ps->p = s;
            return fail(ps, "bad number");
        }
    }
    len = (size_t)(q - s);
    if (len >= sizeof(tmp)) {
        ps->p = s;
        return fail(ps, "number too long");
    }
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    n = new_node(ps, J_NUM);
    if (n == NULL) {
        return NULL;
    }
    n->dval = strtod(tmp, NULL);
    if (!frac && !overflow) {
        n->is_int = 1;
        n->ival = neg ? -iv : iv;
    } else {
        n->ival = (n->dval > 2147483647.0) ? 2147483647L :
                  (n->dval < -2147483647.0) ? -2147483647L : (long)n->dval;
    }
    ps->p = q;
    return n;
}

static int expect_lit(Parser *ps, const char *lit)
{
    size_t n = strlen(lit);

    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0) {
        return 0;
    }
    ps->p += n;
    return 1;
}

static JNode *parse_container(Parser *ps, int is_obj)
{
    JNode *c, *child, *last = NULL;
    const char *key = NULL;
    size_t keylen;
    char close = is_obj ? '}' : ']';

    if (ps->depth >= JSON_MAX_DEPTH) {
        return fail(ps, "nested too deeply");
    }
    c = new_node(ps, is_obj ? J_OBJ : J_ARR);
    if (c == NULL) {
        return NULL;
    }
    ps->p++;                                  /* the [ or { */
    ps->depth++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        ps->depth--;
        return c;
    }
    for (;;) {
        skip_ws(ps);
        if (is_obj) {
            if (ps->p >= ps->end || *ps->p != '"') {
                return fail(ps, "expected a member name");
            }
            if (!parse_string(ps, &key, &keylen)) {
                return NULL;
            }
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                return fail(ps, "expected ':'");
            }
            ps->p++;
            skip_ws(ps);
        }
        child = parse_value(ps);
        if (child == NULL) {
            return NULL;
        }
        child->key = is_obj ? key : NULL;
        if (last == NULL) {
            c->first = child;
        } else {
            last->next = child;
        }
        last = child;
        c->count++;
        skip_ws(ps);
        if (ps->p >= ps->end) {
            return fail(ps, "unterminated container");
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == close) {
            ps->p++;
            break;
        }
        return fail(ps, "expected ',' or the closing bracket");
    }
    ps->depth--;
    return c;
}

static JNode *parse_value(Parser *ps)
{
    JNode *n;

    skip_ws(ps);
    if (ps->p >= ps->end) {
        return fail(ps, "unexpected end of input");
    }
    switch (*ps->p) {
    case '{':
        return parse_container(ps, 1);
    case '[':
        return parse_container(ps, 0);
    case '"':
        n = new_node(ps, J_STR);
        if (n == NULL) {
            return NULL;
        }
        if (!parse_string(ps, &n->str, &n->len)) {
            return NULL;
        }
        return n;
    case 't':
        if (!expect_lit(ps, "true")) {
            return fail(ps, "bad literal");
        }
        return new_node(ps, J_TRUE);
    case 'f':
        if (!expect_lit(ps, "false")) {
            return fail(ps, "bad literal");
        }
        return new_node(ps, J_FALSE);
    case 'n':
        if (!expect_lit(ps, "null")) {
            return fail(ps, "bad literal");
        }
        return new_node(ps, J_NULL);
    default:
        return parse_number(ps);
    }
}

JNode *json_parse(JDoc *doc, const char *text, size_t len)
{
    Parser ps;
    JNode *root;

    doc->chunks = NULL;
    doc->root = NULL;
    doc->err[0] = '\0';
    ps.doc = doc;
    ps.start = text;
    ps.p = text;
    ps.end = text + len;
    ps.depth = 0;
    root = parse_value(&ps);
    if (root != NULL) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            root = fail(&ps, "trailing characters");
        }
    }
    doc->root = root;
    return root;
}

/* ---------------------------------------------------------------- lookup */

JNode *json_get(const JNode *obj, const char *key)
{
    JNode *c;

    if (obj == NULL || obj->type != J_OBJ || key == NULL) {
        return NULL;
    }
    for (c = obj->first; c != NULL; c = c->next) {
        if (c->key != NULL && strcmp(c->key, key) == 0) {
            return c;
        }
    }
    return NULL;
}

JNode *json_at(const JNode *arr, int index)
{
    JNode *c;

    if (arr == NULL || arr->type != J_ARR || index < 0) {
        return NULL;
    }
    for (c = arr->first; c != NULL; c = c->next) {
        if (index-- == 0) {
            return c;
        }
    }
    return NULL;
}

const char *json_str(const JNode *n)
{
    return (n != NULL && n->type == J_STR) ? n->str : NULL;
}

int json_int(const JNode *n, long *out)
{
    if (n == NULL || n->type != J_NUM || !n->is_int) {
        return 0;
    }
    *out = n->ival;
    return 1;
}

int json_bool(const JNode *n, int *out)
{
    if (n == NULL || (n->type != J_TRUE && n->type != J_FALSE)) {
        return 0;
    }
    *out = (n->type == J_TRUE);
    return 1;
}

const char *json_get_str(const JNode *obj, const char *key, const char *def)
{
    const char *s = json_str(json_get(obj, key));

    return s != NULL ? s : def;
}

/* ---------------------------------------------------------------- writer */

static void jw_reserve(JWriter *w, size_t more)
{
    size_t cap;
    char *nb;

    if (w->failed) {
        return;
    }
    if (w->len + more + 1 <= w->cap) {
        return;
    }
    cap = w->cap ? w->cap : 256;
    while (cap < w->len + more + 1) {
        cap *= 2;
    }
    nb = (char *)realloc(w->buf, cap);
    if (nb == NULL) {
        w->failed = 1;
        return;
    }
    w->buf = nb;
    w->cap = cap;
}

static void jw_put(JWriter *w, const char *s, size_t n)
{
    jw_reserve(w, n);
    if (w->failed) {
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void jw_putc(JWriter *w, char c)
{
    jw_put(w, &c, 1);
}

void jw_init(JWriter *w)
{
    memset(w, 0, sizeof(*w));
}

void jw_free(JWriter *w)
{
    free(w->buf);
    memset(w, 0, sizeof(*w));
}

/* A value that follows jw_key has had its comma written by the key; otherwise write one if a sibling
 * already precedes it.  need_comma[d] is 0 (first element), 1 (siblings before) or -1 (a key was just
 * written, the value is next). */
static void jw_value_prefix(JWriter *w)
{
    if (w->depth > 0) {
        if (w->need_comma[w->depth - 1] == 1) {
            jw_putc(w, ',');
        } else if (w->need_comma[w->depth - 1] == -1) {
            w->need_comma[w->depth - 1] = 0;
        }
    }
}

static void jw_done_value(JWriter *w)
{
    if (w->depth > 0) {
        w->need_comma[w->depth - 1] = 1;
    }
}

static void jw_open(JWriter *w, char c)
{
    jw_value_prefix(w);
    jw_putc(w, c);
    if (w->depth >= JSON_MAX_DEPTH) {
        w->failed = 1;
        return;
    }
    w->need_comma[w->depth++] = 0;
}

static void jw_close(JWriter *w, char c)
{
    if (w->depth > 0) {
        w->depth--;
    }
    jw_putc(w, c);
    jw_done_value(w);
}

void jw_begin_obj(JWriter *w) { jw_open(w, '{'); }
void jw_end_obj(JWriter *w) { jw_close(w, '}'); }
void jw_begin_arr(JWriter *w) { jw_open(w, '['); }
void jw_end_arr(JWriter *w) { jw_close(w, ']'); }

static void jw_escaped(JWriter *w, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    unsigned char c;
    char u[6];

    jw_putc(w, '"');
    for (i = 0; i < n; i++) {
        c = (unsigned char)s[i];
        switch (c) {
        case '"': jw_put(w, "\\\"", 2); break;
        case '\\': jw_put(w, "\\\\", 2); break;
        case '\n': jw_put(w, "\\n", 2); break;
        case '\r': jw_put(w, "\\r", 2); break;
        case '\t': jw_put(w, "\\t", 2); break;
        case '\b': jw_put(w, "\\b", 2); break;
        case '\f': jw_put(w, "\\f", 2); break;
        default:
            if (c < 0x20) {
                u[0] = '\\'; u[1] = 'u'; u[2] = '0'; u[3] = '0';
                u[4] = hex[c >> 4]; u[5] = hex[c & 15];
                jw_put(w, u, 6);
            } else {
                jw_putc(w, (char)c);
            }
            break;
        }
    }
    jw_putc(w, '"');
}

void jw_key(JWriter *w, const char *key)
{
    if (w->depth > 0 && w->need_comma[w->depth - 1] == 1) {
        jw_putc(w, ',');
    }
    jw_escaped(w, key, strlen(key));
    jw_putc(w, ':');
    if (w->depth > 0) {
        w->need_comma[w->depth - 1] = -1;
    }
}

void jw_strn(JWriter *w, const char *s, size_t n)
{
    jw_value_prefix(w);
    jw_escaped(w, s, n);
    jw_done_value(w);
}

void jw_str(JWriter *w, const char *s)
{
    jw_strn(w, s, strlen(s));
}

void jw_int(JWriter *w, long v)
{
    char tmp[24];

    jw_value_prefix(w);
    sprintf(tmp, "%ld", v);
    jw_put(w, tmp, strlen(tmp));
    jw_done_value(w);
}

void jw_bool(JWriter *w, int v)
{
    jw_value_prefix(w);
    jw_put(w, v ? "true" : "false", v ? 4 : 5);
    jw_done_value(w);
}

void jw_null(JWriter *w)
{
    jw_value_prefix(w);
    jw_put(w, "null", 4);
    jw_done_value(w);
}
