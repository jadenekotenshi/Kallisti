/* The client's picture of Discord's servers, channels and messages: see model.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"

/* ---------------------------------------------------------------- small helpers */

/* Copies at most cap-1 bytes, never leaving half a UTF-8 sequence at the end. */
static void copy_utf8(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);

    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
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

int model_id_cmp(const char *a, const char *b)
{
    size_t la, lb;

    while (*a == '0' && a[1] != '\0') {
        a++;
    }
    while (*b == '0' && b[1] != '\0') {
        b++;
    }
    la = strlen(a);
    lb = strlen(b);
    if (la != lb) {
        return la < lb ? -1 : 1;
    }
    return strcmp(a, b);
}

/* A snowflake member: its string, or NULL (Discord sends ids as strings, but be forgiving of a number). */
static const char *id_of(const JNode *obj, const char *key)
{
    const char *s = json_get_str(obj, key, NULL);

    return valid_id(s) ? s : NULL;
}

static int int_of(const JNode *obj, const char *key, int def)
{
    long v;

    if (json_int(json_get(obj, key), &v)) {
        return (int)v;
    }
    return def;
}

static void push_change(Model *m, int type, const char *guild_id, const char *channel_id, const char *message_id)
{
    ModelChange *c;

    if (m->nchanges >= MODEL_MAX_CHANGES) {
        if (!m->overflow) {
            m->overflow = 1;
        }
        return;
    }
    c = &m->changes[m->nchanges++];
    memset(c, 0, sizeof(*c));
    c->type = type;
    if (guild_id != NULL) {
        copy_utf8(c->guild_id, sizeof(c->guild_id), guild_id);
    }
    if (channel_id != NULL) {
        copy_utf8(c->channel_id, sizeof(c->channel_id), channel_id);
    }
    if (message_id != NULL) {
        copy_utf8(c->message_id, sizeof(c->message_id), message_id);
    }
}

int model_next_change(Model *m, ModelChange *out)
{
    int i;

    if (m->overflow && m->nchanges == 0) {
        memset(out, 0, sizeof(*out));
        out->type = MC_RESET;
        m->overflow = 0;
        return 1;
    }
    if (m->nchanges == 0) {
        return 0;
    }
    *out = m->changes[0];
    for (i = 1; i < m->nchanges; i++) {
        m->changes[i - 1] = m->changes[i];
    }
    m->nchanges--;
    return 1;
}

/* ---------------------------------------------------------------- lifecycle */

static void message_clear(Message *m)
{
    free(m->text);
    free(m->att);
    free(m->embeds);
    m->embeds = NULL;
    free(m->react);
    m->react = NULL;
    m->nreact = 0;
    free(m->emoji);
    m->emoji = NULL;
    m->nemoji = 0;
    m->text = NULL;
    m->att = NULL;
    m->natt = 0;
}

void model_init(Model *m)
{
    memset(m, 0, sizeof(*m));
}

static void channel_free(Channel *c)
{
    int i;

    for (i = 0; i < c->nmsgs; i++) {
        message_clear(&c->msgs[i]);
    }
    free(c->msgs);
    memset(c, 0, sizeof(*c));
}

static void guild_free(Guild *g)
{
    int i;

    for (i = 0; i < g->nch; i++) {
        channel_free(&g->ch[i]);
    }
    free(g->ch);
    memset(g, 0, sizeof(*g));
}

void model_free(Model *m)
{
    int i;

    for (i = 0; i < m->nguilds; i++) {
        guild_free(&m->guilds[i]);
    }
    free(m->guilds);
    memset(m, 0, sizeof(*m));
}

/* ---------------------------------------------------------------- lookup */

Guild *model_find_guild(Model *m, const char *id)
{
    int i;

    for (i = 0; i < m->nguilds; i++) {
        if (strcmp(m->guilds[i].id, id) == 0) {
            return &m->guilds[i];
        }
    }
    return NULL;
}

static Channel *guild_find_channel(Guild *g, const char *id)
{
    int i;

    for (i = 0; i < g->nch; i++) {
        if (strcmp(g->ch[i].id, id) == 0) {
            return &g->ch[i];
        }
    }
    return NULL;
}

Channel *model_find_channel(Model *m, const char *id)
{
    int i;
    Channel *c;

    for (i = 0; i < m->nguilds; i++) {
        c = guild_find_channel(&m->guilds[i], id);
        if (c != NULL) {
            return c;
        }
    }
    return NULL;
}

Message *model_find_message(Channel *c, const char *id)
{
    int i;

    for (i = c->nmsgs - 1; i >= 0; i--) {
        if (strcmp(c->msgs[i].id, id) == 0) {
            return &c->msgs[i];
        }
    }
    return NULL;
}

static Guild *guild_upsert(Model *m, const char *id, int *created)
{
    Guild *g = model_find_guild(m, id);

    if (created != NULL) {
        *created = 0;
    }
    if (g != NULL) {
        return g;
    }
    if (m->nguilds >= m->gcap) {
        int ncap = m->gcap ? m->gcap * 2 : 8;
        Guild *ng = (Guild *)realloc(m->guilds, (size_t)ncap * sizeof(Guild));

        if (ng == NULL) {
            return NULL;
        }
        m->guilds = ng;
        m->gcap = ncap;
    }
    g = &m->guilds[m->nguilds++];
    memset(g, 0, sizeof(*g));
    copy_utf8(g->id, sizeof(g->id), id);
    if (created != NULL) {
        *created = 1;
    }
    return g;
}

static void guild_remove(Model *m, int index)
{
    guild_free(&m->guilds[index]);
    memmove(&m->guilds[index], &m->guilds[index + 1], (size_t)(m->nguilds - index - 1) * sizeof(Guild));
    m->nguilds--;
}

static Channel *channel_upsert(Guild *g, const char *id, int *created)
{
    Channel *c = guild_find_channel(g, id);

    *created = 0;
    if (c != NULL) {
        return c;
    }
    if (g->nch >= g->chcap) {
        int ncap = g->chcap ? g->chcap * 2 : 16;
        Channel *nc = (Channel *)realloc(g->ch, (size_t)ncap * sizeof(Channel));

        if (nc == NULL) {
            return NULL;
        }
        g->ch = nc;
        g->chcap = ncap;
    }
    c = &g->ch[g->nch++];
    memset(c, 0, sizeof(*c));
    copy_utf8(c->id, sizeof(c->id), id);
    copy_utf8(c->guild_id, sizeof(c->guild_id), g->id);
    *created = 1;
    return c;
}

