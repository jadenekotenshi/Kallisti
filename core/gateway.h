/*
 * gateway.h -- Discord's gateway session (API v10, JSON, uncompressed) as a sans-I/O state machine.
 *
 * The gateway never touches a socket or a clock.  The driver (the app, or a test) feeds it:
 *   - gw_start()/gw_stop() to begin and end,
 *   - gw_connection_opened() once a WebSocket handshake has succeeded, gw_connection_failed() if a
 *     connection attempt failed, gw_connection_lost() when an open one dropped,
 *   - every WsEvent that arrives, through gw_handle_ws_event(),
 *   - gw_tick() whenever gw_next_deadline() says something is due,
 * and after any of those drains two queues:
 *   - gw_next_command(): GW_CMD_CONNECT (open a WebSocket to this URL after delay_ms) and
 *     GW_CMD_CLOSE (flush the output and drop the connection),
 *   - gw_next_event(): READY, RESUMED, DISCONNECTED, FATAL and every DISPATCH (event name plus its
 *     JSON "d" payload).
 * Frames the gateway sends go straight onto the WsClient it was given; the driver flushes ws_output().
 *
 * What it does for you: answers HELLO with IDENTIFY (or RESUME when a session can be resumed),
 * heartbeats with the interval and jitter Discord asks for and treats a missing ACK as a dead
 * connection, tracks the sequence number, honours RECONNECT (op 7) and INVALID_SESSION (op 9),
 * reconnects with exponential backoff, and classifies close codes: a bad token (4004), invalid or
 * disallowed intents (4013/4014), and the sharding/version errors are fatal, everything else
 * reconnects, resuming unless the code (4007, 4009, 1000, 1001) invalidates the session.
 *
 * Times are GwMs, unsigned 32-bit milliseconds from any monotonic clock; wraparound is handled.
 */
#ifndef KALLISTI_GATEWAY_H
#define KALLISTI_GATEWAY_H

#include <stddef.h>
#include "json.h"
#include "kms.h"
#include "ws.h"

typedef KMs GwMs;

#define GW_DEFAULT_URL "wss://gateway.discord.gg/?v=10&encoding=json"

#define GW_INTENT_GUILDS           (1L << 0)
#define GW_INTENT_GUILD_MESSAGES   (1L << 9)
#define GW_INTENT_GUILD_REACTIONS  (1L << 10)
#define GW_INTENT_DIRECT_MESSAGES  (1L << 12)
#define GW_INTENT_DM_REACTIONS     (1L << 13)
#define GW_INTENT_MESSAGE_CONTENT  (1L << 15)    /* privileged: switch it on in the Developer Portal */

typedef enum {
    GW_IDLE = 0,             /* not started, or stopped */
    GW_WAIT_CONNECT,         /* a GW_CMD_CONNECT is outstanding */
    GW_WAIT_HELLO,           /* connected, waiting for HELLO */
    GW_AUTHENTICATING,       /* IDENTIFY/RESUME sent, waiting for READY/RESUMED */
    GW_READY,
    GW_FATAL                 /* will not reconnect: see the FATAL event */
} GwState;

enum { GW_CMD_NONE = 0, GW_CMD_CONNECT, GW_CMD_CLOSE };

typedef struct {
    int type;
    char url[320];           /* CONNECT: the wss:// URL */
    GwMs delay_ms;           /* CONNECT: wait this long first */
} GwCommand;

enum { GW_EV_NONE = 0, GW_EV_READY, GW_EV_RESUMED, GW_EV_DISPATCH, GW_EV_DISCONNECTED, GW_EV_FATAL };

typedef struct {
    int type;
    char name[48];           /* DISPATCH: the event name ("MESSAGE_CREATE", ...); READY: our user name */
    const JNode *d;          /* DISPATCH/READY: the "d" payload, valid until the next gw_handle_ws_event() */
    long seq;                /* DISPATCH: the sequence number, or -1 */
    char user_id[32];        /* READY: our own user id */
    int code;                /* DISCONNECTED/FATAL: the close code (0 if the connection simply failed) */
    int will_reconnect;      /* DISCONNECTED */
    char text[160];          /* FATAL: what is wrong, for the user */
} GwEvent;

typedef struct {
    char token[200];
    long intents;

    GwState state;
    WsClient *ws;                        /* the open connection, or NULL */
    int (*rng)(void *out, size_t len);   /* jitter source (ssh_rng_bytes); tests substitute their own */

    char session_id[128];
    char resume_url[320];
    char user_id[32];
    char user_name[80];
    long seq;                            /* last sequence number seen, -1 if none */
    int can_resume;

    GwMs hb_interval;           /* 0 until HELLO */
    GwMs next_hb;
    int ack_pending;
    GwMs hb_sent_at;
    long latency_ms;                     /* last heartbeat round trip, -1 if none yet */

    int auth_pending;                    /* an IDENTIFY/RESUME is waiting for auth_at (after INVALID_SESSION) */
    GwMs auth_at;

    int failures;                        /* consecutive failed connections, for the backoff */

    GwCommand cmds[4];
    int ncmds;
    GwEvent evs[6];
    int nevs;
    JDoc doc;                            /* the frame behind the DISPATCH event handed out last */
    int have_doc;
    int bad_frames;                      /* unparseable or malformed frames seen (diagnostics) */
    char fatal[160];
} Gateway;

void gw_init(Gateway *gw, const char *token, long intents);
void gw_free(Gateway *gw);

/* Begin connecting (queues a CONNECT command). */
void gw_start(Gateway *gw, GwMs now);
/* The user disconnects: sends a normal close, queues CLOSE, and does not reconnect. */
void gw_stop(Gateway *gw, GwMs now);

void gw_connection_opened(Gateway *gw, WsClient *ws, GwMs now);
void gw_connection_failed(Gateway *gw, GwMs now);
void gw_connection_lost(Gateway *gw, int close_code, GwMs now);

void gw_handle_ws_event(Gateway *gw, const WsEvent *ev, GwMs now);
void gw_tick(Gateway *gw, GwMs now);
/* Milliseconds until gw_tick() has something to do, or -1 if nothing is scheduled. */
long gw_next_deadline(const Gateway *gw, GwMs now);

int gw_next_command(Gateway *gw, GwCommand *cmd);
int gw_next_event(Gateway *gw, GwEvent *ev);

/* Close-code policy, exposed for tests: 1 if the code means "do not reconnect" (*why describes it),
 * and whether it invalidates the session. */
int gw_close_is_fatal(int code, const char **why);
int gw_close_invalidates_session(int code);

#endif
