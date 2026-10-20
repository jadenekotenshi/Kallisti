#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/model.h"
#include "test.h"

/* ---- feeding the model gateway events from JSON text ---- */
static void dispatch(Model *m, const char *name, const char *json)
{
    JDoc doc;
    GwEvent ev;

    memset(&ev, 0, sizeof(ev));
    memset(&doc, 0, sizeof(doc));
    ev.type = GW_EV_DISPATCH;
    strcpy(ev.name, name);
    ev.d = json_parse(&doc, json, strlen(json));
    ev.seq = 1;
    model_gateway_event(m, &ev);
    json_free(&doc);
}

static void ready(Model *m, const char *json)
{
    JDoc doc;
    GwEvent ev;

    memset(&ev, 0, sizeof(ev));
    memset(&doc, 0, sizeof(doc));
    ev.type = GW_EV_READY;
    strcpy(ev.name, "Kallisti-bot");
    strcpy(ev.user_id, "42");
    ev.d = json_parse(&doc, json, strlen(json));
    model_gateway_event(m, &ev);
    json_free(&doc);
}

static void rest_guilds(Model *m, const char *json)
{
    JDoc doc;

    memset(&doc, 0, sizeof(doc));
    model_ingest_guilds(m, json_parse(&doc, json, strlen(json)));
    json_free(&doc);
}

static void rest_history(Model *m, const char *cid, const char *json)
{
    JDoc doc;

    memset(&doc, 0, sizeof(doc));
    model_ingest_history(m, cid, json_parse(&doc, json, strlen(json)));
    json_free(&doc);
}

static int drain(Model *m, ModelChange *out, int max)
{
    int n = 0;

    while (n < max && model_next_change(m, &out[n])) {
        n++;
    }
    return n;
}

static int count_changes(Model *m, int type)
{
    ModelChange c;
    int n = 0;

    while (model_next_change(m, &c)) {
        if (c.type == type) {
            n++;
        }
    }
    return n;
}

static const char *GUILD =
    "{\"id\":\"100\",\"name\":\"Test Guild\",\"channels\":["
    "{\"id\":\"203\",\"name\":\"voice\",\"type\":2,\"position\":0},"
    "{\"id\":\"210\",\"name\":\"Text\",\"type\":4,\"position\":1},"
    "{\"id\":\"202\",\"name\":\"random\",\"type\":0,\"position\":1,\"parent_id\":\"210\"},"
    "{\"id\":\"201\",\"name\":\"general\",\"type\":0,\"position\":0,\"parent_id\":\"210\"},"
    "{\"id\":\"211\",\"name\":\"Archive\",\"type\":4,\"position\":0},"
    "{\"id\":\"200\",\"name\":\"welcome\",\"type\":0,\"position\":5},"
    "{\"id\":\"204\",\"name\":\"news\",\"type\":5,\"position\":2,\"parent_id\":\"211\"},"
    "{\"id\":\"205\",\"name\":\"a-thread\",\"type\":11,\"position\":0}]}";

static Model *start(void)
{
    static Model m;

    model_free(&m);
    model_init(&m);
    ready(&m, "{\"guilds\":[{\"id\":\"100\",\"unavailable\":true}]}");
    dispatch(&m, "GUILD_CREATE", GUILD);
    return &m;
}

static void test_guilds_and_channels(void)
{
    Model *m = start();
    Guild *g;
    ModelChange ch[16];
    int n;

    CHECK(strcmp(m->user_id, "42") == 0 && strcmp(m->user_name, "Kallisti-bot") == 0);
    g = model_find_guild(m, "100");
    CHECK(g != NULL && strcmp(g->name, "Test Guild") == 0 && !g->unavailable);
    CHECK(g->nch == 6);                                   /* voice and thread dropped */
    /* orphans first (welcome), then Archive and its child, then Text and its children */
    CHECK(strcmp(g->ch[0].id, "200") == 0);
    CHECK(strcmp(g->ch[1].id, "211") == 0 && g->ch[1].type == CH_CATEGORY);
    CHECK(strcmp(g->ch[2].id, "204") == 0 && g->ch[2].type == CH_ANNOUNCEMENT);
    CHECK(strcmp(g->ch[3].id, "210") == 0);
    CHECK(strcmp(g->ch[4].id, "201") == 0 && strcmp(g->ch[5].id, "202") == 0);
    CHECK(strcmp(g->ch[4].guild_id, "100") == 0 && strcmp(g->ch[4].parent_id, "210") == 0);
    CHECK(model_find_channel(m, "202") != NULL && model_find_channel(m, "203") == NULL);
    CHECK(model_find_channel(m, "999") == NULL && model_find_guild(m, "1") == NULL);

    n = drain(m, ch, 16);
    CHECK(n >= 3 && ch[0].type == MC_GUILDS);
    {
        int i, saw = 0;

        for (i = 0; i < n; i++) {
            if (ch[i].type == MC_CHANNELS && strcmp(ch[i].guild_id, "100") == 0) {
                saw = 1;
            }
        }
        CHECK(saw);
    }

    /* channel create / update / delete */
    dispatch(m, "CHANNEL_CREATE", "{\"id\":\"220\",\"guild_id\":\"100\",\"name\":\"new\",\"type\":0,\"position\":0}");
    g = model_find_guild(m, "100");
    CHECK(g->nch == 7);
    CHECK(strcmp(g->ch[0].id, "220") == 0 && strcmp(g->ch[1].id, "200") == 0);     /* position 0 sorts before 5 */
    CHECK(count_changes(m, MC_CHANNELS) == 1);
    dispatch(m, "CHANNEL_UPDATE", "{\"id\":\"220\",\"guild_id\":\"100\",\"name\":\"renamed\",\"type\":0,\"position\":9}");
    g = model_find_guild(m, "100");
    CHECK(g->nch == 7 && strcmp(model_find_channel(m, "220")->name, "renamed") == 0);
    CHECK(strcmp(g->ch[1].id, "220") == 0);                                         /* moved behind welcome */
    dispatch(m, "CHANNEL_UPDATE", "{\"id\":\"220\",\"guild_id\":\"100\",\"name\":\"renamed\",\"type\":2,\"position\":9}");
    CHECK(model_find_channel(m, "220") == NULL && model_find_guild(m, "100")->nch == 6);   /* became a voice channel */
    dispatch(m, "CHANNEL_DELETE", "{\"id\":\"202\",\"guild_id\":\"100\",\"type\":0}");
    CHECK(model_find_channel(m, "202") == NULL && model_find_guild(m, "100")->nch == 5);
    dispatch(m, "CHANNEL_CREATE", "{\"id\":\"230\",\"guild_id\":\"555\",\"name\":\"x\",\"type\":0}");   /* unknown guild */
    CHECK(model_find_channel(m, "230") == NULL);
    dispatch(m, "CHANNEL_DELETE", "{\"id\":\"9999\",\"guild_id\":\"100\"}");
    CHECK(model_find_guild(m, "100")->nch == 5);

    /* a category deleted: its children stay, now outside any category */
    dispatch(m, "CHANNEL_DELETE", "{\"id\":\"210\",\"guild_id\":\"100\",\"type\":4}");
    g = model_find_guild(m, "100");
    CHECK(g->nch == 4 && strcmp(g->ch[0].id, "201") == 0);

    /* guild update / delete */
    dispatch(m, "GUILD_UPDATE", "{\"id\":\"100\",\"name\":\"Renamed Guild\"}");
    CHECK(strcmp(model_find_guild(m, "100")->name, "Renamed Guild") == 0);
    dispatch(m, "GUILD_DELETE", "{\"id\":\"100\",\"unavailable\":true}");
    CHECK(model_find_guild(m, "100") != NULL && model_find_guild(m, "100")->unavailable);
    dispatch(m, "GUILD_CREATE", GUILD);
    CHECK(!model_find_guild(m, "100")->unavailable && model_find_guild(m, "100")->nch == 6);
    dispatch(m, "GUILD_DELETE", "{\"id\":\"100\"}");
    CHECK(model_find_guild(m, "100") == NULL && m->nguilds == 0);
    model_free(m);
}