static void channel_remove(Guild *g, int index)
{
    channel_free(&g->ch[index]);
    memmove(&g->ch[index], &g->ch[index + 1], (size_t)(g->nch - index - 1) * sizeof(Channel));
    g->nch--;
}

/* ---------------------------------------------------------------- channel ordering */

typedef struct {
    int rank;                    /* 0: channels outside any category, 1: categories and their children */
    int gpos;
    char gid[24];
    int is_child;
    int pos;
    char id[24];
    int index;
} SortRec;

static int sortrec_cmp(const void *pa, const void *pb)
{
    const SortRec *a = (const SortRec *)pa, *b = (const SortRec *)pb;
    int d;

    if (a->rank != b->rank) {
        return a->rank < b->rank ? -1 : 1;
    }
    if (a->gpos != b->gpos) {
        return a->gpos < b->gpos ? -1 : 1;
    }
    d = model_id_cmp(a->gid, b->gid);
    if (d != 0) {
        return d;
    }
    if (a->is_child != b->is_child) {
        return a->is_child < b->is_child ? -1 : 1;
    }
    if (a->pos != b->pos) {
        return a->pos < b->pos ? -1 : 1;
    }
    return model_id_cmp(a->id, b->id);
}

/* Display order, as Discord shows it: channels without a category first, then each category followed by its
 * channels, all by position (ties by id). */
static void sort_channels(Guild *g)
{
    SortRec *recs;
    Channel *out;
    int i, j;

    if (g->nch < 2) {
        return;
    }
    recs = (SortRec *)malloc((size_t)g->nch * sizeof(SortRec));
    out = (Channel *)malloc((size_t)g->nch * sizeof(Channel));
    if (recs == NULL || out == NULL) {
        free(recs);
        free(out);
        return;
    }
    for (i = 0; i < g->nch; i++) {
        Channel *c = &g->ch[i];
        Channel *parent = c->parent_id[0] != '\0' ? guild_find_channel(g, c->parent_id) : NULL;
        SortRec *r = &recs[i];

        memset(r, 0, sizeof(*r));
        r->index = i;
        r->pos = c->position;
        strcpy(r->id, c->id);
        if (c->type == CH_CATEGORY) {
            r->rank = 1;
            r->gpos = c->position;
            strcpy(r->gid, c->id);
        } else if (parent != NULL && parent->type == CH_CATEGORY) {
            r->rank = 1;
            r->gpos = parent->position;
            strcpy(r->gid, parent->id);
            r->is_child = 1;
        } else {
            r->rank = 0;
            r->gpos = c->position;
            strcpy(r->gid, c->id);
        }
    }
    qsort(recs, (size_t)g->nch, sizeof(SortRec), sortrec_cmp);
    for (j = 0; j < g->nch; j++) {
        out[j] = g->ch[recs[j].index];
    }
    memcpy(g->ch, out, (size_t)g->nch * sizeof(Channel));
    free(recs);
    free(out);
}

/* ---------------------------------------------------------------- message text */

typedef struct {
    char *buf;
    size_t len, cap;
    int failed;
} Str;

static void str_add(Str *s, const char *p, size_t n)
{
    if (s->failed) {
        return;
    }
    if (s->len + n + 1 > s->cap) {
        size_t ncap = s->cap ? s->cap : 64;
        char *nb;

        while (ncap < s->len + n + 1) {
            ncap *= 2;
        }
        nb = (char *)realloc(s->buf, ncap);
        if (nb == NULL) {
            s->failed = 1;
            return;
        }
        s->buf = nb;
        s->cap = ncap;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void str_adds(Str *s, const char *p)
{
    str_add(s, p, strlen(p));
}

/* Recognises a Discord tag at s[0] == '<': <@id>, <@!id>, <@&id>, <#id>, <:name:id>, <a:name:id>.
 * Returns its length, or 0. kind is 'u', 'r', 'c' or 'e'; id and name are filled. */
static size_t parse_tag(const char *s, int *kind, char *id, char *name)
{
    size_t i = 1, n;

    id[0] = name[0] = '\0';
    if (s[i] == '@') {
        i++;
        if (s[i] == '&') {
            *kind = 'r';
            i++;
        } else {
            *kind = 'u';
            if (s[i] == '!') {
                i++;
            }
        }
    } else if (s[i] == '#') {
        *kind = 'c';
        i++;
    } else if (s[i] == ':' || (s[i] == 'a' && s[i + 1] == ':')) {
        *kind = 'e';
        i += s[i] == 'a' ? 2 : 1;
        n = 0;
        while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') ||
               s[i] == '_') {
            if (n < 63) {
                name[n++] = s[i];
            }
            i++;
        }
        name[n] = '\0';
        if (n == 0 || s[i] != ':') {
            return 0;
        }
        i++;
    } else {
        return 0;
    }
    n = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        if (n < 20) {
            id[n++] = s[i];
        }
        i++;
    }
    id[n] = '\0';
    if (n == 0 || n > 20 || s[i] != '>') {
        return 0;
    }
    return i + 1;
}

static const char *user_display(const JNode *user)
{
    const char *n = json_get_str(json_get(user, "member"), "nick", NULL);

    if (n == NULL || n[0] == '\0') {
        n = json_get_str(user, "global_name", NULL);
    }
    if (n == NULL || n[0] == '\0') {
        n = json_get_str(user, "username", NULL);
    }
    return n != NULL && n[0] != '\0' ? n : "unknown";
}

static int is_image_attachment(const JNode *a)
{
    const char *ct = json_get_str(a, "content_type", NULL);

    if (ct != NULL) {
        return strncmp(ct, "image/", 6) == 0;
    }
    return int_of(a, "width", 0) > 0 && int_of(a, "height", 0) > 0;
}

