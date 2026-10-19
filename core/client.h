/*
 * client.h -- the Discord client engine: everything between "here is a token" and "here are the servers,
 * channels and messages", with no user interface.  It owns the gateway session, the REST queue and the model
 * (see gateway.h, rest.h, model.h), and the two network connections they run over (netconn.h), and is driven
 * by polling: call kc_tick() from a timer (the app uses 20 ms), then drain kc_next_event() and the model's
 * change queue (kc_next_change()).
 *
 * The flow: kc_start() first asks REST who the token belongs to (so a bad token is reported at once and by
 * name), then opens the gateway and keeps it open, reconnecting and resuming by itself.  History for a
 * channel is fetched on request (kc_open_channel()); a message is sent with kc_send_message(), and shows up
 * through the model as the gateway echoes it back.
 */
#ifndef KALLISTI_CLIENT_H
#define KALLISTI_CLIENT_H

#include "gateway.h"
#include "rest.h"
#include "model.h"
#include "netconn.h"
#include "http.h"
#include "ws.h"

enum { KC_IDLE = 0, KC_VERIFYING, KC_CONNECTING, KC_ONLINE, KC_FATAL, KC_STOPPED };
enum { KE_STATE = 1, KE_ERROR };

typedef struct {
    int type;                    /* KE_STATE: the client's state changed; KE_ERROR: something to tell the user */
    int state;                   /* KE_STATE: the new KC_* state */
    char text[240];              /* a sentence for the status line or an alert */
} KEvent;

#define KC_MAX_PENDING 24
#define KC_HISTORY_LIMIT 50

typedef struct {
    long id;
    int kind;
    char channel_id[24];
} KcPending;

typedef struct {
    /* configuration: set between kc_init() and kc_start() */
    char token[200];
    int is_bot;                  /* 0: a user account token (against Discord's terms; experimental) */
    char rest_host[256];
    int rest_port, rest_tls;
    char gateway_url[320];       /* "" for Discord's own */
    const TrustStore *trust;     /* required for TLS; must outlive the client */
    int check_dates;             /* validate certificate dates against the clock */

    int state;
    char user_name[80];

    RestClient rest;
    Gateway gw;
    Model model;
    int started;

    NetConn rconn;
    HttpReq rreq;
    HttpResp rresp;
    int rphase;                  /* 0 idle, 1 connecting, 2 waiting for the reply */
    int rreused, rgot;
    KcPending pend[KC_MAX_PENDING];
    int npend;
    long verify_id;

    NetConn gconn;
    WsClient ws;
    HttpResp gresp;
    int gphase;                  /* 0 none, 1 waiting to connect, 2 connecting, 3 upgrading, 4 open */
    char gurl[320];
    KMs gconnect_at, gphase_at;

    KEvent evs[16];
    int nevs;
} KClient;

void kc_init(KClient *kc, const char *token, int is_bot);
void kc_free(KClient *kc);

/* Begins. Needs a seeded random pool (ssh_rng_ready()) for TLS. */
void kc_start(KClient *kc, KMs now);
void kc_stop(KClient *kc);
void kc_tick(KClient *kc, KMs now);

int kc_next_event(KClient *kc, KEvent *ev);
int kc_next_change(KClient *kc, ModelChange *ch);
Model *kc_model(KClient *kc);

/* Fetches the channel's recent messages if they are not loaded yet (and not already being fetched). */
void kc_open_channel(KClient *kc, const char *channel_id);
/* Queues a message; returns 0, or a REST_E_* code (empty, too long, queue full). Failures that happen later
 * come back as KE_ERROR events. */
int kc_send_message(KClient *kc, const char *channel_id, const char *text);

#endif