static void test_rest_lists(void)
{
    Model m;
    JDoc doc;
    const char *chans = "[{\"id\":\"301\",\"name\":\"a\",\"type\":0,\"position\":1},{\"id\":\"302\",\"name\":\"b\",\"type\":0,\"position\":0}]";

    model_init(&m);
    rest_guilds(&m, "[{\"id\":\"100\",\"name\":\"One\"},{\"id\":\"101\",\"name\":\"Two\"},{\"id\":\"bad\"},{\"name\":\"no id\"}]");
    CHECK(m.nguilds == 2 && strcmp(model_find_guild(&m, "101")->name, "Two") == 0);
    memset(&doc, 0, sizeof(doc));
    model_ingest_channels(&m, "100", json_parse(&doc, chans, strlen(chans)));
    json_free(&doc);
    CHECK(model_find_guild(&m, "100")->nch == 2 && strcmp(model_find_guild(&m, "100")->ch[0].id, "302") == 0);
    memset(&doc, 0, sizeof(doc));
    model_ingest_channels(&m, "555", json_parse(&doc, chans, strlen(chans)));      /* unknown guild */
    model_ingest_channels(&m, "100", NULL);
    model_ingest_channels(&m, "x", json_parse(&doc, chans, strlen(chans)));
    json_free(&doc);
    CHECK(m.nguilds == 2);
    rest_guilds(&m, "{\"not\":\"an array\"}");
    CHECK(m.nguilds == 2);
    model_free(&m);
}

static const char *MSG1 =
    "{\"id\":\"1000\",\"channel_id\":\"201\",\"guild_id\":\"100\",\"content\":\"hi <@7> and <@!42>, see <#202> <@&5> <:kek:123> <a:dance:456> <t:123:R> <>\","
    "\"timestamp\":\"2026-10-19T10:00:00.123000+00:00\",\"edited_timestamp\":null,\"type\":0,"
    "\"author\":{\"id\":\"8\",\"username\":\"bob\",\"global_name\":\"Bob B\"},\"member\":{\"nick\":\"Bobby\"},"
    "\"mentions\":[{\"id\":\"7\",\"username\":\"seven\",\"global_name\":null},{\"id\":\"42\",\"username\":\"Kallisti-bot\",\"bot\":true}],"
    "\"attachments\":[{\"filename\":\"cat.png\"},{\"filename\":\"dog.png\"}]}";