/* The text to show for a message object, in a malloc'd string (NULL if out of memory). */
/* `emo` (room for MODEL_MAX_EMOJI) and *nemo receive where the custom emoji ended up in the text. */
static char *render_text(Model *m, const JNode *msg, Emoji *emo, int *nemo)
{
    Str out;
    const char *content = json_get_str(msg, "content", "");
    const JNode *mentions = json_get(msg, "mentions"), *a;
    const char *p = content;
    int kind, i, natt = 0, nstk;
    char id[32], name[80];
    size_t n;

    memset(&out, 0, sizeof(out));
    *nemo = 0;
    while (*p != '\0' && !out.failed) {
        if (*p == '<' && (n = parse_tag(p, &kind, id, name)) > 0) {
            if (kind == 'u') {
                const JNode *u;
                const char *shown = NULL;

                for (u = mentions != NULL ? mentions->first : NULL; u != NULL; u = u->next) {
                    if (strcmp(json_get_str(u, "id", ""), id) == 0) {
                        shown = user_display(u);
                        break;
                    }
                }
                str_adds(&out, "@");
                str_adds(&out, shown != NULL ? shown : "user");
            } else if (kind == 'r') {
                str_adds(&out, "@role");
            } else if (kind == 'c') {
                Channel *c = model_find_channel(m, id);

                str_adds(&out, "#");
                str_adds(&out, c != NULL ? c->name : "channel");
            } else {
                if (*nemo < MODEL_MAX_EMOJI && strlen(id) < sizeof(emo[0].id)) {
                    emo[*nemo].off = (int)out.len;
                    emo[*nemo].len = (int)strlen(name) + 2;
                    strcpy(emo[*nemo].id, id);
                    emo[*nemo].animated = p[1] == 'a';
                    (*nemo)++;
                }
                str_adds(&out, ":");
                str_adds(&out, name);
                str_adds(&out, ":");
            }
            p += n;
        } else {
            str_add(&out, p, 1);
            p++;
        }
    }
    for (a = json_get(msg, "attachments") != NULL ? json_get(msg, "attachments")->first : NULL; a != NULL; a = a->next) {
        if (natt++ < 5) {
            if (out.len > 0) {
                str_adds(&out, "\n");
            }
            if (is_image_attachment(a)) {
                int aw = int_of(a, "width", 0), ah = int_of(a, "height", 0);

                str_adds(&out, "[image: ");
                str_adds(&out, json_get_str(a, "filename", "file"));
                if (aw > 0 && ah > 0) {
                    char dims[40];

                    sprintf(dims, " %dx%d", aw, ah);
                    str_adds(&out, dims);
                }
                str_adds(&out, "]");
            } else {
                str_adds(&out, "[attachment: ");
                str_adds(&out, json_get_str(a, "filename", "file"));
                str_adds(&out, "]");
            }
        }
    }
    {
        const JNode *stickers = json_get(msg, "sticker_items"), *st;
        int shown = 0;

        for (st = stickers != NULL && stickers->type == J_ARR ? stickers->first : NULL; st != NULL && shown < 3; st = st->next, shown++) {
            const char *nm = json_get_str(st, "name", "");

            if (out.len > 0) {
                str_adds(&out, "\n");
            }
            if (nm[0] != '\0') {
                str_adds(&out, "[sticker: ");
                str_adds(&out, nm);
                str_adds(&out, "]");
            } else {
                str_adds(&out, "[sticker]");
            }
        }
    }
    if (out.len == 0) {
        nstk = 0;
        i = int_of(msg, "type", 0);
        if (nstk > 0) {
            str_adds(&out, "[sticker]");
        } else if (i != 0 && i != 19 && i != 20) {
            str_adds(&out, "[system message]");
        } else {
            str_adds(&out, "");
        }
    }
    if (out.failed) {
        free(out.buf);
        return NULL;
    }
    if (out.buf == NULL) {
        out.buf = (char *)malloc(1);
        if (out.buf == NULL) {
            return NULL;
        }
        out.buf[0] = '\0';
    }
    return out.buf;
}

/* ---------------------------------------------------------------- messages */

