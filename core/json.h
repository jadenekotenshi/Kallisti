/*
 * Small JSON reader/writer for Discord's gateway and REST payloads.
 * Portable C89 (no stdint, no 64-bit integers: snowflake ids are JSON strings).
 *
 * Reading: json_parse() builds a read-only tree of nodes inside a JDoc; strings are
 * unescaped (including \uXXXX and surrogate pairs, to UTF-8) into the document's own
 * string pool.  Everything is freed with json_free().
 *
 * Writing: a JWriter appends compact JSON to a growing buffer.
 */
#ifndef KALLISTI_JSON_H
#define KALLISTI_JSON_H

#include <stddef.h>

#define JSON_MAX_DEPTH 48

typedef enum {
    J_NULL = 0, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ
} JType;

typedef struct JNode {
    JType type;
    const char *str;         /* J_STR: NUL-terminated, UTF-8.  Object member: its key is in key. */
    size_t len;              /* J_STR: byte length without the NUL */
    const char *key;         /* member of an object: the member name, else NULL */
    long ival;               /* J_NUM: the integer value (truncated toward zero) */
    double dval;             /* J_NUM: the value as a double */
    int is_int;              /* J_NUM: written without fraction/exponent and fits a long */
    struct JNode *first;     /* J_ARR / J_OBJ: first child */
    struct JNode *next;      /* next sibling */
    int count;               /* J_ARR / J_OBJ: number of children */
} JNode;

typedef struct JChunk {
    struct JChunk *next;
    size_t used, cap;
    char data[8];            /* really cap bytes; chunks never move, so node/string pointers stay valid */
} JChunk;

typedef struct {
    JChunk *chunks;          /* arena holding every node and string of the tree */
    JNode *root;
    char err[96];
} JDoc;

/* Parse text[0..len).  Returns the root node, or NULL (message in doc->err).  The document must be
 * released with json_free() either way. */
JNode *json_parse(JDoc *doc, const char *text, size_t len);
void json_free(JDoc *doc);

/* Lookup helpers; all are NULL-safe and return NULL / 0 for a missing or mistyped value. */
JNode *json_get(const JNode *obj, const char *key);
JNode *json_at(const JNode *arr, int index);
const char *json_str(const JNode *n);                    /* NULL unless a string */
int json_int(const JNode *n, long *out);                 /* 1 and *out set if an integer number */
int json_bool(const JNode *n, int *out);                 /* 1 and *out set if true/false */
/* Convenience: string member, or def. */
const char *json_get_str(const JNode *obj, const char *key, const char *def);

/* ---- writer ---- */
typedef struct {
    char *buf;
    size_t len, cap;
    int need_comma[JSON_MAX_DEPTH];
    int depth;
    int failed;              /* out of memory or nested too deep: the output is not valid */
} JWriter;

void jw_init(JWriter *w);
void jw_free(JWriter *w);
void jw_begin_obj(JWriter *w);
void jw_end_obj(JWriter *w);
void jw_begin_arr(JWriter *w);
void jw_end_arr(JWriter *w);
void jw_key(JWriter *w, const char *key);                /* inside an object, before the value */
void jw_str(JWriter *w, const char *s);                  /* UTF-8 in, escaped out */
void jw_strn(JWriter *w, const char *s, size_t n);
void jw_int(JWriter *w, long v);
void jw_bool(JWriter *w, int v);
void jw_null(JWriter *w);

#endif
