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
#include "img.h"

enum { KC_IDLE = 0, KC_VERIFYING, KC_CONNECTING, KC_ONLINE, KC_FATAL, KC_STOPPED };
enum { KE_STATE = 1, KE_ERROR, KE_CHANNEL };

typedef struct {
    int type;                    /* KE_STATE: the client's state changed; KE_ERROR: something to tell the user;
                                  * KE_CHANNEL: a conversation the user asked for is ready, show it */
    int state;                   /* KE_STATE: the new KC_* state */
    char text[240];              /* a sentence for the status line or an alert; KE_CHANNEL: the channel id */
} KEvent;

#define KC_MAX_PENDING 24
#define KC_HISTORY_LIMIT 50

typedef struct {
    long id;
    int kind;
    char channel_id[24];
} KcPending;

#define KC_IMAGE_MAX_W 800UL        /* pictures are fetched at most this large (Discord resizes them for us) */
#define KC_IMAGE_MAX_H 600UL
#define KC_IMAGE_QUEUE 4
#define KC_FETCH_JPEG 1             /* ask the media proxy for a JPEG instead of a PNG (for photographs) */
#define KC_IMAGE_BODY_MAX (12L * 1024L * 1024L)

/* A fetched picture, ready for AppKit: an uncompressed TIFF. */
typedef struct {
    char url[512];               /* the address asked for (as given to kc_fetch_image) */
    unsigned long tag;           /* whatever the caller passed in, to tell its requests apart */
    int ok;
    char error[160];             /* when !ok */
    unsigned char *tiff;         /* malloc'd; the receiver frees it */
    size_t tiff_len;
    unsigned long width, height;
    unsigned long ms_connect, ms_download, ms_decode;   /* where the time went: TLS setup, transfer, PNG -> TIFF */
    unsigned long bytes;         /* size of the PNG that was downloaded */
} KImage;

typedef struct {
    /* configuration: set between kc_init() and kc_start() */
    char token[200];
    int is_bot;                  /* 0: a user account token (against Discord's terms; experimental) */
    char rest_host[256];
    int rest_port, rest_tls;
    char gateway_url[320];       /* "" for Discord's own */
    const TrustStore *trust;     /* required for TLS; must outlive the client */
    int check_dates;             /* validate certificate dates against the clock */
    char proxy_spec[256];        /* "[http://|socks5://][user:pass@]host[:port]", or "" for none (see netconn.h) */
    char proxy_user[128];        /* a login kept apart from the address (wins over one in it) */
    char proxy_pass[128];
    int resize_any_host;         /* add format/size parameters to picture addresses on every host, not only Discord's
                                  * (tests, whose fake server is on localhost, set this) */
    int proxy_env;               /* with no proxy_spec, use https_proxy / all_proxy / http_proxy (default 1) */
    NetProxy proxy;              /* the parsed setting, once started */
    int proxy_set;
    char proxy_label[80];        /* "host:port" of the proxy in use (configured, or from the environment), for the status line */

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

    /* picture downloads (one at a time, each on a connection of its own) */
    struct { char url[512]; unsigned long w, h, max_w, max_h, tag; int flags; } fq[KC_IMAGE_QUEUE];
    int nfq;
    KImage imgs[KC_IMAGE_QUEUE];
    int nimgs;
    NetConn fconn;
    HttpReq freq;
    HttpResp fresp;
    int fphase;                  /* 0 idle, 1 connecting, 2 waiting for the reply */
    char furl[512];              /* the address being fetched now (after redirects) */
    char forig[512];             /* ... and the one that was asked for */
    unsigned long fmax_w, fmax_h, ftag;
    int fflags, fretried, freused, fgot;
    KMs fnow, fidle_since;
    unsigned long fopens;        /* connections opened for pictures (a kept connection is not a new one) */
    unsigned long fq_w_saved, fq_h_saved;      /* the size asked for, kept for a retry in another format */
    int fredirects;
    KMs fstarted;
    unsigned long fwall_start, fwall_open, fwall_done, fbytes, fms_decode;

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
/* Queues a picture download: `url` as found in a message's attachment, `w` x `h` its size (0 if unknown). Discord's
 * media proxy is asked for a PNG of at most KC_IMAGE_MAX_W x KC_IMAGE_MAX_H, which is decoded, flattened onto
 * white and handed back by kc_next_image(). Returns 0, or -1 if the queue is full or the address is no good. */
int kc_fetch_image(KClient *kc, const char *url, unsigned long w, unsigned long h);
/* The same with a size limit of your own (e.g. a thumbnail) and a tag that comes back in the KImage. */
int kc_fetch_image_boxed(KClient *kc, const char *url, unsigned long w, unsigned long h, unsigned long max_w,
                         unsigned long max_h, unsigned long tag);
/* ... and KC_FETCH_* flags. Whichever format arrives (PNG or JPEG) is decoded. */
int kc_fetch_image_ex(KClient *kc, const char *url, unsigned long w, unsigned long h, unsigned long max_w,
                      unsigned long max_h, unsigned long tag, int flags);
/* One finished download (success or failure). The caller owns out->tiff. */
int kc_next_image(KClient *kc, KImage *out);

/* Opens a direct message with a user (by id). Returns 0 or a REST_E_* code; the answer is a KE_CHANNEL event,
 * or a KE_ERROR (for a bot: the user must share a server with it). */
int kc_open_dm(KClient *kc, const char *user_id);

#endif