static int mentions_user(const JNode *msg, const char *user_id)
{
    const JNode *u = json_get(msg, "mentions");

    if (user_id[0] == '\0') {
        return 0;
    }
    for (u = u != NULL ? u->first : NULL; u != NULL; u = u->next) {
        if (strcmp(json_get_str(u, "id", ""), user_id) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- direct messages */

/* The pseudo-server for DMs, created if need be and moved to the front of the list. */
static Guild *dm_guild(Model *m)
{
    int created;
    Guild *g = guild_upsert(m, MODEL_DM_GUILD, &created);

    if (g == NULL) {
        return NULL;
    }
    if (created) {
        Guild tmp;
        int i = (int)(g - m->guilds);

        copy_utf8(g->name, sizeof(g->name), "Direct Messages");
        tmp = *g;
        memmove(&m->guilds[1], &m->guilds[0], (size_t)i * sizeof(Guild));
        m->guilds[0] = tmp;
        g = &m->guilds[0];
        push_change(m, MC_GUILDS, NULL, NULL, NULL);
    }
    return g;
}

static void dm_name(char *out, size_t cap, const JNode *chan, const char *self_id)
{
    const char *name = json_get_str(chan, "name", NULL);
    const JNode *r = json_get(chan, "recipients");
    const JNode *u;
    size_t used = 0;

    out[0] = '\0';
    if (name != NULL && name[0] != '\0') {
        copy_utf8(out, cap, name);
        return;
    }
    for (u = r != NULL ? r->first : NULL; u != NULL; u = u->next) {
        const char *n = user_display(u);
        size_t l = strlen(n);

        if (strcmp(json_get_str(u, "id", ""), self_id) == 0) {
            continue;
        }
        if (used > 0 && used + 2 < cap) {
            out[used++] = ',';
            out[used++] = ' ';
        }
        if (used + l + 1 > cap) {
            break;
        }
        memcpy(out + used, n, l);
        used += l;
        out[used] = '\0';
    }
    if (out[0] == '\0') {
        copy_utf8(out, cap, "unknown");
    }
}

void model_ingest_private_channel(Model *m, const JNode *chan)
{
    const char *id = id_of(chan, "id");
    int type = int_of(chan, "type", -1), created;
    Guild *g;
    Channel *c;

    if (id == NULL || (type != CH_DM && type != CH_GROUP_DM)) {
        return;
    }
    g = dm_guild(m);
    if (g == NULL) {
        return;
    }
    c = channel_upsert(g, id, &created);
    if (c == NULL) {
        return;
    }
    c->type = type;
    c->position = 0;
    c->parent_id[0] = '\0';
    dm_name(c->name, sizeof(c->name), chan, m->user_id);
    sort_channels(g);
    push_change(m, MC_CHANNELS, g->id, NULL, NULL);
}

/* A message arrived in a DM we have no channel for: make the channel from what the message tells us. */
static Channel *dm_from_message(Model *m, const char *cid, const JNode *author)
{
    Guild *g;
    Channel *c;
    int created;

    g = dm_guild(m);
    if (g == NULL) {
        return NULL;
    }
    c = channel_upsert(g, cid, &created);
    if (c == NULL) {
        return NULL;
    }
    c->type = CH_DM;
    c->position = 0;
    if (strcmp(json_get_str(author, "id", ""), m->user_id) == 0) {
        copy_utf8(c->name, sizeof(c->name), "conversation");      /* we spoke first, from another client */
    } else {
        copy_utf8(c->name, sizeof(c->name), user_display(author));
    }
    sort_channels(g);
    push_change(m, MC_CHANNELS, g->id, NULL, NULL);
    return model_find_channel(m, cid);
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix), i;

    if (n < m) {
        return 0;
    }
    for (i = 0; i < m; i++) {
        char a = s[n - m + i], b = suffix[i];

        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- reactions */

/* Reads an emoji object ({id, name, animated}) into r's name, id and animated. */
static int reaction_emoji(Reaction *r, const JNode *e)
{
    const char *id = id_of(e, "id"), *name = json_get_str(e, "name", NULL);
    int b = 0;

    memset(r, 0, sizeof(*r));
    if (name == NULL && id == NULL) {
        return 0;
    }
    copy_utf8(r->name, sizeof(r->name), name != NULL ? name : "");
    copy_utf8(r->id, sizeof(r->id), id != NULL ? id : "");
    r->animated = json_bool(json_get(e, "animated"), &b) ? b : 0;
    return 1;
}

static int reaction_same(const Reaction *a, const Reaction *b)
{
    return a->id[0] != '\0' || b->id[0] != '\0' ? strcmp(a->id, b->id) == 0 : strcmp(a->name, b->name) == 0;
}

static void parse_reactions(Message *msg, const JNode *arr)
{
    const JNode *x;
    int b;

    if (arr == NULL || arr->type != J_ARR || arr->count == 0) {
        return;
    }
    msg->react = (Reaction *)calloc(MODEL_MAX_REACTS, sizeof(Reaction));
    if (msg->react == NULL) {
        return;
    }
    for (x = arr->first; x != NULL && msg->nreact < MODEL_MAX_REACTS; x = x->next) {
        Reaction *r = &msg->react[msg->nreact];

        if (!reaction_emoji(r, json_get(x, "emoji"))) {
            continue;
        }
        r->count = int_of(x, "count", 1);
        b = 0;
        r->me = json_bool(json_get(x, "me"), &b) ? b : 0;
        msg->nreact++;
    }
    if (msg->nreact == 0) {
        free(msg->react);
        msg->react = NULL;
    }
}

/* MESSAGE_REACTION_ADD / _REMOVE / _REMOVE_ALL / _REMOVE_EMOJI. */
static void on_reaction(Model *m, const char *kind, const JNode *d)
{
    const char *cid = id_of(d, "channel_id"), *mid = id_of(d, "message_id"), *uid = id_of(d, "user_id");
    Channel *c = cid != NULL ? model_find_channel(m, cid) : NULL;
    Message *msg = (c != NULL && mid != NULL) ? model_find_message(c, mid) : NULL;
    Reaction r;
    int i, mine;

    if (msg == NULL) {
        return;
    }
    if (strcmp(kind, "REMOVE_ALL") == 0) {
        free(msg->react);
        msg->react = NULL;
        msg->nreact = 0;
        push_change(m, MC_MESSAGE_CHANGED, c->guild_id, c->id, mid);
        return;
    }
    if (!reaction_emoji(&r, json_get(d, "emoji"))) {
        return;
    }
    mine = uid != NULL && strcmp(uid, m->user_id) == 0;
    for (i = 0; i < msg->nreact; i++) {
        if (reaction_same(&msg->react[i], &r)) {
            break;
        }
    }
    if (strcmp(kind, "ADD") == 0) {
        if (i == msg->nreact) {
            if (msg->nreact >= MODEL_MAX_REACTS) {
                return;
            }
            if (msg->react == NULL) {
                msg->react = (Reaction *)calloc(MODEL_MAX_REACTS, sizeof(Reaction));
                if (msg->react == NULL) {
                    return;
                }
            }
            msg->react[i] = r;
            msg->react[i].count = 0;
            msg->nreact++;
        }
        msg->react[i].count++;
        if (mine) {
            msg->react[i].me = 1;
        }
    } else {
        if (i == msg->nreact) {
            return;
        }
        if (strcmp(kind, "REMOVE_EMOJI") == 0) {
            msg->react[i].count = 0;
        } else {
            msg->react[i].count--;
            if (mine) {
                msg->react[i].me = 0;
            }
        }
        if (msg->react[i].count <= 0) {
            memmove(&msg->react[i], &msg->react[i + 1], (size_t)(msg->nreact - i - 1) * sizeof(Reaction));
            msg->nreact--;
            if (msg->nreact == 0) {
                free(msg->react);
                msg->react = NULL;
            }
        }
    }
    push_change(m, MC_MESSAGE_CHANGED, c->guild_id, c->id, mid);
}

/* ---------------------------------------------------------------- embeds */

#define MAX_EMBEDS_SHOWN 3

/* copies at most `max` bytes of text onto the string, cut on a character boundary, "..." if cut */
static void add_cut(Str *s, const char *text, size_t max)
{
    size_t n = strlen(text);

    if (n > max) {
        n = max;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) {
            n--;
        }
        str_add(s, text, n);
        str_adds(s, "...");
    } else {
        str_add(s, text, n);
    }
}

/* one line "| text" for each line of `text` (at most `lines` of them, each cut at `width` bytes) */
static void add_quoted(Str *s, const char *text, int lines, size_t width)
{
    const char *p = text;

    while (*p != '\0' && lines-- > 0) {
        size_t n = strcspn(p, "\n");
        char *line = (char *)malloc(n + 1);

        if (line == NULL) {
            return;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        if (n > 0 && line[n - 1] == '\r') {
            line[n - 1] = '\0';
        }
        if (line[0] != '\0') {
            str_adds(s, "| ");
            add_cut(s, line, width);
            str_adds(s, "\n");
        }
        free(line);
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
}

/* The picture of an embed (its image, else its thumbnail), as an Attachment; 0 if it has none. */
static int embed_picture(const JNode *e, Attachment *a)
{
    const JNode *pic = json_get(e, "image");
    const char *url, *slash, *q;
    size_t n;

    if (pic == NULL || pic->type != J_OBJ) {
        pic = json_get(e, "thumbnail");
    }
    if (pic == NULL || pic->type != J_OBJ) {
        return 0;
    }
    url = json_get_str(pic, "proxy_url", NULL);
    if (url == NULL || url[0] == '\0') {
        url = json_get_str(pic, "url", "");
    }
    if (url[0] == '\0' || strlen(url) >= sizeof(a->url)) {
        return 0;
    }
    memset(a, 0, sizeof(*a));
    strcpy(a->url, url);
    q = strchr(url, '?');
    n = q ? (size_t)(q - url) : strlen(url);
    slash = url + n;
    while (slash > url && slash[-1] != '/') {
        slash--;
    }
    if ((size_t)(url + n - slash) > 0 && (size_t)(url + n - slash) < sizeof(a->name)) {
        memcpy(a->name, slash, (size_t)(url + n - slash));
        a->name[url + n - slash] = '\0';
    } else {
        copy_utf8(a->name, sizeof(a->name), json_get_str(e, "title", "picture"));
    }
    a->width = int_of(pic, "width", 0);
    a->height = int_of(pic, "height", 0);
    a->is_image = 1;
    a->is_jpeg = ends_with_ci(a->name, ".jpg") || ends_with_ci(a->name, ".jpeg");
    a->is_gif = ends_with_ci(a->name, ".gif");
    a->from_embed = 1;
    return 1;
}

/* The embeds as plain lines, or NULL if there is nothing to show. */
static char *render_embeds(const JNode *embeds)
{
    Str out;
    const JNode *e, *f;
    int shown = 0, total = 0;

    memset(&out, 0, sizeof(out));
    if (embeds == NULL || embeds->type != J_ARR) {
        return NULL;
    }
    for (e = embeds->first; e != NULL; e = e->next) {
        const char *type = json_get_str(e, "type", "rich");
        const char *title = json_get_str(e, "title", NULL), *desc = json_get_str(e, "description", NULL);
        const char *who = json_get_str(json_get(e, "author"), "name", NULL);
        const char *footer = json_get_str(json_get(e, "footer"), "text", NULL);
        const JNode *fields = json_get(e, "fields");
        Attachment pic;
        int nf = 0;

        total++;
        if (shown >= MAX_EMBEDS_SHOWN) {
            continue;
        }
        if (who == NULL) {
            who = json_get_str(json_get(e, "provider"), "name", NULL);
        }
        if (strcmp(type, "image") != 0) {                  /* a bare image link says nothing but its picture */
            if (strcmp(type, "video") == 0 || strcmp(type, "gifv") == 0) {
                str_adds(&out, "| [video]");
                if (title != NULL && title[0] != '\0') {
                    str_adds(&out, " ");
                    add_cut(&out, title, 120);
                }
                str_adds(&out, "\n");
            } else {
                if (who != NULL && who[0] != '\0' && (title == NULL || strcmp(who, title) != 0)) {
                    str_adds(&out, "| ");
                    add_cut(&out, who, 80);
                    str_adds(&out, "\n");
                }
                if (title != NULL && title[0] != '\0') {
                    str_adds(&out, "| ");
                    add_cut(&out, title, 160);
                    str_adds(&out, "\n");
                }
            }
            if (desc != NULL) {
                add_quoted(&out, desc, 4, 200);
            }
            for (f = fields != NULL && fields->type == J_ARR ? fields->first : NULL; f != NULL && nf < 5; f = f->next, nf++) {
                const char *fname = json_get_str(f, "name", ""), *fval = json_get_str(f, "value", "");

                str_adds(&out, "| ");
                add_cut(&out, fname, 60);
                str_adds(&out, ": ");
                add_cut(&out, fval, 120);
                str_adds(&out, "\n");
            }
            if (footer != NULL && footer[0] != '\0') {
                str_adds(&out, "| ");
                add_cut(&out, footer, 120);
                str_adds(&out, "\n");
            }
        }
        if (embed_picture(e, &pic)) {
            char dims[48] = "";

            if (pic.width > 0 && pic.height > 0) {
                sprintf(dims, " %dx%d", pic.width, pic.height);
            }
            str_adds(&out, "[image: ");
            str_adds(&out, pic.name);
            str_adds(&out, dims);
            str_adds(&out, "]\n");
        }
        shown++;
    }
    if (total > MAX_EMBEDS_SHOWN) {
        char more[48];

        sprintf(more, "(%d more embeds)\n", total - MAX_EMBEDS_SHOWN);
        str_adds(&out, more);
    }
    if (out.failed || out.len == 0) {
        free(out.buf);
        return NULL;
    }
    if (out.buf[out.len - 1] == '\n') {
        out.buf[--out.len] = '\0';
    }
    return out.buf;
}

/* Stickers (PNG, animated PNG and GIF ones; Lottie cannot be shown) as pictures after the attachments. */
static void append_sticker_images(Message *msg, const JNode *json)
{
    const JNode *list = json_get(json, "sticker_items"), *st;
    int shown = 0;

    if (list == NULL || list->type != J_ARR) {
        return;
    }
    for (st = list->first; st != NULL && shown < 3; st = st->next, shown++) {
        const char *id = id_of(st, "id");
        int fmt = int_of(st, "format_type", 0);
        Attachment *a;

        if (id == NULL || (fmt != 1 && fmt != 2 && fmt != 4) || msg->natt >= MODEL_MAX_ATTACHMENTS) {
            continue;
        }
        if (msg->att == NULL) {
            msg->att = (Attachment *)calloc(MODEL_MAX_ATTACHMENTS, sizeof(Attachment));
            if (msg->att == NULL) {
                return;
            }
        }
        a = &msg->att[msg->natt++];
        memset(a, 0, sizeof(*a));
        sprintf(a->url, "https://media.discordapp.net/stickers/%s.%s", id, fmt == 4 ? "gif" : "png");
        copy_utf8(a->name, sizeof(a->name), json_get_str(st, "name", "sticker"));
        a->is_image = 1;
        a->is_gif = fmt == 4;
    }
}

/* Adds the embeds' pictures after the attachments already in msg->att (up to the limit). */
static void append_embed_images(Message *msg, const JNode *embeds)
{
    const JNode *e;
    Attachment pic;
    int shown = 0;

    if (embeds == NULL || embeds->type != J_ARR) {
        return;
    }
    for (e = embeds->first; e != NULL && shown < MAX_EMBEDS_SHOWN; e = e->next, shown++) {
        if (msg->natt >= MODEL_MAX_ATTACHMENTS || !embed_picture(e, &pic)) {
            continue;
        }
        if (msg->att == NULL) {
            msg->att = (Attachment *)calloc(MODEL_MAX_ATTACHMENTS, sizeof(Attachment));
            if (msg->att == NULL) {
                return;
            }
        }
        msg->att[msg->natt++] = pic;
    }
}

/* Keeps the first few attachments' addresses, names and sizes, so an image can be fetched later. */
static void parse_attachments(Message *msg, const JNode *json)
{
    const JNode *list = json_get(json, "attachments"), *a;
    int n = 0;

    msg->att = NULL;
    msg->natt = 0;
    if (list == NULL || list->type != J_ARR || list->count == 0) {
        return;
    }
    msg->att = (Attachment *)calloc(MODEL_MAX_ATTACHMENTS, sizeof(Attachment));
    if (msg->att == NULL) {
        return;
    }
    for (a = list->first; a != NULL && n < MODEL_MAX_ATTACHMENTS; a = a->next) {
        const char *url = json_get_str(a, "proxy_url", NULL);
        Attachment *t = &msg->att[n];

        if (url == NULL || url[0] == '\0') {
            url = json_get_str(a, "url", "");
        }
        if (strlen(url) >= sizeof(t->url)) {
            continue;                                      /* a longer address than we keep: unusable */
        }
        strcpy(t->url, url);
        copy_utf8(t->name, sizeof(t->name), json_get_str(a, "filename", "file"));
        t->width = int_of(a, "width", 0);
        t->height = int_of(a, "height", 0);
        t->is_image = is_image_attachment(a);
        {
            const char *ct = json_get_str(a, "content_type", "");

            t->is_jpeg = strncmp(ct, "image/jpeg", 10) == 0 || ends_with_ci(t->name, ".jpg") || ends_with_ci(t->name, ".jpeg");
            t->is_gif = strncmp(ct, "image/gif", 9) == 0 || ends_with_ci(t->name, ".gif");
        }
        n++;
    }
    msg->natt = n;
    if (n == 0) {
        free(msg->att);
        msg->att = NULL;
    }
}

void model_ingest_message(Model *m, const JNode *msg, int from_gateway)
{
    const char *id = id_of(msg, "id"), *cid = id_of(msg, "channel_id"), *aid;
    const JNode *author = json_get(msg, "author");
    Channel *c;
    Message tmp, *ex;
    char *text;
    Emoji *tmp_emoji = NULL;
    int tmp_nemoji = 0;
    int at, bt;

    if (id == NULL || cid == NULL || author == NULL || author->type != J_OBJ) {
        return;
    }
    c = model_find_channel(m, cid);
    if (c == NULL && json_get(msg, "guild_id") == NULL) {
        c = dm_from_message(m, cid, author);              /* a conversation we had not heard of yet */
    }
    if (c == NULL) {
        return;
    }
    {
        Emoji emo[MODEL_MAX_EMOJI];
        int nemo = 0;

        text = render_text(m, msg, emo, &nemo);
        if (text != NULL && nemo > 0) {
            tmp_emoji = (Emoji *)malloc((size_t)nemo * sizeof(Emoji));
            if (tmp_emoji != NULL) {
                memcpy(tmp_emoji, emo, (size_t)nemo * sizeof(Emoji));
                tmp_nemoji = nemo;
            }
        }
    }
    if (text == NULL) {
        return;
    }
    memset(&tmp, 0, sizeof(tmp));
    copy_utf8(tmp.id, sizeof(tmp.id), id);
    aid = id_of(author, "id");
    copy_utf8(tmp.author_id, sizeof(tmp.author_id), aid != NULL ? aid : "");
    {
        /* the member's nickname rides in msg.member, not in the author */
        const char *nick = json_get_str(json_get(msg, "member"), "nick", NULL);

        if (nick != NULL && nick[0] != '\0') {
            copy_utf8(tmp.author, sizeof(tmp.author), nick);
        } else {
            copy_utf8(tmp.author, sizeof(tmp.author), user_display(author));
        }
    }
    copy_utf8(tmp.time, sizeof(tmp.time), json_get_str(msg, "timestamp", ""));
    tmp.edited = json_str(json_get(msg, "edited_timestamp")) != NULL;
    bt = 0;
    tmp.bot = json_bool(json_get(author, "bot"), &bt) ? bt : 0;
    tmp.type = int_of(msg, "type", 0);
    tmp.text = text;
    tmp.emoji = tmp_emoji;
    tmp.nemoji = tmp_nemoji;
    parse_attachments(&tmp, msg);
    parse_reactions(&tmp, json_get(msg, "reactions"));
    append_sticker_images(&tmp, msg);
    {
        const JNode *emb = json_get(msg, "embeds");

        tmp.embeds = render_embeds(emb);
        append_embed_images(&tmp, emb);
        if (tmp.text[0] == '\0' && tmp.embeds == NULL && emb != NULL && emb->type == J_ARR && emb->count > 0) {
            char *fallback = (char *)malloc(8);

            if (fallback != NULL) {
                strcpy(fallback, "[embed]");
                free(tmp.text);
                tmp.text = fallback;
            }
        }
    }

    ex = model_find_message(c, id);
    if (ex != NULL) {                    /* already known (our own POST, then the gateway's copy): refresh quietly */
        message_clear(ex);
        *ex = tmp;
        return;
    }
    if (c->nmsgs >= MODEL_MAX_MESSAGES) {
        if (model_id_cmp(id, c->msgs[0].id) < 0) {
            message_clear(&tmp);         /* older than everything kept */
            return;
        }
        message_clear(&c->msgs[0]);
        memmove(&c->msgs[0], &c->msgs[1], (size_t)(c->nmsgs - 1) * sizeof(Message));
        c->nmsgs--;
    }
    if (c->nmsgs >= c->cap) {
        int ncap = c->cap ? c->cap * 2 : 16;
        Message *nm = (Message *)realloc(c->msgs, (size_t)ncap * sizeof(Message));

        if (nm == NULL) {
            message_clear(&tmp);
            return;
        }
        c->msgs = nm;
        c->cap = ncap;
    }
    for (at = c->nmsgs; at > 0 && model_id_cmp(c->msgs[at - 1].id, id) > 0; at--) {
        ;
    }
    memmove(&c->msgs[at + 1], &c->msgs[at], (size_t)(c->nmsgs - at) * sizeof(Message));
    c->msgs[at] = tmp;
    c->nmsgs++;
    if (from_gateway) {
        push_change(m, MC_MESSAGE_ADDED, c->guild_id, c->id, id);
        if (at == c->nmsgs - 1 && strcmp(tmp.author_id, m->user_id) != 0) {
            c->unread++;
            if (mentions_user(msg, m->user_id) || c->type == CH_DM || c->type == CH_GROUP_DM) {
                c->mentions++;                           /* a direct message is always addressed to us */
            }
            push_change(m, MC_UNREAD, c->guild_id, c->id, NULL);
        }
    }
}

static void message_update(Model *m, const JNode *d)
{
    const char *id = id_of(d, "id"), *cid = id_of(d, "channel_id");
    Channel *c;
    Message *msg;
    char *text;
    int edited, embeds_changed = 0;

    if (id == NULL || cid == NULL || (c = model_find_channel(m, cid)) == NULL ||
        (msg = model_find_message(c, id)) == NULL) {
        return;
    }
    edited = msg->edited;
    if (json_get(d, "edited_timestamp") != NULL) {
        edited = json_str(json_get(d, "edited_timestamp")) != NULL;
    }
    if (json_get(d, "embeds") != NULL) {                 /* a link preview arriving, or changing */
        const JNode *emb = json_get(d, "embeds");
        char *etext = render_embeds(emb);
        int i, kept = 0;

        embeds_changed = (etext == NULL) != (msg->embeds == NULL) || (etext != NULL && strcmp(etext, msg->embeds) != 0);
        free(msg->embeds);
        msg->embeds = etext;
        for (i = 0; i < msg->natt; i++) {                /* the files stay; the embeds' pictures are replaced */
            if (!msg->att[i].from_embed) {
                msg->att[kept++] = msg->att[i];
            }
        }
        msg->natt = kept;
        append_embed_images(msg, emb);
        if (msg->natt == 0) {
            free(msg->att);
            msg->att = NULL;
        }
    }
    if (json_get(d, "content") != NULL) {
        Emoji emo[MODEL_MAX_EMOJI];
        int nemo = 0;

        text = render_text(m, d, emo, &nemo);
        if (text == NULL) {
            return;
        }
        if (strcmp(text, msg->text) == 0 && edited == msg->edited) {
            free(text);
            if (embeds_changed) {
                push_change(m, MC_MESSAGE_CHANGED, c->guild_id, c->id, id);
            }
            return;
        }
        free(msg->text);
        msg->text = text;
        free(msg->emoji);
        msg->emoji = nemo > 0 ? (Emoji *)malloc((size_t)nemo * sizeof(Emoji)) : NULL;
        msg->nemoji = 0;
        if (msg->emoji != NULL) {
            memcpy(msg->emoji, emo, (size_t)nemo * sizeof(Emoji));
            msg->nemoji = nemo;
        }
    } else if (edited == msg->edited && !embeds_changed) {
        return;
    }
    msg->edited = edited;
    push_change(m, MC_MESSAGE_CHANGED, c->guild_id, c->id, id);
}

static void message_delete(Model *m, const char *channel_id, const char *message_id)
{
    Channel *c = model_find_channel(m, channel_id);
    int i;

    if (c == NULL) {
        return;
    }
    for (i = 0; i < c->nmsgs; i++) {
        if (strcmp(c->msgs[i].id, message_id) == 0) {
            message_clear(&c->msgs[i]);
            memmove(&c->msgs[i], &c->msgs[i + 1], (size_t)(c->nmsgs - i - 1) * sizeof(Message));
            c->nmsgs--;
            push_change(m, MC_MESSAGE_REMOVED, c->guild_id, c->id, message_id);
            return;
        }
    }
}

void model_ingest_history(Model *m, const char *channel_id, const JNode *arr)
{
    Channel *c = valid_id(channel_id) ? model_find_channel(m, channel_id) : NULL;
    const JNode *msg;

    if (c == NULL || arr == NULL || arr->type != J_ARR) {
        return;
    }
    for (msg = arr->first; msg != NULL; msg = msg->next) {
        model_ingest_message(m, msg, 0);
        c = model_find_channel(m, channel_id);       /* the arrays may have moved */
        if (c == NULL) {
            return;
        }
    }
    c->history_loaded = 1;
    push_change(m, MC_HISTORY, c->guild_id, c->id, NULL);
}

void model_mark_read(Model *m, const char *channel_id)
{
    Channel *c = model_find_channel(m, channel_id);

    if (c != NULL && (c->unread != 0 || c->mentions != 0)) {
        c->unread = 0;
        c->mentions = 0;
        push_change(m, MC_UNREAD, c->guild_id, c->id, NULL);
    }
}

/* ---------------------------------------------------------------- channels and guilds */

static int kept_type(int t)
{
    return t == CH_TEXT || t == CH_CATEGORY || t == CH_ANNOUNCEMENT;
}

/* Adds or updates one channel object; returns 1 if it was kept (the list may have changed). */
static int channel_apply(Guild *g, const JNode *o)
{
    const char *id = id_of(o, "id"), *parent, *name;
    int type = int_of(o, "type", -1), created;
    Channel *c;

    if (id == NULL || !kept_type(type)) {
        return 0;
    }
    c = channel_upsert(g, id, &created);
    if (c == NULL) {
        return 0;
    }
    name = json_get_str(o, "name", "");
    copy_utf8(c->name, sizeof(c->name), name);
    c->type = type;
    c->position = int_of(o, "position", 0);
    parent = id_of(o, "parent_id");
    copy_utf8(c->parent_id, sizeof(c->parent_id), parent != NULL ? parent : "");
    return 1;
}

/* Replaces the guild's channel list by `arr` (keeping the messages of channels that stay). */
static void apply_channel_list(Model *m, Guild *g, const JNode *arr)
{
    const JNode *o;
    int i, found;
    const char *id;

    if (arr == NULL || arr->type != J_ARR) {
        return;
    }
    for (o = arr->first; o != NULL; o = o->next) {
        channel_apply(g, o);
    }
    for (i = g->nch - 1; i >= 0; i--) {
        found = 0;
        for (o = arr->first; o != NULL && !found; o = o->next) {
            id = id_of(o, "id");
            found = id != NULL && strcmp(id, g->ch[i].id) == 0;
        }
        if (!found) {
            channel_remove(g, i);
        }
    }
    sort_channels(g);
    push_change(m, MC_CHANNELS, g->id, NULL, NULL);
}

void model_ingest_channels(Model *m, const char *guild_id, const JNode *arr)
{
    Guild *g = valid_id(guild_id) ? model_find_guild(m, guild_id) : NULL;

    if (g != NULL) {
        apply_channel_list(m, g, arr);
    }
}

void model_ingest_guilds(Model *m, const JNode *arr)
{
    const JNode *o;
    const char *id;
    Guild *g;
    int created;

    if (arr == NULL || arr->type != J_ARR) {
        return;
    }
    for (o = arr->first; o != NULL; o = o->next) {
        id = id_of(o, "id");
        if (id == NULL) {
            continue;
        }
        g = guild_upsert(m, id, &created);
        if (g != NULL) {
            copy_utf8(g->name, sizeof(g->name), json_get_str(o, "name", g->name));
            if (created) {
                g->unavailable = 0;
            }
        }
    }
    push_change(m, MC_GUILDS, NULL, NULL, NULL);
}

static void on_ready(Model *m, const GwEvent *ev)
{
    const JNode *list = json_get(ev->d, "guilds"), *o;
    const char *id;
    Guild *g;
    int i, created;

    copy_utf8(m->user_id, sizeof(m->user_id), ev->user_id);
    copy_utf8(m->user_name, sizeof(m->user_name), ev->name);
    for (i = 0; i < m->nguilds; i++) {
        m->guilds[i].stale = strcmp(m->guilds[i].id, MODEL_DM_GUILD) != 0;
    }
    for (o = list != NULL ? list->first : NULL; o != NULL; o = o->next) {
        id = id_of(o, "id");
        if (id == NULL) {
            continue;
        }
        g = guild_upsert(m, id, &created);
        if (g == NULL) {
            continue;
        }
        g->stale = 0;
        if (created) {
            g->unavailable = 1;
        }
    }
    for (i = m->nguilds - 1; i >= 0; i--) {
        if (m->guilds[i].stale) {
            guild_remove(m, i);
        }
    }
    list = json_get(ev->d, "private_channels");        /* user accounts get their DMs here; bots get none */
    for (o = list != NULL ? list->first : NULL; o != NULL; o = o->next) {
        model_ingest_private_channel(m, o);
    }
    push_change(m, MC_GUILDS, NULL, NULL, NULL);
}

static void on_guild_create(Model *m, const JNode *d)
{
    const char *id = id_of(d, "id");
    Guild *g;
    int created;

    if (id == NULL) {
        return;
    }
    g = guild_upsert(m, id, &created);
    if (g == NULL) {
        return;
    }
    copy_utf8(g->name, sizeof(g->name), json_get_str(d, "name", g->name));
    g->unavailable = 0;
    g->stale = 0;
    apply_channel_list(m, g, json_get(d, "channels"));
    push_change(m, MC_GUILDS, NULL, NULL, NULL);
}

static void on_guild_delete(Model *m, const JNode *d)
{
    const char *id = id_of(d, "id");
    int i, flag = 0;

    if (id == NULL) {
        return;
    }
    for (i = 0; i < m->nguilds; i++) {
        if (strcmp(m->guilds[i].id, id) == 0) {
            if (json_bool(json_get(d, "unavailable"), &flag) && flag) {
                m->guilds[i].unavailable = 1;
            } else {
                guild_remove(m, i);
            }
            push_change(m, MC_GUILDS, NULL, NULL, NULL);
            return;
        }
    }
}

static void on_channel(Model *m, const char *name, const JNode *d)
{
    const char *gid = id_of(d, "guild_id"), *id = id_of(d, "id");
    Guild *g;
    int i, type = int_of(d, "type", -1);

    if (gid == NULL && (type == CH_DM || type == CH_GROUP_DM)) {              /* a direct message */
        g = model_find_guild(m, MODEL_DM_GUILD);
        if (strcmp(name, "CHANNEL_DELETE") != 0) {
            model_ingest_private_channel(m, d);
        } else if (g != NULL && id != NULL) {
            for (i = 0; i < g->nch; i++) {
                if (strcmp(g->ch[i].id, id) == 0) {
                    channel_remove(g, i);
                    push_change(m, MC_CHANNELS, g->id, NULL, NULL);
                    break;
                }
            }
        }
        return;
    }
    g = gid != NULL ? model_find_guild(m, gid) : NULL;
    if (g == NULL || id == NULL) {
        return;
    }
    if (strcmp(name, "CHANNEL_DELETE") == 0 || !kept_type(type)) {
        for (i = 0; i < g->nch; i++) {
            if (strcmp(g->ch[i].id, id) == 0) {
                channel_remove(g, i);
                sort_channels(g);
                push_change(m, MC_CHANNELS, g->id, NULL, NULL);
                break;
            }
        }
        return;
    }
    if (channel_apply(g, d)) {
        sort_channels(g);
        push_change(m, MC_CHANNELS, g->id, NULL, NULL);
    }
}

/* ---------------------------------------------------------------- gateway events */

void model_gateway_event(Model *m, const GwEvent *ev)
{
    const char *n;
    const JNode *ids, *x;

    if (ev->type == GW_EV_READY) {
        on_ready(m, ev);
        return;
    }
    if (ev->type != GW_EV_DISPATCH || ev->d == NULL) {
        return;
    }
    n = ev->name;
    if (strcmp(n, "GUILD_CREATE") == 0) {
        on_guild_create(m, ev->d);
    } else if (strcmp(n, "GUILD_UPDATE") == 0) {
        const char *id = id_of(ev->d, "id");
        Guild *g = id != NULL ? model_find_guild(m, id) : NULL;

        if (g != NULL) {
            copy_utf8(g->name, sizeof(g->name), json_get_str(ev->d, "name", g->name));
            push_change(m, MC_GUILDS, NULL, NULL, NULL);
        }
    } else if (strcmp(n, "GUILD_DELETE") == 0) {
        on_guild_delete(m, ev->d);
    } else if (strcmp(n, "CHANNEL_CREATE") == 0 || strcmp(n, "CHANNEL_UPDATE") == 0 ||
               strcmp(n, "CHANNEL_DELETE") == 0) {
        on_channel(m, n, ev->d);
    } else if (strcmp(n, "MESSAGE_CREATE") == 0) {
        model_ingest_message(m, ev->d, 1);
    } else if (strcmp(n, "MESSAGE_UPDATE") == 0) {
        message_update(m, ev->d);
    } else if (strcmp(n, "MESSAGE_REACTION_ADD") == 0) {
        on_reaction(m, "ADD", ev->d);
    } else if (strcmp(n, "MESSAGE_REACTION_REMOVE") == 0) {
        on_reaction(m, "REMOVE", ev->d);
    } else if (strcmp(n, "MESSAGE_REACTION_REMOVE_ALL") == 0) {
        on_reaction(m, "REMOVE_ALL", ev->d);
    } else if (strcmp(n, "MESSAGE_REACTION_REMOVE_EMOJI") == 0) {
        on_reaction(m, "REMOVE_EMOJI", ev->d);
    } else if (strcmp(n, "MESSAGE_DELETE") == 0) {
        const char *cid = id_of(ev->d, "channel_id"), *mid = id_of(ev->d, "id");

        if (cid != NULL && mid != NULL) {
            message_delete(m, cid, mid);
        }
    } else if (strcmp(n, "MESSAGE_DELETE_BULK") == 0) {
        const char *cid = id_of(ev->d, "channel_id");

        ids = json_get(ev->d, "ids");
        for (x = ids != NULL ? ids->first : NULL; cid != NULL && x != NULL; x = x->next) {
            if (valid_id(json_str(x))) {
                message_delete(m, cid, json_str(x));
            }
        }
    }
}
