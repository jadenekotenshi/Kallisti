/*
 * model.h -- what the client knows about Discord: the servers (guilds) we are in, their text channels and
 * categories, and the most recent messages of each channel.  It is fed by gateway events (READY,
 * GUILD_CREATE/UPDATE/DELETE, CHANNEL_CREATE/UPDATE/DELETE, MESSAGE_CREATE/UPDATE/DELETE) and by REST
 * results (the server list, a channel list, message history), and it tells the UI what changed through a
 * queue of ModelChange records.
 *
 * Guild text (0), announcement (5) and category (4) channels are kept, and direct messages (1) and group DMs (3)
 * under the pseudo-server "@me"; threads, voice and forums are ignored.  Channel visibility is NOT filtered by permissions (that needs role and overwrite
 * arithmetic); a channel the bot cannot see simply fails with a 403 when opened.
 *
 * Message text is stored ready to display: user, role and channel mentions and custom emoji are replaced
 * by "@name", "@role", "#channel" and ":name:", attachments are listed as "[attachment: file]".
 *
 * Pointers returned by the find functions are valid until the next call that ingests data.  Identify things
 * by their id strings (Discord snowflakes) across calls.
 */
#ifndef KALLISTI_MODEL_H
#define KALLISTI_MODEL_H

#include <stddef.h>
#include "gateway.h"
#include "json.h"

#define MODEL_MAX_MESSAGES 200       /* kept per channel; older ones fall off */
#define MODEL_MAX_CHANGES 128

enum { CH_TEXT = 0, CH_DM = 1, CH_GROUP_DM = 3, CH_CATEGORY = 4, CH_ANNOUNCEMENT = 5 };

#define MODEL_DM_GUILD "@me"            /* the pseudo-server that holds the direct-message channels */

#define MODEL_MAX_ATTACHMENTS 4

typedef struct {
    char url[480];               /* where to fetch it: Discord's media proxy URL if it sent one */
    char name[100];
    int width, height;           /* 0 if unknown */
    int is_image;                /* an image content type (or one with a size) */
} Attachment;

typedef struct {
    char id[24];
    char author_id[24];
    char author[132];            /* nickname, display name or user name */
    char time[40];               /* ISO 8601 as Discord sends it */
    char *text;                  /* display text, never NULL, owned by the model */
    int edited;
    int bot;
    int type;                    /* Discord's message type: 0 default, 19 reply, others are system messages */
    Attachment *att;             /* the first few attachments, or NULL; owned by the model */
    int natt;
} Message;

typedef struct {
    char id[24];
    char guild_id[24];
    char parent_id[24];          /* the category, or "" */
    char name[132];
    int type;
    int position;
    Message *msgs;               /* oldest first */
    int nmsgs, cap;
    int unread;                  /* messages that arrived while nobody had marked the channel read */
    int mentions;                /* of those, how many mention us */
    int history_loaded;
} Channel;

typedef struct {
    char id[24];
    char name[132];
    int unavailable;             /* an outage, or a READY stub whose GUILD_CREATE has not arrived */
    int stale;                   /* from before a new READY, not yet re-sent */
    Channel *ch;                 /* sorted for display: see model_sort_channels() */
    int nch, chcap;
} Guild;

enum {
    MC_RESET = 1,                /* too much changed (or the queue overflowed): refresh everything */
    MC_GUILDS,                   /* the server list changed */
    MC_CHANNELS,                 /* guild_id: its channel list changed */
    MC_MESSAGE_ADDED,            /* channel_id, message_id */
    MC_MESSAGE_CHANGED,
    MC_MESSAGE_REMOVED,
    MC_HISTORY,                  /* channel_id: history was merged in */
    MC_UNREAD                    /* channel_id: its unread/mention counters changed */
};

typedef struct {
    int type;
    char guild_id[24];
    char channel_id[24];
    char message_id[24];
} ModelChange;

typedef struct {
    Guild *guilds;
    int nguilds, gcap;
    char user_id[24];
    char user_name[80];
    ModelChange changes[MODEL_MAX_CHANGES];
    int nchanges;
    int overflow;
} Model;

void model_init(Model *m);
void model_free(Model *m);

/* Feeds one gateway event (READY, RESUMED and DISPATCH matter; others are ignored). */
void model_gateway_event(Model *m, const GwEvent *ev);

/* REST results: GET /users/@me/guilds, GET /guilds/{id}/channels, GET /channels/{id}/messages, and the message
 * object a POST returns.  `arr`/`msg` is the parsed JSON body. */
void model_ingest_guilds(Model *m, const JNode *arr);
void model_ingest_channels(Model *m, const char *guild_id, const JNode *arr);
void model_ingest_history(Model *m, const char *channel_id, const JNode *arr);
void model_ingest_message(Model *m, const JNode *msg, int from_gateway);
/* A direct-message channel object (from READY, CHANNEL_CREATE, or the reply to opening a DM): it goes into the
 * "Direct Messages" pseudo-server (MODEL_DM_GUILD), created on first use and kept first in the list. */
void model_ingest_private_channel(Model *m, const JNode *chan);

Guild *model_find_guild(Model *m, const char *id);
Channel *model_find_channel(Model *m, const char *channel_id);
Message *model_find_message(Channel *c, const char *message_id);

void model_mark_read(Model *m, const char *channel_id);
int model_next_change(Model *m, ModelChange *out);

/* Snowflake ordering (they are decimal strings): <0, 0, >0. */
int model_id_cmp(const char *a, const char *b);

#endif