static void test_messages(void)
{
    Model *m = start();
    Channel *c;
    Message *msg;
    ModelChange ch[16];

    drain(m, ch, 16);
    dispatch(m, "MESSAGE_CREATE", MSG1);
    c = model_find_channel(m, "201");
    CHECK(c->nmsgs == 1);
    msg = &c->msgs[0];
    CHECK(strcmp(msg->author, "Bobby") == 0 && strcmp(msg->author_id, "8") == 0 && !msg->edited && !msg->bot);
    CHECK(strcmp(msg->text, "hi @seven and @Kallisti-bot, see #random @role :kek: :dance: <t:123:R> <>\n"
                            "[attachment: cat.png]\n[attachment: dog.png]") == 0);
    CHECK(strcmp(msg->time, "2026-10-19T10:00:00.123000+00:00") == 0);
    CHECK(c->unread == 1 && c->mentions == 1);
    CHECK(drain(m, ch, 16) == 2 && ch[0].type == MC_MESSAGE_ADDED && strcmp(ch[0].message_id, "1000") == 0 &&
          strcmp(ch[0].channel_id, "201") == 0 && strcmp(ch[0].guild_id, "100") == 0 && ch[1].type == MC_UNREAD);

    /* display name falls back from nick to global_name to username; bots flagged */
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"1001\",\"channel_id\":\"201\",\"content\":\"x\",\"author\":{\"id\":\"9\",\"username\":\"u9\",\"global_name\":\"Nine\"}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"1002\",\"channel_id\":\"201\",\"content\":\"y\",\"author\":{\"id\":\"10\",\"username\":\"u10\",\"global_name\":null,\"bot\":true}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"1003\",\"channel_id\":\"201\",\"content\":\"z\",\"author\":{\"id\":\"11\"}}");
    c = model_find_channel(m, "201");
    CHECK(strcmp(c->msgs[1].author, "Nine") == 0 && strcmp(c->msgs[2].author, "u10") == 0 && c->msgs[2].bot);
    CHECK(strcmp(c->msgs[3].author, "unknown") == 0);
    CHECK(c->unread == 4 && c->mentions == 1);

    /* our own messages do not count as unread; neither do ones arriving through REST */
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"1004\",\"channel_id\":\"201\",\"content\":\"mine\",\"author\":{\"id\":\"42\",\"username\":\"Kallisti-bot\"}}");
    CHECK(model_find_channel(m, "201")->unread == 4);
    model_mark_read(m, "201");
    CHECK(model_find_channel(m, "201")->unread == 0 && model_find_channel(m, "201")->mentions == 0);
    CHECK(count_changes(m, MC_UNREAD) >= 1);

    /* duplicates (our POST result, then the gateway's copy) are merged without new events */
    drain(m, ch, 16);
    {
        JDoc doc;
        const char *own = "{\"id\":\"1005\",\"channel_id\":\"202\",\"content\":\"sent\",\"author\":{\"id\":\"42\",\"username\":\"Kallisti-bot\"}}";

        memset(&doc, 0, sizeof(doc));
        model_ingest_message(m, json_parse(&doc, own, strlen(own)), 1);
        model_ingest_message(m, doc.root, 1);
        json_free(&doc);
    }
    CHECK(model_find_channel(m, "202")->nmsgs == 1 && count_changes(m, MC_MESSAGE_ADDED) == 1);

    /* out of order arrival is sorted by id (numerically, not as text) */
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"99\",\"channel_id\":\"200\",\"content\":\"old\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"1000000000000000000\",\"channel_id\":\"200\",\"content\":\"new\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"100\",\"channel_id\":\"200\",\"content\":\"mid\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    c = model_find_channel(m, "200");
    CHECK(c->nmsgs == 3 && strcmp(c->msgs[0].id, "99") == 0 && strcmp(c->msgs[1].id, "100") == 0 &&
          strcmp(c->msgs[2].id, "1000000000000000000") == 0);
    CHECK(c->unread == 2);                       /* "mid" was not the newest */
    CHECK(model_find_message(c, "100") != NULL && model_find_message(c, "5") == NULL);

    /* system messages, embeds, stickers with no text */
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"2000\",\"channel_id\":\"200\",\"content\":\"\",\"type\":7,\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"2001\",\"channel_id\":\"200\",\"content\":\"\",\"embeds\":[{}],\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"2002\",\"channel_id\":\"200\",\"content\":\"\",\"sticker_items\":[{}],\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"2003\",\"channel_id\":\"200\",\"content\":\"\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    c = model_find_channel(m, "200");
    CHECK(strcmp(model_find_message(c, "2000")->text, "[system message]") == 0);
    CHECK(strcmp(model_find_message(c, "2001")->text, "[embed]") == 0);
    CHECK(strcmp(model_find_message(c, "2002")->text, "[sticker]") == 0);
    CHECK(strcmp(model_find_message(c, "2003")->text, "") == 0);

    /* ignored: unknown channel (a thread), missing fields, wrong types */
    drain(m, ch, 16);
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"3000\",\"channel_id\":\"9999\",\"guild_id\":\"100\",\"content\":\"x\",\"author\":{\"id\":\"1\"}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"3001\",\"channel_id\":\"200\",\"content\":\"x\"}");
    dispatch(m, "MESSAGE_CREATE", "{\"channel_id\":\"200\",\"content\":\"x\",\"author\":{\"id\":\"1\"}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":3002,\"channel_id\":\"200\",\"content\":\"x\",\"author\":{\"id\":\"1\"}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"3003\",\"channel_id\":\"200\",\"content\":5,\"author\":\"x\"}");
    dispatch(m, "MESSAGE_CREATE", "[]");
    dispatch(m, "MESSAGE_CREATE", "null");
    dispatch(m, "SOMETHING_ELSE", "{\"id\":\"1\"}");
    CHECK(drain(m, ch, 16) == 0);
    model_free(m);
}

static void test_update_delete(void)
{
    Model *m = start();
    Channel *c;
    ModelChange ch[16];
    char big[4000];

    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"10\",\"channel_id\":\"201\",\"content\":\"hello\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"11\",\"channel_id\":\"201\",\"content\":\"world\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"12\",\"channel_id\":\"201\",\"content\":\"again\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    drain(m, ch, 16);

    dispatch(m, "MESSAGE_UPDATE", "{\"id\":\"11\",\"channel_id\":\"201\",\"content\":\"WORLD\",\"edited_timestamp\":\"2026-10-19T11:00:00+00:00\"}");
    c = model_find_channel(m, "201");
    CHECK(strcmp(model_find_message(c, "11")->text, "WORLD") == 0 && model_find_message(c, "11")->edited);
    CHECK(drain(m, ch, 16) == 1 && ch[0].type == MC_MESSAGE_CHANGED && strcmp(ch[0].message_id, "11") == 0);
    /* an embed unfurling: the same text, no edit time -> nothing to show */
    dispatch(m, "MESSAGE_UPDATE", "{\"id\":\"11\",\"channel_id\":\"201\",\"content\":\"WORLD\",\"embeds\":[{}]}");
    CHECK(drain(m, ch, 16) == 0);
    /* a partial update without content leaves the text alone */
    dispatch(m, "MESSAGE_UPDATE", "{\"id\":\"10\",\"channel_id\":\"201\",\"edited_timestamp\":\"2026-10-19T11:00:00+00:00\"}");
    CHECK(strcmp(model_find_message(model_find_channel(m, "201"), "10")->text, "hello") == 0);
    CHECK(model_find_message(model_find_channel(m, "201"), "10")->edited);
    drain(m, ch, 16);
    dispatch(m, "MESSAGE_UPDATE", "{\"id\":\"555\",\"channel_id\":\"201\",\"content\":\"nobody\"}");   /* not kept */
    dispatch(m, "MESSAGE_UPDATE", "{\"id\":\"10\",\"channel_id\":\"999\",\"content\":\"nobody\"}");
    CHECK(drain(m, ch, 16) == 0);

    dispatch(m, "MESSAGE_DELETE", "{\"id\":\"11\",\"channel_id\":\"201\"}");
    c = model_find_channel(m, "201");
    CHECK(c->nmsgs == 2 && model_find_message(c, "11") == NULL);
    CHECK(drain(m, ch, 16) == 1 && ch[0].type == MC_MESSAGE_REMOVED && strcmp(ch[0].message_id, "11") == 0);
    dispatch(m, "MESSAGE_DELETE", "{\"id\":\"11\",\"channel_id\":\"201\"}");
    dispatch(m, "MESSAGE_DELETE", "{\"id\":\"11\"}");
    CHECK(drain(m, ch, 16) == 0);
    dispatch(m, "MESSAGE_DELETE_BULK", "{\"ids\":[\"10\",\"12\",\"77\",5,\"x\"],\"channel_id\":\"201\"}");
    CHECK(model_find_channel(m, "201")->nmsgs == 0);
    CHECK(drain(m, ch, 16) == 2);

    /* names that do not fit are cut on a character boundary */
    memset(big, 0, sizeof(big));
    strcpy(big, "{\"id\":\"240\",\"guild_id\":\"100\",\"type\":0,\"position\":0,\"name\":\"");
    {
        size_t n = strlen(big);
        int i;

        for (i = 0; i < 200; i++) {
            memcpy(big + n, "\xE2\x98\x83", 3);
            n += 3;
        }
        strcpy(big + n, "\"}");
    }
    dispatch(m, "CHANNEL_CREATE", big);
    c = model_find_channel(m, "240");
    CHECK(c != NULL && strlen(c->name) <= 131 && strlen(c->name) % 3 == 0 && strlen(c->name) >= 126);
    model_free(m);
}

