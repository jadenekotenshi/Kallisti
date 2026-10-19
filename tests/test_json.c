#include <stdlib.h>
#include <string.h>
#include "../core/json.h"
#include "test.h"

static JNode *parse(JDoc *d, const char *s)
{
    return json_parse(d, s, strlen(s));
}

static void test_scalars(void)
{
    JDoc d;
    JNode *n;
    long v;
    int b;

    n = parse(&d, "  42 ");
    CHECK(n != NULL && n->type == J_NUM && json_int(n, &v) && v == 42);
    json_free(&d);
    n = parse(&d, "-7");
    CHECK(n != NULL && json_int(n, &v) && v == -7);
    json_free(&d);
    n = parse(&d, "0");
    CHECK(n != NULL && json_int(n, &v) && v == 0);
    json_free(&d);
    n = parse(&d, "1.5e2");
    CHECK(n != NULL && n->type == J_NUM && !n->is_int && n->dval == 150.0 && n->ival == 150);
    json_free(&d);
    /* too large for a long: still a number, not an integer */
    n = parse(&d, "4294967296");
    CHECK(n != NULL && n->type == J_NUM && !n->is_int && n->dval == 4294967296.0);
    CHECK(!json_int(n, &v));
    json_free(&d);
    n = parse(&d, "true");
    CHECK(n != NULL && json_bool(n, &b) && b == 1);
    json_free(&d);
    n = parse(&d, "false");
    CHECK(n != NULL && json_bool(n, &b) && b == 0);
    json_free(&d);
    n = parse(&d, "null");
    CHECK(n != NULL && n->type == J_NULL);
    json_free(&d);
}

static void test_strings(void)
{
    JDoc d;
    JNode *n;

    n = parse(&d, "\"plain\"");
    CHECK(n != NULL && n->type == J_STR && n->len == 5 && strcmp(n->str, "plain") == 0);
    json_free(&d);
    n = parse(&d, "\"a\\\"b\\\\c\\/d\\n\\t\\r\\b\\f\"");
    CHECK(n != NULL && strcmp(n->str, "a\"b\\c/d\n\t\r\b\f") == 0);
    json_free(&d);
    /* \u escapes: BMP, ASCII, NUL-free; U+00E9 is C3 A9, U+20AC is E2 82 AC */
    n = parse(&d, "\"caf\\u00e9 \\u20AC \\u0041\"");
    CHECK(n != NULL && strcmp(n->str, "caf\xC3\xA9 \xE2\x82\xAC A") == 0);
    json_free(&d);
    /* a surrogate pair is one 4-byte character: U+1F600 is F0 9F 98 80 */
    n = parse(&d, "\"\\ud83d\\ude00\"");
    CHECK(n != NULL && n->len == 4 && strcmp(n->str, "\xF0\x9F\x98\x80") == 0);
    json_free(&d);
    /* a lone surrogate becomes U+FFFD (EF BF BD) rather than an error */
    n = parse(&d, "\"x\\ud83dy\"");
    CHECK(n != NULL && strcmp(n->str, "x\xEF\xBF\xBDy") == 0);
    json_free(&d);
    n = parse(&d, "\"\\ude00\"");
    CHECK(n != NULL && strcmp(n->str, "\xEF\xBF\xBD") == 0);
    json_free(&d);
    /* raw UTF-8 passes through untouched */
    n = parse(&d, "\"\xE2\x9C\x93 ok\"");
    CHECK(n != NULL && strcmp(n->str, "\xE2\x9C\x93 ok") == 0);
    json_free(&d);
    n = parse(&d, "\"\"");
    CHECK(n != NULL && n->len == 0 && n->str[0] == '\0');
    json_free(&d);
}