static void test_history_and_cap(void)
{
    Model *m = start();
    Channel *c;
    ModelChange ch[16];
    char json[8192], one[200];
    int i, n;

    /* newest first, as REST returns it; one event for the whole batch */
    drain(m, ch, 16);
    rest_history(m, "201", "[{\"id\":\"30\",\"channel_id\":\"201\",\"content\":\"c\",\"author\":{\"id\":\"1\",\"username\":\"a\"}},"
                           "{\"id\":\"20\",\"channel_id\":\"201\",\"content\":\"b\",\"author\":{\"id\":\"1\",\"username\":\"a\"}},"
                           "{\"id\":\"10\",\"channel_id\":\"201\",\"content\":\"a\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}]");
    c = model_find_channel(m, "201");
    CHECK(c->nmsgs == 3 && strcmp(c->msgs[0].text, "a") == 0 && strcmp(c->msgs[2].text, "c") == 0 && c->history_loaded);
    CHECK(c->unread == 0);
    CHECK(drain(m, ch, 16) == 1 && ch[0].type == MC_HISTORY && strcmp(ch[0].channel_id, "201") == 0);
    /* loading it again changes nothing */
    rest_history(m, "201", "[{\"id\":\"20\",\"channel_id\":\"201\",\"content\":\"b\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}]");
    CHECK(model_find_channel(m, "201")->nmsgs == 3);
    rest_history(m, "999", "[]");
    rest_history(m, "201", "{}");
    rest_history(m, "nope", "[]");

    /* the 200-message cap: the oldest fall off, and an older message is dropped */
    for (i = 1; i <= MODEL_MAX_MESSAGES + 50; i++) {
        sprintf(one, "{\"id\":\"%d\",\"channel_id\":\"200\",\"content\":\"m%d\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}", 1000 + i, i);
        dispatch(m, "MESSAGE_CREATE", one);
    }
    c = model_find_channel(m, "200");
    CHECK(c->nmsgs == MODEL_MAX_MESSAGES && strcmp(c->msgs[0].id, "1051") == 0 && strcmp(c->msgs[199].id, "1250") == 0);
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"5\",\"channel_id\":\"200\",\"content\":\"ancient\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    CHECK(model_find_channel(m, "200")->nmsgs == MODEL_MAX_MESSAGES && strcmp(model_find_channel(m, "200")->msgs[0].id, "1051") == 0);
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"1100\",\"channel_id\":\"200\",\"content\":\"dup\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    CHECK(model_find_channel(m, "200")->nmsgs == MODEL_MAX_MESSAGES);

    /* a long burst of changes with nobody listening collapses into one RESET */
    n = 0;
    while (model_next_change(m, &ch[0])) {
        n++;
    }
    CHECK(n == MODEL_MAX_CHANGES + 1 && ch[0].type == MC_RESET);
    (void)json;
    model_free(m);
}

static void test_reconnect(void)
{
    Model *m = start();
    ModelChange ch[16];

    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"10\",\"channel_id\":\"201\",\"content\":\"keep me\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    drain(m, ch, 16);
    /* a new session: guilds we left disappear, the others keep their messages until GUILD_CREATE arrives */
    dispatch(m, "GUILD_CREATE", "{\"id\":\"101\",\"name\":\"Gone\",\"channels\":[]}");
    CHECK(m->nguilds == 2);
    ready(m, "{\"guilds\":[{\"id\":\"100\",\"unavailable\":true},{\"id\":\"102\",\"unavailable\":true}]}");
    CHECK(m->nguilds == 2 && model_find_guild(m, "101") == NULL);
    CHECK(model_find_guild(m, "102") != NULL && model_find_guild(m, "102")->unavailable);
    CHECK(model_find_channel(m, "201") != NULL && model_find_channel(m, "201")->nmsgs == 1);
    dispatch(m, "GUILD_CREATE", GUILD);
    CHECK(model_find_channel(m, "201")->nmsgs == 1 && strcmp(model_find_channel(m, "201")->msgs[0].text, "keep me") == 0);
    dispatch(m, "GUILD_CREATE", "{\"id\":\"100\",\"name\":\"Test Guild\",\"channels\":[{\"id\":\"200\",\"name\":\"welcome\",\"type\":0}]}");
    CHECK(model_find_guild(m, "100")->nch == 1 && model_find_channel(m, "201") == NULL);   /* removed while we were away */

    /* READY with no guilds list */
    ready(m, "{}");
    CHECK(m->nguilds == 0);
    model_free(m);
}