static void test_containers(void)
{
    JDoc d;
    JNode *n, *a;
    long v;
    const char *msg =
        "{\"op\":0,\"s\":42,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"1234567890123456789\","
        "\"content\":\"hi \\u2603\",\"author\":{\"username\":\"Rat\",\"bot\":false},"
        "\"mentions\":[],\"attachments\":[{\"id\":\"1\"},{\"id\":\"2\"}]}}";

    n = parse(&d, msg);
    CHECK(n != NULL && n->type == J_OBJ && n->count == 4);
    CHECK(json_int(json_get(n, "op"), &v) && v == 0);
    CHECK(json_int(json_get(n, "s"), &v) && v == 42);
    CHECK(strcmp(json_get_str(n, "t", "?"), "MESSAGE_CREATE") == 0);
    CHECK(strcmp(json_get_str(json_get(n, "d"), "id", "?"), "1234567890123456789") == 0);
    CHECK(strcmp(json_get_str(json_get(n, "d"), "content", "?"), "hi \xE2\x98\x83") == 0);
    CHECK(strcmp(json_get_str(json_get(json_get(n, "d"), "author"), "username", "?"), "Rat") == 0);
    CHECK(json_get(n, "nope") == NULL);
    CHECK(strcmp(json_get_str(n, "nope", "dflt"), "dflt") == 0);
    a = json_get(json_get(n, "d"), "attachments");
    CHECK(a != NULL && a->type == J_ARR && a->count == 2);
    CHECK(strcmp(json_get_str(json_at(a, 1), "id", "?"), "2") == 0);
    CHECK(json_at(a, 2) == NULL && json_at(a, -1) == NULL);
    CHECK(json_get(json_get(n, "d"), "mentions")->count == 0);
    /* lookups on the wrong type are NULL-safe */
    CHECK(json_get(json_get(n, "op"), "x") == NULL);
    CHECK(json_at(json_get(n, "op"), 0) == NULL);
    CHECK(json_str(json_get(n, "op")) == NULL);
    CHECK(json_str(NULL) == NULL && json_get(NULL, "a") == NULL);
    json_free(&d);

    n = parse(&d, "[]");
    CHECK(n != NULL && n->type == J_ARR && n->count == 0);
    json_free(&d);
    n = parse(&d, "{}");
    CHECK(n != NULL && n->type == J_OBJ && n->count == 0);
    json_free(&d);
    n = parse(&d, "[1, [2, [3]], {\"a\": [ ]}]");
    CHECK(n != NULL && n->count == 3);
    json_free(&d);
}

static void test_errors(void)
{
    static const char *bad[] = {
        "", "   ", "{", "[", "[1,", "[1 2]", "{\"a\"}", "{\"a\":}", "{a:1}", "{\"a\":1,}", "[1,]",
        "\"abc", "\"a\\q\"", "\"\\u12\"", "\"\\u12zz\"", "tru", "nul", "01x", "-", "1.", "1e", ".5",
        "{} {}", "[1] x", "}", "]", "\"\\", "{\"a\":1 \"b\":2}", NULL
    };
    JDoc d;
    int i;

    for (i = 0; bad[i] != NULL; i++) {
        CHECK(parse(&d, bad[i]) == NULL && d.err[0] != '\0');
        json_free(&d);
    }
    /* depth limit */
    {
        char deep[JSON_MAX_DEPTH + 8 + 1];
        int k;
        for (k = 0; k < JSON_MAX_DEPTH + 8; k++) {
            deep[k] = '[';
        }
        deep[k] = '\0';
        CHECK(parse(&d, deep) == NULL);
        json_free(&d);
    }
    /* exactly the limit, closed, parses */
    {
        char ok[2 * JSON_MAX_DEPTH + 1];
        int k;
        for (k = 0; k < JSON_MAX_DEPTH; k++) {
            ok[k] = '[';
            ok[2 * JSON_MAX_DEPTH - 1 - k] = ']';
        }
        ok[2 * JSON_MAX_DEPTH] = '\0';
        CHECK(parse(&d, ok) != NULL);
        json_free(&d);
    }
}