static void test_attachments(void)
{
    Model *m = start();
    Channel *c;
    Message *msg;

    {
        char json[1200];

        strcpy(json, "{\"id\":\"10\",\"channel_id\":\"201\",\"content\":\"look\",\"author\":{\"id\":\"1\",\"username\":\"a\"},");
        strcat(json, "\"attachments\":[{\"filename\":\"cat.png\",\"url\":\"https://cdn.example/a/cat.png\",");
        strcat(json, "\"proxy_url\":\"https://media.example/a/cat.png?ex=1&hm=2\",\"width\":800,\"height\":600,\"content_type\":\"image/png\"},");
        strcat(json, "{\"filename\":\"notes.txt\",\"url\":\"https://cdn.example/a/notes.txt\",\"content_type\":\"text/plain\"},");
        strcat(json, "{\"filename\":\"pic.jpg\",\"url\":\"https://cdn.example/a/pic.jpg\",\"width\":10,\"height\":20},");
        strcat(json, "{\"filename\":\"bad\",\"url\":123},{\"filename\":\"five\",\"url\":\"u5\"},{\"filename\":\"six\",\"url\":\"u6\"}]}");
        dispatch(m, "MESSAGE_CREATE", json);
    }
    c = model_find_channel(m, "201");
    msg = &c->msgs[0];
    CHECK(strcmp(msg->text, "look\n[image: cat.png 800x600]\n[attachment: notes.txt]\n[image: pic.jpg 10x20]\n[attachment: bad]\n[attachment: five]") == 0);
    CHECK(msg->natt == MODEL_MAX_ATTACHMENTS && msg->att != NULL);
    CHECK(strcmp(msg->att[0].url, "https://media.example/a/cat.png?ex=1&hm=2") == 0 && msg->att[0].is_image &&
          msg->att[0].width == 800 && msg->att[0].height == 600 && strcmp(msg->att[0].name, "cat.png") == 0);
    CHECK(strcmp(msg->att[1].url, "https://cdn.example/a/notes.txt") == 0 && !msg->att[1].is_image);
    CHECK(msg->att[2].is_image && msg->att[2].width == 10);
    CHECK(!msg->att[0].is_gif && !msg->att[2].is_gif);
    CHECK(!msg->att[0].is_jpeg && !msg->att[1].is_jpeg && msg->att[2].is_jpeg);          /* pic.jpg: by its extension */
    /* a message without attachments has none, and an over-long address is dropped rather than cut */
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"11\",\"channel_id\":\"201\",\"content\":\"plain\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    CHECK(model_find_channel(m, "201")->msgs[1].att == NULL && model_find_channel(m, "201")->msgs[1].natt == 0);
    {
        char big[1200];

        strcpy(big, "{\"id\":\"12\",\"channel_id\":\"201\",\"content\":\"\",\"author\":{\"id\":\"1\",\"username\":\"a\"},\"attachments\":[{\"filename\":\"x.png\",\"width\":1,\"height\":1,\"url\":\"");
        memset(big + strlen(big), 'a', 600);
        strcpy(big + strlen(big), "\"}]}");
        dispatch(m, "MESSAGE_CREATE", big);
        CHECK(model_find_channel(m, "201")->msgs[2].att == NULL);
    }
    /* deleting and replacing release them (the sanitizer run checks) */
    dispatch(m, "MESSAGE_DELETE", "{\"id\":\"10\",\"channel_id\":\"201\"}");
    CHECK(model_find_message(model_find_channel(m, "201"), "10") == NULL);
    model_free(m);
}