static void test_large(void)
{
    /* a document well past one arena chunk, so pointers must stay valid across chunk growth */
    JDoc d;
    JNode *n, *c;
    JWriter w;
    int i, ok = 1;
    char key[32];

    jw_init(&w);
    jw_begin_arr(&w);
    for (i = 0; i < 2000; i++) {
        jw_begin_obj(&w);
        jw_key(&w, "id");
        sprintf(key, "%d", i);
        jw_str(&w, key);
        jw_key(&w, "name");
        jw_str(&w, "channel-name-with-some-length-to-fill-the-arena-faster");
        jw_end_obj(&w);
    }
    jw_end_arr(&w);
    CHECK(!w.failed && w.depth == 0);
    n = json_parse(&d, w.buf, w.len);
    CHECK(n != NULL && n->count == 2000);
    i = 0;
    for (c = n->first; c != NULL; c = c->next, i++) {
        sprintf(key, "%d", i);
        if (strcmp(json_get_str(c, "id", ""), key) != 0) {
            ok = 0;
        }
    }
    CHECK(ok && i == 2000);
    json_free(&d);
    jw_free(&w);
}

static void test_writer(void)
{
    JWriter w;

    /* the shape of a gateway IDENTIFY */
    jw_init(&w);
    jw_begin_obj(&w);
    jw_key(&w, "op"); jw_int(&w, 2);
    jw_key(&w, "d");
    jw_begin_obj(&w);
    jw_key(&w, "token"); jw_str(&w, "abc");
    jw_key(&w, "intents"); jw_int(&w, 37377);
    jw_key(&w, "properties");
    jw_begin_obj(&w);
    jw_key(&w, "os"); jw_str(&w, "openstep");
    jw_end_obj(&w);
    jw_key(&w, "tags");
    jw_begin_arr(&w);
    jw_str(&w, "a"); jw_bool(&w, 1); jw_null(&w); jw_int(&w, -3);
    jw_end_arr(&w);
    jw_end_obj(&w);
    jw_end_obj(&w);
    CHECK(!w.failed && w.depth == 0);
    CHECK(strcmp(w.buf, "{\"op\":2,\"d\":{\"token\":\"abc\",\"intents\":37377,"
                        "\"properties\":{\"os\":\"openstep\"},\"tags\":[\"a\",true,null,-3]}}") == 0);
    jw_free(&w);

    /* escaping: quotes, backslash, newline, control char; non-ASCII UTF-8 passes through */
    jw_init(&w);
    jw_begin_obj(&w);
    jw_key(&w, "content");
    jw_str(&w, "say \"hi\"\\\n\x01\xC3\xA9");
    jw_end_obj(&w);
    CHECK(strcmp(w.buf, "{\"content\":\"say \\\"hi\\\"\\\\\\n\\u0001\xC3\xA9\"}") == 0);
    jw_free(&w);

    /* writer output is read back by the parser */
    {
        JDoc d;
        JNode *n;
        const char *text = "line1\nline2 \"q\" \\ tab\t end \xE2\x98\x83";
        jw_init(&w);
        jw_begin_obj(&w);
        jw_key(&w, "content");
        jw_str(&w, text);
        jw_end_obj(&w);
        n = json_parse(&d, w.buf, w.len);
        CHECK(n != NULL && strcmp(json_get_str(n, "content", ""), text) == 0);
        json_free(&d);
        jw_free(&w);
    }

    /* empty containers and a top-level scalar */
    jw_init(&w);
    jw_begin_arr(&w);
    jw_begin_obj(&w); jw_end_obj(&w);
    jw_begin_arr(&w); jw_end_arr(&w);
    jw_end_arr(&w);
    CHECK(strcmp(w.buf, "[{},[]]") == 0);
    jw_free(&w);
    jw_init(&w);
    jw_int(&w, 7);
    CHECK(strcmp(w.buf, "7") == 0);
    jw_free(&w);

    /* nesting past the limit marks the writer failed instead of overflowing */
    {
        int k;
        jw_init(&w);
        for (k = 0; k < JSON_MAX_DEPTH + 4; k++) {
            jw_begin_arr(&w);
        }
        CHECK(w.failed);
        jw_free(&w);
    }
}

int main(void)
{
    test_scalars();
    test_strings();
    test_containers();
    test_errors();
    test_large();
    test_writer();
    TEST_DONE("json");
}