static void test_embeds(void)
{
    Model *m = start();
    Channel *c;
    Message *msg;
    ModelChange ch[16];
    char json[3000];
    int i;

    drain(m, ch, 16);
    /* a link preview: provider, title, description, and a thumbnail that becomes a picture */
    strcpy(json, "{\"id\":\"10\",\"channel_id\":\"201\",\"content\":\"see this\",\"author\":{\"id\":\"1\",\"username\":\"a\"},");
    strcat(json, "\"embeds\":[{\"type\":\"article\",\"title\":\"Cool Page\",\"description\":\"line one\\nline two\\n\\nline three\",");
    strcat(json, "\"provider\":{\"name\":\"Example\"},\"thumbnail\":{\"proxy_url\":\"https://media.example/ext/x/pic.jpg?width=100&hm=1\",\"width\":100,\"height\":50}}]}");
    dispatch(m, "MESSAGE_CREATE", json);
    c = model_find_channel(m, "201");
    msg = &c->msgs[0];
    CHECK(strcmp(msg->text, "see this") == 0);
    CHECK(msg->embeds != NULL && strcmp(msg->embeds, "| Example\n| Cool Page\n| line one\n| line two\n| line three\n[image: pic.jpg 100x50]") == 0);
    CHECK(msg->natt == 1 && msg->att[0].from_embed && msg->att[0].is_image && msg->att[0].is_jpeg && strcmp(msg->att[0].name, "pic.jpg") == 0 &&
          strcmp(msg->att[0].url, "https://media.example/ext/x/pic.jpg?width=100&hm=1") == 0 && msg->att[0].width == 100);

    /* a rich embed: author, fields, footer; its image beats its thumbnail */
    strcpy(json, "{\"id\":\"11\",\"channel_id\":\"201\",\"content\":\"\",\"author\":{\"id\":\"1\",\"username\":\"a\"},");
    strcat(json, "\"embeds\":[{\"type\":\"rich\",\"title\":\"Report\",\"author\":{\"name\":\"Bot\"},\"fields\":[{\"name\":\"CPU\",\"value\":\"9%\"},{\"name\":\"RAM\",\"value\":\"1 GB\"}],");
    strcat(json, "\"footer\":{\"text\":\"v1.0\"},\"image\":{\"url\":\"https://cdn.example/big.png\"},\"thumbnail\":{\"url\":\"https://cdn.example/small.png\"}}]}");
    dispatch(m, "MESSAGE_CREATE", json);
    msg = &model_find_channel(m, "201")->msgs[1];
    CHECK(strcmp(msg->text, "") == 0);
    CHECK(msg->embeds != NULL && strcmp(msg->embeds, "| Bot\n| Report\n| CPU: 9%\n| RAM: 1 GB\n| v1.0\n[image: big.png]") == 0);
    CHECK(msg->natt == 1 && strcmp(msg->att[0].name, "big.png") == 0 && !msg->att[0].is_jpeg);

    /* a bare image link, a video, an empty embed, and a crowd */
    strcpy(json, "{\"id\":\"12\",\"channel_id\":\"201\",\"content\":\"https://x/y.png\",\"author\":{\"id\":\"1\",\"username\":\"a\"},");
    strcat(json, "\"embeds\":[{\"type\":\"image\",\"url\":\"https://x/y.png\",\"thumbnail\":{\"url\":\"https://x/y.png\",\"width\":64,\"height\":64}}]}");
    dispatch(m, "MESSAGE_CREATE", json);
    msg = &model_find_channel(m, "201")->msgs[2];
    CHECK(msg->embeds != NULL && strcmp(msg->embeds, "[image: y.png 64x64]") == 0 && msg->natt == 1);
    strcpy(json, "{\"id\":\"13\",\"channel_id\":\"201\",\"content\":\"\",\"author\":{\"id\":\"1\",\"username\":\"a\"},");
    strcat(json, "\"embeds\":[{\"type\":\"gifv\",\"title\":\"Funny cat\",\"video\":{\"url\":\"https://t/c.mp4\"},\"thumbnail\":{\"url\":\"https://t/c.png\"}}]}");
    dispatch(m, "MESSAGE_CREATE", json);
    msg = &model_find_channel(m, "201")->msgs[3];
    CHECK(msg->embeds != NULL && strcmp(msg->embeds, "| [video] Funny cat\n[image: c.png]") == 0 && msg->natt == 1);
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"14\",\"channel_id\":\"201\",\"content\":\"\",\"embeds\":[{}],\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    msg = &model_find_channel(m, "201")->msgs[4];
    CHECK(strcmp(msg->text, "[embed]") == 0 && msg->embeds == NULL && msg->natt == 0);
    strcpy(json, "{\"id\":\"15\",\"channel_id\":\"201\",\"content\":\"\",\"author\":{\"id\":\"1\",\"username\":\"a\"},\"embeds\":[");
    for (i = 0; i < 5; i++) {
        strcat(json, i ? ",{\"title\":\"T\"}" : "{\"title\":\"T\"}");
    }
    strcat(json, "]}");
    dispatch(m, "MESSAGE_CREATE", json);
    msg = &model_find_channel(m, "201")->msgs[5];
    CHECK(msg->embeds != NULL && strcmp(msg->embeds, "| T\n| T\n| T\n(2 more embeds)") == 0);
    /* long text is cut on a character boundary, at four lines */
    {
        char desc[700];
        size_t n = 0;

        for (i = 0; i < 100; i++) {
            memcpy(desc + n, "\xE2\x98\x83", 3);                /* 300 bytes of snowmen */
            n += 3;
        }
        desc[n] = '\0';
        sprintf(json, "{\"id\":\"16\",\"channel_id\":\"201\",\"content\":\"\",\"author\":{\"id\":\"1\",\"username\":\"a\"},"
                      "\"embeds\":[{\"description\":\"%s\\nb\\nc\\nd\\ne\"}]}", desc);
        dispatch(m, "MESSAGE_CREATE", json);
        msg = &model_find_channel(m, "201")->msgs[6];
        CHECK(msg->embeds != NULL && strstr(msg->embeds, "...") != NULL && strstr(msg->embeds, "\n| e") == NULL && strstr(msg->embeds, "| d") != NULL);
        {
            const char *dots = strstr(msg->embeds, "...");

            CHECK(dots != NULL && ((unsigned char)dots[-1] & 0xC0) == 0x80 && ((unsigned char)dots[-3]) == 0xE2);       /* whole snowmen */
        }
    }
    drain(m, ch, 16);

    /* link previews arrive after the message, as a MESSAGE_UPDATE with embeds and no content */
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"20\",\"channel_id\":\"201\",\"content\":\"look http://x\",\"author\":{\"id\":\"1\",\"username\":\"a\"},"
             "\"attachments\":[{\"filename\":\"mine.png\",\"url\":\"https://cdn.example/mine.png\",\"width\":5,\"height\":5,\"content_type\":\"image/png\"}]}");
    drain(m, ch, 16);
    msg = model_find_message(model_find_channel(m, "201"), "20");
    CHECK(msg->embeds == NULL && msg->natt == 1 && !msg->att[0].from_embed);
    strcpy(json, "{\"id\":\"20\",\"channel_id\":\"201\",\"guild_id\":\"100\",\"embeds\":[{\"type\":\"link\",\"title\":\"Page\",\"thumbnail\":{\"url\":\"https://x/p.png\"}}]}");
    dispatch(m, "MESSAGE_UPDATE", json);
    msg = model_find_message(model_find_channel(m, "201"), "20");
    CHECK(strcmp(msg->text, "look http://x\n[image: mine.png 5x5]") == 0);        /* the content is untouched */
    CHECK(msg->embeds != NULL && strcmp(msg->embeds, "| Page\n[image: p.png]") == 0 && msg->natt == 2);
    CHECK(!msg->att[0].from_embed && msg->att[1].from_embed);
    CHECK(drain(m, ch, 16) == 1 && ch[0].type == MC_MESSAGE_CHANGED);
    dispatch(m, "MESSAGE_UPDATE", json);                                                  /* the same again: nothing new */
    CHECK(drain(m, ch, 16) == 0);
    dispatch(m, "MESSAGE_UPDATE", "{\"id\":\"20\",\"channel_id\":\"201\",\"embeds\":[{\"title\":\"Other\"}]}");
    msg = model_find_message(model_find_channel(m, "201"), "20");
    CHECK(msg->embeds != NULL && strcmp(msg->embeds, "| Other") == 0 && msg->natt == 1 && !msg->att[0].from_embed);
    CHECK(drain(m, ch, 16) == 1);
    dispatch(m, "MESSAGE_UPDATE", "{\"id\":\"20\",\"channel_id\":\"201\",\"embeds\":[]}");              /* the preview goes away */
    msg = model_find_message(model_find_channel(m, "201"), "20");
    CHECK(msg->embeds == NULL && msg->natt == 1 && drain(m, ch, 16) == 1);
    /* an update without embeds does not touch them */
    dispatch(m, "MESSAGE_UPDATE", "{\"id\":\"20\",\"channel_id\":\"201\",\"embeds\":[{\"title\":\"Back\"}]}");
    drain(m, ch, 16);
    dispatch(m, "MESSAGE_UPDATE", "{\"id\":\"20\",\"channel_id\":\"201\",\"content\":\"look http://x\",\"edited_timestamp\":\"t\"}");
    msg = model_find_message(model_find_channel(m, "201"), "20");
    CHECK(msg->embeds != NULL && strcmp(msg->embeds, "| Back") == 0 && msg->edited);
    model_free(m);
}

static void test_stickers(void)
{
    Model *m = start();
    Message *msg;

    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"10\",\"channel_id\":\"201\",\"content\":\"hi\",\"author\":{\"id\":\"1\",\"username\":\"a\"},"
             "\"sticker_items\":[{\"id\":\"555\",\"name\":\"Wave\",\"format_type\":1},{\"id\":\"556\",\"name\":\"Dance\",\"format_type\":4},"
             "{\"id\":\"557\",\"name\":\"Lottie\",\"format_type\":3},{\"id\":\"x\",\"name\":\"bad\",\"format_type\":1},{\"id\":\"558\",\"name\":\"Cut\",\"format_type\":2}]}");
    msg = &model_find_channel(m, "201")->msgs[0];
    CHECK(strcmp(msg->text, "hi\n[sticker: Wave]\n[sticker: Dance]\n[sticker: Lottie]") == 0);      /* the first three */
    CHECK(msg->natt == 2 && strcmp(msg->att[0].url, "https://media.discordapp.net/stickers/555.png") == 0 && msg->att[0].is_image &&
          strcmp(msg->att[0].name, "Wave") == 0 && strcmp(msg->att[1].url, "https://media.discordapp.net/stickers/556.gif") == 0 &&
          !msg->att[0].from_embed && !msg->att[0].is_gif && msg->att[1].is_gif);
    model_free(m);
}

static void test_custom_emoji(void)
{
    Model *m = start();
    Message *msg;
    char json[2000];
    int i;

    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"10\",\"channel_id\":\"201\",\"content\":\"hi <:blob:123> and <a:dance:456>!\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    msg = &model_find_channel(m, "201")->msgs[0];
    CHECK(strcmp(msg->text, "hi :blob: and :dance:!") == 0 && msg->nemoji == 2 && msg->emoji != NULL);
    CHECK(msg->emoji[0].off == 3 && msg->emoji[0].len == 6 && strcmp(msg->emoji[0].id, "123") == 0 && !msg->emoji[0].animated &&
          strncmp(msg->text + msg->emoji[0].off, ":blob:", 6) == 0);
    CHECK(msg->emoji[1].off == 14 && msg->emoji[1].len == 7 && strcmp(msg->emoji[1].id, "456") == 0 && msg->emoji[1].animated &&
          strncmp(msg->text + msg->emoji[1].off, ":dance:", 7) == 0);
    /* no emoji, no list; malformed tags stay text */
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"11\",\"channel_id\":\"201\",\"content\":\"plain <:bad:> <:x:12a> <::5>\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    msg = &model_find_channel(m, "201")->msgs[1];
    CHECK(msg->emoji == NULL && msg->nemoji == 0);
    /* mentions before an emoji shift its offset by what they became */
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"12\",\"channel_id\":\"201\",\"content\":\"<@7> <:e:99>\",\"author\":{\"id\":\"1\",\"username\":\"a\"},\"mentions\":[{\"id\":\"7\",\"username\":\"seven\"}]}");
    msg = &model_find_channel(m, "201")->msgs[2];
    CHECK(strcmp(msg->text, "@seven :e:") == 0 && msg->nemoji == 1 && msg->emoji[0].off == 7 && msg->emoji[0].len == 3);
    /* a crowd: only the first MODEL_MAX_EMOJI are recorded, the rest stay as text */
    strcpy(json, "{\"id\":\"13\",\"channel_id\":\"201\",\"content\":\"");
    for (i = 0; i < 30; i++) {
        strcat(json, "<:e:77>");
    }
    strcat(json, "\",\"author\":{\"id\":\"1\",\"username\":\"a\"}}");
    dispatch(m, "MESSAGE_CREATE", json);
    msg = &model_find_channel(m, "201")->msgs[3];
    CHECK(msg->nemoji == MODEL_MAX_EMOJI && strlen(msg->text) == 30 * 3);
    /* an edit re-reads the emoji */
    dispatch(m, "MESSAGE_UPDATE", "{\"id\":\"10\",\"channel_id\":\"201\",\"content\":\"now <:new:5>\",\"edited_timestamp\":\"t\"}");
    msg = model_find_message(model_find_channel(m, "201"), "10");
    CHECK(strcmp(msg->text, "now :new:") == 0 && msg->nemoji == 1 && strcmp(msg->emoji[0].id, "5") == 0 && msg->emoji[0].off == 4);
    dispatch(m, "MESSAGE_UPDATE", "{\"id\":\"10\",\"channel_id\":\"201\",\"content\":\"none\",\"edited_timestamp\":\"t\"}");
    msg = model_find_message(model_find_channel(m, "201"), "10");
    CHECK(msg->emoji == NULL && msg->nemoji == 0);
    model_free(m);
}

static void test_direct_messages(void)
{
    Model *m = start();
    Guild *g;
    Channel *c;
    ModelChange ch[16];

    drain(m, ch, 16);
    /* a user account's READY carries its DMs */
    ready(m, "{\"guilds\":[{\"id\":\"100\",\"unavailable\":true}],\"private_channels\":["
             "{\"id\":\"900\",\"type\":1,\"recipients\":[{\"id\":\"7\",\"username\":\"seven\",\"global_name\":\"Seven\"}]},"
             "{\"id\":\"901\",\"type\":3,\"name\":null,\"recipients\":[{\"id\":\"7\",\"username\":\"seven\"},{\"id\":\"8\",\"username\":\"eight\"},{\"id\":\"42\",\"username\":\"me\"}]},"
             "{\"id\":\"902\",\"type\":3,\"name\":\"The Group\",\"recipients\":[]},"
             "{\"id\":\"903\",\"type\":2},{\"type\":1}]}");
    CHECK(m->nguilds == 2 && strcmp(m->guilds[0].id, MODEL_DM_GUILD) == 0 && strcmp(m->guilds[0].name, "Direct Messages") == 0);
    g = &m->guilds[0];
    CHECK(g->nch == 3);
    c = model_find_channel(m, "900");
    CHECK(c != NULL && c->type == CH_DM && strcmp(c->name, "Seven") == 0 && strcmp(c->guild_id, "@me") == 0);
    c = model_find_channel(m, "901");
    CHECK(c != NULL && c->type == CH_GROUP_DM && strcmp(c->name, "seven, eight") == 0);        /* we are left out of the name */
    CHECK(model_find_channel(m, "902") != NULL && strcmp(model_find_channel(m, "902")->name, "The Group") == 0);
    CHECK(model_find_channel(m, "903") == NULL);

    /* a message in a DM always counts as a mention */
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"1\",\"channel_id\":\"900\",\"content\":\"psst\",\"author\":{\"id\":\"7\",\"username\":\"seven\"}}");
    c = model_find_channel(m, "900");
    CHECK(c->nmsgs == 1 && c->unread == 1 && c->mentions == 1);

    /* a message in a conversation we have not heard of makes its channel */
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"2\",\"channel_id\":\"950\",\"content\":\"hello?\",\"author\":{\"id\":\"9\",\"username\":\"nine\",\"global_name\":\"Nine\"}}");
    c = model_find_channel(m, "950");
    CHECK(c != NULL && c->type == CH_DM && strcmp(c->name, "Nine") == 0 && c->nmsgs == 1 && c->unread == 1);
    /* ... but not for a guild channel we do not know (a thread), and not one we wrote in ourselves unnamed */
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"3\",\"channel_id\":\"951\",\"guild_id\":\"100\",\"content\":\"x\",\"author\":{\"id\":\"9\",\"username\":\"nine\"}}");
    CHECK(model_find_channel(m, "951") == NULL);
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"4\",\"channel_id\":\"952\",\"content\":\"from my phone\",\"author\":{\"id\":\"42\",\"username\":\"me\"}}");
    c = model_find_channel(m, "952");
    CHECK(c != NULL && strcmp(c->name, "conversation") == 0 && c->unread == 0);

    /* channel events without a guild */
    dispatch(m, "CHANNEL_CREATE", "{\"id\":\"960\",\"type\":1,\"recipients\":[{\"id\":\"5\",\"username\":\"five\"}]}");
    CHECK(model_find_channel(m, "960") != NULL && strcmp(model_find_channel(m, "960")->name, "five") == 0);
    dispatch(m, "CHANNEL_DELETE", "{\"id\":\"960\",\"type\":1}");
    CHECK(model_find_channel(m, "960") == NULL);
    dispatch(m, "CHANNEL_DELETE", "{\"id\":\"961\",\"type\":1}");

    /* a new session keeps the DMs (they are not a server READY lists) and stays first */
    ready(m, "{\"guilds\":[{\"id\":\"100\",\"unavailable\":true},{\"id\":\"105\",\"unavailable\":true}]}");
    CHECK(m->nguilds == 3 && strcmp(m->guilds[0].id, "@me") == 0 && model_find_channel(m, "900") != NULL &&
          model_find_channel(m, "900")->nmsgs == 1);
    /* DMs created before any server exists still come first */
    model_free(m);
    model_init(m);
    ready(m, "{\"guilds\":[{\"id\":\"100\",\"unavailable\":true}]}");
    dispatch(m, "MESSAGE_CREATE", "{\"id\":\"5\",\"channel_id\":\"970\",\"content\":\"hi\",\"author\":{\"id\":\"6\",\"username\":\"six\"}}");
    CHECK(m->nguilds == 2 && strcmp(m->guilds[0].id, "@me") == 0 && strcmp(m->guilds[1].id, "100") == 0);
    model_free(m);
}

static unsigned long rs = 99;
static unsigned rnd(void)
{
    rs = rs * 1103515245UL + 12345UL;
    return (unsigned)((rs >> 16) & 0x7fff);
}

static void test_fuzz(void)
{
    static const char *names[] = { "GUILD_CREATE", "GUILD_UPDATE", "GUILD_DELETE", "CHANNEL_CREATE", "CHANNEL_UPDATE",
                                   "CHANNEL_DELETE", "MESSAGE_CREATE", "MESSAGE_UPDATE", "MESSAGE_DELETE", "MESSAGE_DELETE_BULK" };
    const char *seeds[5];
    Model *m = start();
    char buf[2048];
    int i, k, n;
    JDoc doc;
    GwEvent ev;
    ModelChange ch;

    seeds[0] = GUILD;
    seeds[1] = "{\"id\":\"220\",\"guild_id\":\"100\",\"name\":\"n\",\"type\":0,\"position\":3,\"parent_id\":\"210\"}";
    seeds[2] = "{\"id\":\"1\",\"channel_id\":\"201\",\"content\":\"<@7> <#202> <:x:1> text\",\"author\":{\"id\":\"8\",\"username\":\"b\"},\"mentions\":[{\"id\":\"7\"}],\"attachments\":[{\"filename\":\"f\"}]}";
    seeds[3] = "{\"id\":\"1\",\"channel_id\":\"201\",\"content\":\"edit\",\"edited_timestamp\":\"t\"}";
    seeds[4] = "{\"ids\":[\"1\",\"2\"],\"channel_id\":\"201\"}";
    for (i = 0; i < 20000; i++) {
        strcpy(buf, seeds[rnd() % (sizeof(seeds) / sizeof(seeds[0]))]);
        n = (int)strlen(buf);
        for (k = (int)(rnd() % 4); k > 0; k--) {
            int at = (int)(rnd() % (unsigned)n);

            switch (rnd() % 4) {
            case 0: buf[at] = (char)(rnd() & 0x7f); break;
            case 1: buf[at] = "0123456789\"{}[]<>@#:,"[rnd() % 21]; break;
            case 2: buf[at] = ' '; break;
            default: n = at + 1; buf[n] = '\0'; break;
            }
        }
        memset(&ev, 0, sizeof(ev));
        memset(&doc, 0, sizeof(doc));
        ev.type = GW_EV_DISPATCH;
        strcpy(ev.name, names[rnd() % (sizeof(names) / sizeof(names[0]))]);
        ev.d = json_parse(&doc, buf, (size_t)n);
        model_gateway_event(m, &ev);
        if (i % 7 == 0) {
            model_ingest_history(m, "201", ev.d);
            model_ingest_guilds(m, ev.d);
            model_ingest_channels(m, "100", ev.d);
        }
        json_free(&doc);
        if (rnd() % 5 == 0) {
            while (model_next_change(m, &ch)) {
                ;
            }
        }
        if (i % 500 == 0) {
            dispatch(m, "GUILD_CREATE", GUILD);
        }
    }
    CHECK(m->nchanges <= MODEL_MAX_CHANGES);
    model_free(m);
}

static void test_ids(void)
{
    CHECK(model_id_cmp("1", "2") < 0 && model_id_cmp("10", "9") > 0 && model_id_cmp("007", "7") == 0);
    CHECK(model_id_cmp("1000000000000000000", "999999999999999999") > 0 && model_id_cmp("5", "5") == 0);
    CHECK(model_id_cmp("0", "0") == 0 && model_id_cmp("0", "1") < 0);
}

int main(void)
{
    test_ids();
    test_guilds_and_channels();
    test_rest_lists();
    test_messages();
    test_update_delete();
    test_history_and_cap();
    test_reconnect();
    test_direct_messages();
    test_attachments();
    test_embeds();
    test_stickers();
    test_custom_emoji();
    test_fuzz();
    TEST_DONE("model");
}
