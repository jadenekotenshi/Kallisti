#import "AppController.h"
#import "UIHelpers.h"
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "rng.h"
#include "roots.h"
#include "ktime.h"
#include "oscompat.h"

static NSString *S(const char *s) { return ui_string_from_utf8(s); }

/* A millisecond clock for the engine; only differences matter, so wrapping every 49 days is fine. */
static KMs app_now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (KMs)((unsigned long)tv.tv_sec * 1000UL + (unsigned long)tv.tv_usec / 1000UL);
}

/* ---------------------------------------------------------------- */
/* Entropy meter: a small panel that turns mouse and key timing into credited random bits.  Copied from
 * RatChat (which copied StepSSH's, hardware-confirmed there): OPENSTEP 4.2 has no /dev/urandom worth
 * trusting, so a TLS handshake's ephemeral keys need this fallback. */

@interface EntropyMeter : NSView
{
    id   target;
    int  lastX, lastY;
}
- (void)setTarget:(id)t;
@end

@implementation EntropyMeter
- (void)setTarget:(id)t { target = t; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (BOOL)isOpaque { return YES; }

- (void)feed:(NSEvent *)e
{
    NSPoint p = [e locationInWindow];
    int x = (int)p.x, y = (int)p.y;
    if (x == lastX && y == lastY) return;              /* only movement counts */
    lastX = x; lastY = y;
    ssh_rng_add(&p, sizeof(p), 0);
    ssh_rng_add_timing(1);                             /* conservative: one bit per event */
    [self setNeedsDisplay:YES];
    if (ssh_rng_ready()) [target entropyReady];
}
- (void)mouseMoved:(NSEvent *)e   { [self feed:e]; }
- (void)mouseDragged:(NSEvent *)e { [self feed:e]; }
- (void)mouseDown:(NSEvent *)e    { [self feed:e]; }
- (void)keyDown:(NSEvent *)e      { ssh_rng_add_timing(1); [self setNeedsDisplay:YES]; if (ssh_rng_ready()) [target entropyReady]; }

- (void)drawRect:(NSRect)rect
{
    NSRect b = [self bounds], bar;
    float frac = (float)ssh_rng_credited() / (float)SSH_RNG_MIN_BITS;
    if (frac > 1.0) frac = 1.0;
    [[NSColor controlBackgroundColor] set];
    NSRectFill(b);
    [[NSColor blackColor] set];
    NSFrameRect(b);
    bar = NSInsetRect(b, 2, 2);
    bar.size.width *= frac;
    [[NSColor selectedControlColor] set];
    NSRectFill(bar);
}
@end

/* gcc 2.7.2 does not look ahead within an @implementation. */
@interface AppController (Private)
- (void)prepareDirectory;
- (void)showLogin:(id)sender;
- (void)disconnect:(id)sender;
- (void)toggleDates:(id)sender;
- (void)beginWithToken:(char *)token;
- (void)showEntropyPanel;
- (void)tick:(NSTimer *)t;
- (void)drainEngine;
- (void)endSession;
- (void)savePrefs;
- (void)openCurrentInWindow:(id)sender;
- (void)showNewDM:(id)sender;
- (void)closeChannelWindows;
- (void)showImages:(id)sender;
- (void)showReact:(id)sender;
- (void)downloadArrived:(KImage *)img;
- (void)toggleInline:(id)sender;
- (void)toggleEmoji:(id)sender;
- (NSString *)findEmojiPack;
- (void)applyInlineSetting;
- (void)reloadAllMessages;
- (void)closeImageWindows;
- (void)sendText:(NSString *)text toChannel:(NSString *)channelId notify:(id)target;
- (void)attachFile:(id)sender;
@end

@implementation AppController

- (id)init
{
    self = [super init];
    if (!self) return nil;
    kallistiDir = [[NSHomeDirectory() stringByAppendingPathComponent:@".kallisti"] retain];
    prefsPath = [[kallistiDir stringByAppendingPathComponent:@"prefs"] retain];
    rngSeedPath = [[kallistiDir stringByAppendingPathComponent:@"rng_seed"] retain];
    trust_init(&trust);
    kprefs_defaults(&prefs);
    channelWindows = [[NSMutableArray alloc] init];
    imageWindows = [[NSMutableArray alloc] init];
    imageNames = [[NSMutableDictionary alloc] init];
    downloads = [[NSMutableDictionary alloc] init];
    thumbs = [[NSMutableDictionary alloc] init];
    thumbsAsked = [[NSMutableSet alloc] init];
    emojiImages = [[NSMutableDictionary alloc] init];
    return self;
}

- (void)dealloc
{
    [self endSession];
    ui_set_emoji_pack(NULL);
    emoji_pack_close(emojiPack);
    trust_free(&trust);
    memset(&prefs, 0, sizeof(prefs));
    [loginController release];
    [mainController release];
    [channelWindows release];
    [dmController release];
    [imageList release]; [reactController release]; [downloads release]; [lastTypingChannel release]; [imageWindows release]; [imageNames release]; [thumbs release]; [thumbsAsked release]; [emojiBase release]; [emojiImages release];
    [kallistiDir release]; [prefsPath release]; [rngSeedPath release];
    [entropyPanel release];
    [restHost release]; [gatewayURL release];
    [super dealloc];
}

- (void)prepareDirectory
{
    struct stat st;
    const char *d = [kallistiDir cString];

    if (stat(d, &st) != 0) mkdir(d, 0700);
}

- (void)savePrefs
{
    kprefs_save(&prefs, [prefsPath cString]);
}

/* ---------------------------------------------------------------- menus */

- (void)addItem:(NSString *)title action:(SEL)action key:(NSString *)key
         target:(id)target toMenu:(NSMenu *)menu
{
    id item = [menu addItemWithTitle:title action:action keyEquivalent:key];
    if (target) [item setTarget:target];
}

- (NSMenu *)submenuNamed:(NSString *)title inMenu:(NSMenu *)parent
{
    NSMenu *sub = [[NSMenu alloc] initWithTitle:title];
    id item = [parent addItemWithTitle:title action:NULL keyEquivalent:@""];
    [parent setSubmenu:sub forItem:item];
    return [sub autorelease];
}

- (void)buildMenu
{
    NSMenu *main = [[[NSMenu alloc] initWithTitle:@"Kallisti"] autorelease];
    NSMenu *m;

    m = [self submenuNamed:@"Discord" inMenu:main];
    [self addItem:@"Sign In..." action:@selector(showLogin:) key:@"n" target:self toMenu:m];
    [self addItem:@"Disconnect" action:@selector(disconnect:) key:@"d" target:self toMenu:m];
    [self addItem:@"New Direct Message..." action:@selector(showNewDM:) key:@"t" target:self toMenu:m];
    [self addItem:@"Open Channel in New Window" action:@selector(openCurrentInWindow:) key:@"o" target:self toMenu:m];
    [self addItem:@"Attachments in This Channel..." action:@selector(showImages:) key:@"i" target:self toMenu:m];
    [self addItem:@"React to a Message..." action:@selector(showReact:) key:@"r" target:self toMenu:m];
    [self addItem:@"Attach File..." action:@selector(attachFile:) key:@"u" target:self toMenu:m];
    [self addItem:@"Show Images Inline (Experimental)" action:@selector(toggleInline:) key:@"" target:self toMenu:m];
    inlineItem = [m itemWithTitle:@"Show Images Inline (Experimental)"];
    [self addItem:@"Show Emoji as Pictures" action:@selector(toggleEmoji:) key:@"" target:self toMenu:m];
    emojiItem = [m itemWithTitle:@"Show Emoji as Pictures"];
    [self addItem:@"Check Certificate Dates" action:@selector(toggleDates:) key:@"" target:self toMenu:m];
    datesItem = [m itemWithTitle:@"Check Certificate Dates"];

    m = [self submenuNamed:@"Edit" inMenu:main];
    [self addItem:@"Copy" action:@selector(copy:) key:@"c" target:nil toMenu:m];
    [self addItem:@"Paste" action:@selector(paste:) key:@"v" target:nil toMenu:m];
    [self addItem:@"Select All" action:@selector(selectAll:) key:@"a" target:nil toMenu:m];

    m = [self submenuNamed:@"Windows" inMenu:main];
    [self addItem:@"Arrange in Front" action:@selector(arrangeInFront:) key:@"" target:nil toMenu:m];
    [self addItem:@"Miniaturize Window" action:@selector(performMiniaturize:) key:@"m" target:nil toMenu:m];
    [NSApp setWindowsMenu:m];

    m = [self submenuNamed:@"Services" inMenu:main];
    [NSApp setServicesMenu:m];

    [self addItem:@"Hide" action:@selector(hide:) key:@"h" target:NSApp toMenu:main];
    [self addItem:@"Quit" action:@selector(terminate:) key:@"q" target:NSApp toMenu:main];

    [NSApp setMainMenu:main];
}

- (void)useRestHost:(NSString *)host port:(int)port tls:(BOOL)tls gatewayURL:(NSString *)url
{
    [restHost release]; restHost = [host retain];
    [gatewayURL release]; gatewayURL = [url retain];
    restPort = port;
    restTLS = tls ? 1 : 0;
    resizeAnyHost = 1;
}

- (void)signInWithToken:(NSString *)token isBot:(BOOL)isBot
{
    char *copy = (char *)malloc([token length] + 1);

    strcpy(copy, [token cString]);
    prefs.is_bot = isBot ? 1 : 0;
    [self beginWithToken:copy];
}

- (MainController *)mainController { return mainController; }
- (KClient *)client { return kc; }

- (void)toggleDates:(id)sender
{
    prefs.check_dates = !prefs.check_dates;
    [datesItem setTitle:prefs.check_dates ? @"Check Certificate Dates" : @"Ignore Certificate Dates"];
    [self savePrefs];
    if (!prefs.check_dates) {
        NSRunAlertPanel(@"Certificate dates are not checked",
                        @"Kallisti will accept certificates that look expired or not yet valid. Use this only because the computer's clock is wrong; it takes effect the next time you sign in.",
                        @"OK", nil, nil);
    }
}

/* ---------------------------------------------------------------- startup */

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    int count = 0, source;

    NSLog(@"Kallisti: applicationDidFinishLaunching");
    SSTrace("applicationDidFinishLaunching");
    [self prepareDirectory];
    kprefs_load(&prefs, [prefsPath cString]);
    [datesItem setTitle:prefs.check_dates ? @"Check Certificate Dates" : @"Ignore Certificate Dates"];
    ssh_rng_seed_system();
    ssh_rng_load_seed([rngSeedPath cString]);
    SSTrace("random pool holds %d of %d bits", ssh_rng_credited(), SSH_RNG_MIN_BITS);
    if (ssh_rng_ready()) ssh_rng_save_seed([rngSeedPath cString]);

    {
        NSString *packPath = [self findEmojiPack];

        if (packPath && [self loadEmojiPackFromPath:packPath]) {
            SSTrace("emoji pictures: %d from %s", emoji_pack_count(emojiPack), SSCS(packPath));
        } else {
            SSTrace("emoji pictures: no pack found (emoji will show as ?)");
        }
        [self setEmojiPictures:prefs.emoji_pictures ? YES : NO];
    }

    source = roots_load_default(&trust, ROOTS_SYSTEM_DIR, &count);
    SSTrace("trust store: %d certificates from %s", count,
            source == ROOTS_FROM_DIR ? ROOTS_SYSTEM_DIR : (source == ROOTS_FROM_BUNDLED ? "the compiled-in set" : "nowhere"));
    [self showLogin:nil];
}

- (void)showLogin:(id)sender
{
    if (kc) {
        NSRunAlertPanel(@"Already signed in", @"Choose Disconnect first to sign in with a different token.", @"OK", nil, nil);
        return;
    }
    if (!loginController) loginController = [[LoginController alloc] initWithOwner:self];
    [loginController showPanelWithToken:prefs.token isBot:(prefs.is_bot ? YES : NO) remember:(prefs.remember ? YES : NO)
                                  proxy:prefs.proxy proxyUser:prefs.proxy_user proxyPassword:prefs.proxy_pass];
}

- (void)loginController:(LoginController *)lc didSubmitToken:(char *)token isBot:(BOOL)isBot remember:(BOOL)remember
                  proxy:(NSString *)proxy proxyUser:(NSString *)proxyUser proxyPassword:(char *)proxyPass
{
    prefs.is_bot = isBot ? 1 : 0;
    prefs.remember = remember ? 1 : 0;
    memset(prefs.token, 0, sizeof(prefs.token));
    if (remember) strncpy(prefs.token, token, sizeof(prefs.token) - 1);
    memset(&prefs.proxy, 0, sizeof(prefs.proxy));
    memset(&prefs.proxy_user, 0, sizeof(prefs.proxy_user));
    memset(&prefs.proxy_pass, 0, sizeof(prefs.proxy_pass));
    strncpy(prefs.proxy, UI_CPATH(proxy), sizeof(prefs.proxy) - 1);
    strncpy(prefs.proxy_user, UI_CPATH(proxyUser), sizeof(prefs.proxy_user) - 1);
    if (proxyPass) {
        strncpy(prefs.proxy_pass, proxyPass, sizeof(prefs.proxy_pass) - 1);       /* the live session uses it; the file keeps it only with "remember" */
        memset(proxyPass, 0, strlen(proxyPass));
        free(proxyPass);
    }
    [self savePrefs];
    if (!isBot) {
        int ans = NSRunAlertPanel(@"User account tokens",
            @"Signing in with a user account token is against Discord's Terms of Service and can get the account banned. Continue?",
            @"Cancel", @"Continue", nil);
        if (ans != NSAlertAlternateReturn) {
            memset(token, 0, strlen(token));
            free(token);
            return;
        }
    }
    if (!ssh_rng_ready()) {
        if (pendingToken) { memset(pendingToken, 0, strlen(pendingToken)); free(pendingToken); }
        pendingToken = token;
        [self showEntropyPanel];
        return;
    }
    [self beginWithToken:token];
}

- (void)showEntropyPanel
{
    NSTextField *label;
    if (entropyPanel) { [entropyPanel makeKeyAndOrderFront:nil]; return; }
    entropyPanel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 360, 130)
                                              styleMask:NSTitledWindowMask
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [entropyPanel setTitle:@"Seeding random number generator"];
    [entropyPanel setHidesOnDeactivate:NO];
    [entropyPanel setAcceptsMouseMovedEvents:YES];
    label = ui_wrapping_label(@"The secure connection to Discord needs real randomness, and this computer has no built-in source of it. Move the mouse around inside this window until the bar is full.",
                              NSMakeRect(16, 66, 328, 54), NO);
    entropyMeter = [[EntropyMeter alloc] initWithFrame:NSMakeRect(16, 24, 328, 26)];
    [entropyMeter setTarget:self];
    [[entropyPanel contentView] addSubview:label];
    [[entropyPanel contentView] addSubview:entropyMeter];
    [entropyPanel center];
    [entropyPanel makeKeyAndOrderFront:nil];
    [entropyPanel makeFirstResponder:entropyMeter];
}

- (void)entropyReady
{
    char *token = pendingToken;

    if (!entropyPanel) return;
    ssh_rng_save_seed([rngSeedPath cString]);           /* remember it: the next launch needs no wiggling */
    [entropyPanel orderOut:nil];
    [entropyMeter release]; entropyMeter = nil;
    [entropyPanel release]; entropyPanel = nil;
    pendingToken = NULL;
    if (token) [self beginWithToken:token];
}

/* ---------------------------------------------------------------- the session */

- (void)beginWithToken:(char *)token
{
    kc = (KClient *)malloc(sizeof(KClient));
    if (!kc) {
        memset(token, 0, strlen(token));
        free(token);
        NSRunAlertPanel(@"Out of memory", @"Kallisti could not start a session.", @"OK", nil, nil);
        return;
    }
    kc_init(kc, token, prefs.is_bot);
    memset(token, 0, strlen(token));                    /* the engine holds its own copy now */
    free(token);
    kc->trust = &trust;
    kc->check_dates = prefs.check_dates;
    strncpy(kc->proxy_spec, prefs.proxy, sizeof(kc->proxy_spec) - 1);
    strncpy(kc->proxy_user, prefs.proxy_user, sizeof(kc->proxy_user) - 1);
    strncpy(kc->proxy_pass, prefs.proxy_pass, sizeof(kc->proxy_pass) - 1);
    if (restHost) {
        strncpy(kc->rest_host, [restHost cString], sizeof(kc->rest_host) - 1);
        kc->rest_port = restPort;
        kc->rest_tls = restTLS;
        kc->resize_any_host = resizeAnyHost;
        if (gatewayURL) strncpy(kc->gateway_url, [gatewayURL cString], sizeof(kc->gateway_url) - 1);
    }
    [mainController release];                           /* a new session has a new model */
    mainController = [[MainController alloc] initWithOwner:self model:kc_model(kc)];
    [mainController setImageSource:self showInline:inlineImages];
    [mainController show];
    [mainController setStatus:@"Starting..."];
    kc_start(kc, app_now_ms());
    timer = [[NSTimer scheduledTimerWithTimeInterval:0.02 target:self selector:@selector(tick:)
                                            userInfo:nil repeats:YES] retain];
}

- (void)tick:(NSTimer *)t
{
    if (!kc) return;
    {
        KMs now = app_now_ms();
        unsigned wi;

        kc_tick(kc, now);
        [self drainEngine];
        [mainController updateTypingAt:now];
        for (wi = 0; wi < [channelWindows count]; wi++) [[channelWindows objectAtIndex:wi] updateTypingAt:now];
    }
}

- (void)drainEngine
{
    KEvent ev;
    KImage img;
    ModelChange ch;
    BOOL fatal = NO, thumbsArrived = NO;
    NSString *fatalText = nil;
    int i;

    while (kc && kc_next_event(kc, &ev)) {
        if (ev.type == KE_STATE) {
            [mainController setStatus:S(ev.text)];
            if (ev.state == KC_FATAL) { fatal = YES; fatalText = S(ev.text); }
        } else if (ev.type == KE_ERROR) {
            [mainController setStatus:S(ev.text)];
            [mainController appendNotice:S(ev.text)];
            for (i = 0; i < (int)[channelWindows count]; i++) [[channelWindows objectAtIndex:i] appendNotice:S(ev.text)];
        } else if (ev.type == KE_CHANNEL) {
            [mainController selectChannelWithId:S(ev.text)];        /* a conversation the user asked for */
        }
    }
    while (kc && kc_next_image(kc, &img)) {
        NSString *url = S(img.url);
        NSString *name = [imageNames objectForKey:url];
        ImageWindow *w = nil;

        if (img.tag == 2) {                                        /* a file being saved */
            [self downloadArrived:&img];
            continue;
        }
        if (img.tag == 1) {                                        /* a thumbnail for the message log */
            NSImage *thumb = nil;

            if (img.ok) thumb = [[[NSImage alloc] initWithData:[NSData dataWithBytes:img.tiff length:img.tiff_len]] autorelease];
            [thumbs setObject:(thumb ? (id)thumb : (id)@"") forKey:url];                /* @"": it failed, do not ask again */
            [thumbsAsked removeObject:url];
            kc_image_free(&img);
            thumbsArrived = YES;
            continue;
        }
        if (img.ok) {
            if (img.anim.n > 1) {                                   /* an animation: every frame, and how long each is shown */
                NSMutableArray *tiffs = [NSMutableArray array], *delays = [NSMutableArray array];
                int fi;

                for (fi = 0; fi < img.anim.n; fi++) {
                    [tiffs addObject:[NSData dataWithBytes:img.anim.tiff[fi] length:img.anim.len[fi]]];
                    [delays addObject:[NSNumber numberWithUnsignedInt:img.anim.delay_ms[fi]]];
                }
                w = [[[ImageWindow alloc] initWithOwner:self title:(name ? name : @"Picture") frames:tiffs delays:delays] autorelease];
            } else {
                NSData *data = [NSData dataWithBytes:img.tiff length:img.tiff_len];

                w = [[[ImageWindow alloc] initWithOwner:self title:(name ? name : @"Picture") tiffData:data] autorelease];
            }
            if (w) {
                [imageWindows addObject:w];
                [w show];
                [mainController setStatus:[NSString stringWithFormat:@"Picture loaded: %u KB, connect %u ms, download %u ms, decode %u ms",
                                           (unsigned)(img.bytes / 1024UL), (unsigned)img.ms_connect, (unsigned)img.ms_download,
                                           (unsigned)img.ms_decode]];
                SSTrace("picture %s: %u bytes, connect %u ms, download %u ms, decode %u ms", SSCS(name), (unsigned)img.bytes,
                        (unsigned)img.ms_connect, (unsigned)img.ms_download, (unsigned)img.ms_decode);
            } else {
                [mainController appendNotice:@"The picture arrived but AppKit could not read it."];
            }
        } else {
            [mainController setStatus:S(img.error)];
            [mainController appendNotice:S(img.error)];
        }
        kc_image_free(&img);
        [imageNames removeObjectForKey:url];
    }
    if (thumbsArrived) [self reloadAllMessages];
    while (kc && kc_next_change(kc, &ch)) {
        [mainController modelChanged:&ch];
        for (i = 0; i < (int)[channelWindows count]; i++) [[channelWindows objectAtIndex:i] modelChanged:&ch];
    }
    if (fatal) {
        [self endSession];
        [mainController setStatus:fatalText];
        NSRunAlertPanel(@"Cannot connect", @"%@", @"OK", nil, nil, fatalText);
        [self showLogin:nil];
    }
}

- (void)closeChannelWindows
{
    int i;

    for (i = 0; i < (int)[channelWindows count]; i++) [[channelWindows objectAtIndex:i] closeWindow];
    [channelWindows removeAllObjects];
}

- (void)endSession
{
    [timer invalidate]; [timer release]; timer = nil;
    [self closeChannelWindows];
    [self closeImageWindows];
    [imageList closePanel];
    [reactController closePanel];
    if (kc) {
        kc_stop(kc);
        kc_free(kc);
        free(kc);
        kc = NULL;
    }
}

- (void)disconnect:(id)sender
{
    if (!kc) return;
    [self endSession];
    [mainController setStatus:@"Disconnected"];
}

/* ---------------------------------------------------------------- MainController's owner protocol */

- (void)mainController:(MainController *)mc didSelectChannel:(NSString *)channelId
{
    if (kc) kc_open_channel(kc, [channelId cString]);
}

/* Sends a line to a channel; problems are reported to `target` (a window or the main controller) as notices. */
- (void)sendText:(NSString *)text toChannel:(NSString *)channelId notify:(id)target
{
    int rc;

    [lastTypingChannel release];                          /* Discord stops showing us as typing when the message lands */
    lastTypingChannel = nil;

    if (!kc || kc->state != KC_ONLINE) {
        [target appendNotice:@"Not connected; the message was not sent."];
        return;
    }
    {
        const char *typed = UI_CPATH(text);
        char *sent = NULL;

        if (emojiPack != NULL) {                                 /* :smile: becomes the emoji, as Discord's own client does */
            size_t cap = strlen(typed) * 4 + 16;

            sent = (char *)malloc(cap);
            if (sent != NULL) emoji_expand_shortcodes(emojiPack, typed, sent, cap);
        }
        rc = kc_send_message(kc, [channelId cString], sent != NULL ? sent : typed);
        free(sent);
    }
    if (rc == REST_E_TOO_LONG) {
        [target appendNotice:@"That message is longer than Discord's limit of 2000 characters."];
    } else if (rc == REST_E_FULL) {
        [target appendNotice:@"Too many messages are waiting to be sent; try again in a moment."];
    } else if (rc != 0) {
        [target appendNotice:@"That message cannot be sent."];
    }
}

/* Typing in a message box: Discord is told at most every 8 seconds per channel (it shows for about 10). */
- (void)userTypingInChannel:(NSString *)channelId
{
    KMs now = app_now_ms();

    if (!kc || kc->state != KC_ONLINE) return;
    if (lastTypingChannel != nil && [lastTypingChannel isEqualToString:channelId] && (now - lastTypingAt) < 8000U) return;
    if (kc_typing(kc, [channelId cString]) == 0) {
        [lastTypingChannel release];
        lastTypingChannel = [channelId retain];
        lastTypingAt = now;
    }
}

- (void)mainController:(MainController *)mc userTypingInChannel:(NSString *)channelId { [self userTypingInChannel:channelId]; }

- (BOOL)mainController:(MainController *)mc didDropFiles:(NSArray *)paths caption:(NSString *)caption forChannel:(NSString *)channelId
{
    return [self sendFiles:paths caption:caption toChannel:channelId notify:mc];
}

- (BOOL)channelWindow:(ChannelWindow *)cw didDropFiles:(NSArray *)paths caption:(NSString *)caption forChannel:(NSString *)channelId
{
    return [self sendFiles:paths caption:caption toChannel:channelId notify:cw];
}

/* Discord > Attach File...: pick files and send them to the channel shown in the main window, with what is typed there. */
- (void)attachFile:(id)sender
{
    NSOpenPanel *op = [NSOpenPanel openPanel];

    if (![mainController currentChannel]) {
        NSRunAlertPanel(@"No channel", @"Choose a channel first.", @"OK", nil, nil);
        return;
    }
    [op setAllowsMultipleSelection:YES];
    [op setCanChooseDirectories:NO];
    if ([op runModalForDirectory:nil file:nil types:nil] == NSOKButton) {
        [mainController attachFiles:[op filenames]];
    }
}

/* Sends files (at most REST_MAX_FILES, REST_MAX_UPLOAD bytes in all) as one message with `caption` as its text.
 * Returns whether it was handed to the engine; the reason for a no is a notice to `target`. */
- (BOOL)sendFiles:(NSArray *)paths caption:(NSString *)caption toChannel:(NSString *)channelId notify:(id)target
{
    RestFile files[REST_MAX_FILES];
    unsigned char *mem[REST_MAX_FILES];
    NSMutableArray *names = [NSMutableArray array];
    int n = 0, i, rc;
    long total = 0;
    BOOL ok = YES;
    const char *typed;
    char *sent = NULL;
    unsigned pi;

    if (!kc || kc->state != KC_ONLINE) {
        [target appendNotice:@"Not connected; the files were not sent."];
        return NO;
    }
    for (pi = 0; ok && pi < [paths count]; pi++) {
        NSString *path = [paths objectAtIndex:pi];
        struct stat st;
        FILE *f;
        unsigned char *buf;

        if (n >= REST_MAX_FILES) {
            [target appendNotice:[NSString stringWithFormat:@"Discord takes at most %d files in a message.", REST_MAX_FILES]];
            ok = NO;
        } else if (stat([path cString], &st) != 0) {
            [target appendNotice:[NSString stringWithFormat:@"Cannot find %@.", [path lastPathComponent]]];
            ok = NO;
        } else if (!S_ISREG(st.st_mode)) {
            [target appendNotice:[NSString stringWithFormat:@"%@ is a folder or special file; only plain files can be sent.", [path lastPathComponent]]];
            ok = NO;
        } else if ((long)st.st_size > REST_MAX_UPLOAD || total + (long)st.st_size > REST_MAX_UPLOAD) {
            [target appendNotice:[NSString stringWithFormat:@"%@ is too big: Discord takes %d MB of files in a message.",
                                  [path lastPathComponent], (int)(REST_MAX_UPLOAD / (1024L * 1024L))]];
            ok = NO;
        } else {
            buf = (unsigned char *)malloc((size_t)st.st_size + 1);
            f = buf != NULL ? fopen([path cString], "rb") : NULL;
            if (f == NULL || fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
                [target appendNotice:[NSString stringWithFormat:@"Cannot read %@.", [path lastPathComponent]]];
                free(buf);
                ok = NO;
            } else {
                NSData *nm = ui_utf8_cstring([path lastPathComponent]);

                [names addObject:nm];
                mem[n] = buf;
                files[n].name = (const char *)[nm bytes];
                files[n].data = buf;
                files[n].len = (size_t)st.st_size;
                total += (long)st.st_size;
                n++;
            }
            if (f != NULL) fclose(f);
        }
    }
    if (!ok || n == 0) {
        for (i = 0; i < n; i++) free(mem[i]);
        return NO;
    }
    typed = UI_CPATH(ui_trim(caption));
    if (emojiPack != NULL) {
        size_t cap = strlen(typed) * 4 + 16;

        sent = (char *)malloc(cap);
        if (sent != NULL) emoji_expand_shortcodes(emojiPack, typed, sent, cap);
    }
    rc = kc_send_files(kc, [channelId cString], sent != NULL ? sent : typed, files, n);
    free(sent);
    for (i = 0; i < n; i++) free(mem[i]);
    [lastTypingChannel release];
    lastTypingChannel = nil;
    if (rc == REST_E_TOO_LONG) {
        [target appendNotice:@"The files or the text are over Discord's limits."];
    } else if (rc == REST_E_FULL) {
        [target appendNotice:@"Too many messages are waiting to be sent; try again in a moment."];
    } else if (rc != 0) {
        [target appendNotice:@"Those files cannot be sent."];
    }
    if (rc == 0) {
        [target appendNotice:[NSString stringWithFormat:@"Sending %d file%s (%d KB)...", n, n == 1 ? "" : "s", (int)((total + 1023L) / 1024L)]];
    }
    return rc == 0;
}

- (void)channelWindow:(ChannelWindow *)cw userTypingInChannel:(NSString *)channelId { [self userTypingInChannel:channelId]; }

- (void)mainController:(MainController *)mc didSubmitText:(NSString *)text forChannel:(NSString *)channelId
{
    [self sendText:text toChannel:channelId notify:mc];
}

- (void)channelWindow:(ChannelWindow *)cw didSubmitText:(NSString *)text forChannel:(NSString *)channelId
{
    [self sendText:text toChannel:channelId notify:cw];
}

- (ChannelWindow *)windowForChannel:(NSString *)channelId
{
    int i;

    for (i = 0; i < (int)[channelWindows count]; i++) {
        ChannelWindow *w = [channelWindows objectAtIndex:i];
        if ([[w channelId] isEqualToString:channelId]) return w;
    }
    return nil;
}

- (void)openWindowForChannel:(NSString *)channelId
{
    ChannelWindow *w;

    if (!kc || channelId == nil) return;
    w = [self windowForChannel:channelId];
    if (w == nil) {
        w = [[[ChannelWindow alloc] initWithOwner:self model:kc_model(kc) channel:channelId] autorelease];
        [channelWindows addObject:w];
        [w setImageSource:self showInline:inlineImages];
        [w reloadMessages];
        kc_open_channel(kc, [channelId cString]);
    }
    [w show];
}

- (void)mainController:(MainController *)mc didRequestWindowForChannel:(NSString *)channelId
{
    [self openWindowForChannel:channelId];
}

- (void)openCurrentInWindow:(id)sender
{
    NSString *cid = [mainController currentChannel];

    if (cid) [self openWindowForChannel:cid];
    else NSBeep();
}

/* ---------------------------------------------------------------- pictures */

- (void)reloadAllMessages
{
    int i;

    [mainController reloadMessages];
    for (i = 0; i < (int)[channelWindows count]; i++) [[channelWindows objectAtIndex:i] reloadMessages];
}

- (void)applyInlineSetting
{
    int i;

    [mainController setImageSource:self showInline:inlineImages];
    for (i = 0; i < (int)[channelWindows count]; i++) [[channelWindows objectAtIndex:i] setImageSource:self showInline:inlineImages];
    [self reloadAllMessages];
}

- (BOOL)inlineImagesEnabled { return inlineImages; }

/* ---------------------------------------------------------------- Unicode emoji */

/* emoji.pack lives next to the program: in the bundle (wherever this system keeps resources) or beside the executable. */
- (NSString *)findEmojiPack
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *bundlePath = [[NSBundle mainBundle] bundlePath], *res = [[NSBundle mainBundle] pathForResource:@"emoji" ofType:@"pack"];
    NSString *cands[5];
    int i;

    if (res) return res;
    cands[0] = [bundlePath stringByAppendingPathComponent:@"emoji.pack"];
    cands[1] = [[bundlePath stringByAppendingPathComponent:@"Resources"] stringByAppendingPathComponent:@"emoji.pack"];
    cands[2] = [[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:@"emoji.pack"];
    cands[3] = [[fm currentDirectoryPath] stringByAppendingPathComponent:@"emoji.pack"];
    cands[4] = @"app/emoji.pack";
    for (i = 0; i < 5; i++) {
        if (cands[i] && [fm fileExistsAtPath:cands[i]]) return cands[i];
    }
    return nil;
}

- (BOOL)loadEmojiPackFromPath:(NSString *)path
{
    EmojiPack *p = emoji_pack_open([path cString]);

    if (p == NULL) return NO;
    ui_set_emoji_pack(NULL);
    emoji_pack_close(emojiPack);
    emojiPack = p;
    ui_set_emoji_pack(emojiPack);                        /* emoji in names and titles become :shortcodes: */
    [emojiImages removeAllObjects];
    return YES;
}

- (BOOL)emojiPicturesEnabled { return prefs.emoji_pictures && emojiPack != NULL; }

- (void)setEmojiPictures:(BOOL)on
{
    prefs.emoji_pictures = on ? 1 : 0;
    [emojiItem setTitle:on ? @"Show Emoji as Text" : @"Show Emoji as Pictures"];
    if (mainController) [self reloadAllMessages];
}

- (void)toggleEmoji:(id)sender
{
    if (emojiPack == NULL) {
        NSRunAlertPanel(@"No emoji pictures", @"The emoji.pack file is missing from the application, so emoji cannot be drawn as pictures.", @"OK", nil, nil);
        return;
    }
    [self setEmojiPictures:!prefs.emoji_pictures];
    [self savePrefs];
}

/* ChannelPane's questions: the pack to scan with (NULL when off), and a picture per emoji key, made once. */
- (EmojiPack *)emojiPack
{
    return prefs.emoji_pictures ? emojiPack : NULL;
}

- (NSImage *)unicodeEmojiImageForKey:(NSString *)key
{
    id v = [emojiImages objectForKey:key];
    size_t n = 0;
    unsigned char *tiff;

    if (v != nil) return [v isKindOfClass:[NSImage class]] ? (NSImage *)v : nil;
    tiff = emojiPack ? emoji_pack_tiff(emojiPack, [key cString], &n) : NULL;
    if (tiff != NULL) {
        NSImage *img = [[[NSImage alloc] initWithData:[NSData dataWithBytes:tiff length:n]] autorelease];

        free(tiff);
        [emojiImages setObject:(img ? (id)img : (id)@"") forKey:key];
        return img;
    }
    [emojiImages setObject:@"" forKey:key];                                     /* no picture: do not try again */
    return nil;
}

- (void)setEmojiBase:(NSString *)base
{
    [emojiBase release];
    emojiBase = [base retain];
}

/* The picture of a custom emoji if it is on hand; otherwise nil and a download of its 32-pixel PNG (shown at 16) starts. */
- (NSImage *)inlineEmojiImageForID:(NSString *)emojiId
{
    NSString *url = [NSString stringWithFormat:@"%@%@.png?size=32", emojiBase ? emojiBase : @"https://cdn.discordapp.com/emojis/", emojiId];
    id v = [thumbs objectForKey:url];

    if (v != nil) return [v isKindOfClass:[NSImage class]] ? (NSImage *)v : nil;
    if (kc && ![thumbsAsked containsObject:url] &&
        kc_fetch_image_ex(kc, [url cString], 0, 0, 16, 16, 1, KC_FETCH_RAW) == 0) {
        [thumbsAsked addObject:url];
    }
    return nil;
}

- (void)setInlineImages:(BOOL)on
{
    inlineImages = on;
    [inlineItem setTitle:on ? @"Show Images as Links Only" : @"Show Images Inline (Experimental)"];
    if (mainController) [self applyInlineSetting];
}

- (void)toggleInline:(id)sender
{
    [self setInlineImages:!inlineImages];
}

/* The thumbnail for an attachment address if it is on hand; otherwise nil and a download is started. */
- (NSImage *)inlineImageForURL:(NSString *)url width:(int)w height:(int)h jpeg:(BOOL)isJpeg
{
    id v = [thumbs objectForKey:url];

    if (v != nil) return [v isKindOfClass:[NSImage class]] ? (NSImage *)v : nil;
    if (kc && ![thumbsAsked containsObject:url] &&
        kc_fetch_image_ex(kc, [url cString], (unsigned long)w, (unsigned long)h, 320, 240, 1, isJpeg ? KC_FETCH_JPEG : 0) == 0) {
        [thumbsAsked addObject:url];
    }
    return nil;
}

- (void)closeImageWindows
{
    int i;

    for (i = 0; i < (int)[imageWindows count]; i++) [[imageWindows objectAtIndex:i] closeWindow];
    [imageWindows removeAllObjects];
    [imageNames removeAllObjects];
    [thumbs removeAllObjects];
    [thumbsAsked removeAllObjects];
}

- (NSArray *)imageWindows { return imageWindows; }

- (NSArray *)attachmentItemsForCurrentChannel
{
    NSMutableArray *list = [NSMutableArray array];
    NSString *cid = [mainController currentChannel];
    Channel *c = (kc && cid) ? model_find_channel(kc_model(kc), [cid cString]) : NULL;
    int i, j;

    for (i = 0; c && i < c->nmsgs; i++) {
        Message *m = &c->msgs[i];
        char hm[8], day[16];

        ktime_local(m->time, hm, day);
        for (j = 0; j < m->natt; j++) {
            Attachment *a = &m->att[j];
            NSString *label;

            label = [NSString stringWithFormat:@"[%s] %@: %@%@", hm, S(m->author), S(a->name),
                     (a->width > 0 && a->height > 0) ? [NSString stringWithFormat:@"  (%dx%d)", a->width, a->height] :
                     (a->is_image ? @"" : @"  (file)")];
            [list addObject:[[[ImageItem alloc] initWithLabel:label url:S(a->url) saveURL:S(a->dl_url) name:S(a->name)
                                                        width:(unsigned long)a->width height:(unsigned long)a->height
                                                         jpeg:(a->is_jpeg ? YES : NO) gif:(a->is_gif ? YES : NO)
                                                        image:(a->is_image ? YES : NO)] autorelease]];
        }
    }
    return list;
}

- (NSArray *)imageItemsForCurrentChannel
{
    NSMutableArray *list = [NSMutableArray array];
    NSArray *all = [self attachmentItemsForCurrentChannel];
    unsigned i;

    for (i = 0; i < [all count]; i++) {
        if (((ImageItem *)[all objectAtIndex:i])->isImage) [list addObject:[all objectAtIndex:i]];
    }
    return list;
}

- (void)showImages:(id)sender
{
    NSArray *items = [self attachmentItemsForCurrentChannel];

    if (![mainController currentChannel]) {
        NSRunAlertPanel(@"No channel", @"Choose a channel first.", @"OK", nil, nil);
        return;
    }
    if ([items count] == 0) {
        NSRunAlertPanel(@"No attachments", @"None of the messages shown in this channel has a file attached.", @"OK", nil, nil);
        return;
    }
    if (!imageList) imageList = [[ImageListController alloc] initWithOwner:self];
    [imageList showItems:items title:@"Attachments in This Channel"];
}

- (void)openImageItem:(ImageItem *)item
{
    /* a GIF is fetched as the GIF (smaller box: every frame is kept in memory) so an animation can play */
    if (!kc || kc_fetch_image_ex(kc, [item->url cString], item->width, item->height,
                                 item->gif ? 400UL : KC_IMAGE_MAX_W, item->gif ? 300UL : KC_IMAGE_MAX_H, 0,
                                 item->gif ? KC_FETCH_ANIM : (item->jpeg ? KC_FETCH_JPEG : 0)) != 0) {
        [mainController appendNotice:@"Too many pictures are loading; try again in a moment."];
        return;
    }
    [imageNames setObject:item->name forKey:item->url];
    [mainController setStatus:[NSString stringWithFormat:@"Loading %@...", item->name]];
}

- (void)imageList:(ImageListController *)c didChooseItem:(ImageItem *)item
{
    [self openImageItem:item];
}

- (void)imageList:(ImageListController *)c didSaveItem:(ImageItem *)item
{
    NSSavePanel *sp = [NSSavePanel savePanel];

    if ([sp runModalForDirectory:nil file:item->name] == NSOKButton) {
        [self saveItem:item toPath:[sp filename]];
    }
}

- (void)saveItem:(ImageItem *)item toPath:(NSString *)path
{
    if (!kc || kc_fetch_file(kc, [item->saveURL cString], 2) != 0) {
        [mainController appendNotice:@"Too many downloads are going; try again in a moment."];
        return;
    }
    [downloads setObject:path forKey:item->saveURL];
    [mainController setStatus:[NSString stringWithFormat:@"Downloading %@...", item->name]];
}

/* The file for a download that arrived (tag 2) is written as it came; nothing is decoded. */
- (void)downloadArrived:(KImage *)img
{
    NSString *url = S(img->url);
    NSString *path = [downloads objectForKey:url];

    if (path != nil) {
        if (img->ok) {
            FILE *f = fopen([path cString], "wb");
            BOOL good = f != NULL && (img->tiff_len == 0 || fwrite(img->tiff, 1, img->tiff_len, f) == img->tiff_len);

            if (f != NULL && fclose(f) != 0) good = NO;
            if (good) {
                [mainController setStatus:[NSString stringWithFormat:@"Saved %@ (%u KB)", [path lastPathComponent],
                                           (unsigned)((img->tiff_len + 1023UL) / 1024UL)]];
            } else {
                if (f != NULL) remove([path cString]);
                [mainController appendNotice:[NSString stringWithFormat:@"Could not write %@.", path]];
            }
        } else {
            [mainController appendNotice:[NSString stringWithFormat:@"Download failed: %@", S(img->error)]];
        }
        [downloads removeObjectForKey:url];
    }
    kc_image_free(img);
}

- (void)showReact:(id)sender
{
    NSMutableArray *ids = [NSMutableArray array], *labels = [NSMutableArray array], *infos = [NSMutableArray array];
    NSString *cid = [mainController currentChannel];
    Channel *c = (kc && cid) ? model_find_channel(kc_model(kc), [cid cString]) : NULL;
    int i, first;

    if (c == NULL) {
        NSRunAlertPanel(@"No channel", @"Choose a channel first.", @"OK", nil, nil);
        return;
    }
    if (c->nmsgs == 0) {
        NSRunAlertPanel(@"No messages", @"There is nothing here to react to yet.", @"OK", nil, nil);
        return;
    }
    first = c->nmsgs > 40 ? c->nmsgs - 40 : 0;
    for (i = first; i < c->nmsgs; i++) {
        Message *m = &c->msgs[i];
        char hm[8], day[16];
        char line[100];
        NSString *rs = @"";
        int k;

        ktime_local(m->time, hm, day);
        strncpy(line, m->text, sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        {
            char *nl = strchr(line, '\n');
            if (nl != NULL) *nl = '\0';
        }
        for (k = 0; k < m->nreact; k++) {
            const Reaction *r = &m->react[k];
            char one[160];

            if (r->id[0] != '\0') sprintf(one, ":%.60s:", r->name); else strncpy(one, r->name, 100);
            one[sizeof(one) - 1] = '\0';
            rs = [rs stringByAppendingFormat:@"%@%@ %d%s", k > 0 ? @"   " : @"", S(one), r->count, r->me ? "*" : ""];
        }
        [ids addObject:S(m->id)];
        [labels addObject:[NSString stringWithFormat:@"[%s] %@: %@", hm, S(m->author), S(line)]];
        [infos addObject:rs];
    }
    if (!reactController) reactController = [[ReactController alloc] initWithOwner:self];
    [reactController showMessageIds:ids labels:labels reactions:infos];
}

- (BOOL)reactController:(ReactController *)rc reactWith:(NSString *)emojiText toMessage:(NSString *)messageId remove:(BOOL)remove
{
    return [self reactToMessage:messageId withText:emojiText remove:remove];
}

- (BOOL)reactToMessage:(NSString *)messageId withText:(NSString *)text remove:(BOOL)remove
{
    NSString *cid = [mainController currentChannel];
    Channel *c = (kc && cid) ? model_find_channel(kc_model(kc), [cid cString]) : NULL;
    char out[96];

    if (c == NULL) return NO;
    if (model_resolve_reaction(c, emojiPack, UI_CPATH(text), out, sizeof(out)) != 0) {
        [mainController appendNotice:@"That is not an emoji. Type :name: (like :thumbsup:), a custom emoji's :name:, or paste the emoji."];
        return NO;
    }
    if (kc_react(kc, c->id, [messageId cString], out, remove ? 1 : 0) != 0) {
        [mainController appendNotice:@"The reaction could not be sent."];
        return NO;
    }
    return YES;
}

- (void)imageWindowClosed:(ImageWindow *)iw
{
    [[iw retain] autorelease];
    [imageWindows removeObject:iw];
}

- (void)channelWindowClosed:(ChannelWindow *)cw
{
    [[cw retain] autorelease];
    [channelWindows removeObject:cw];
}

- (void)showNewDM:(id)sender
{
    if (!kc || kc->state != KC_ONLINE) {
        NSRunAlertPanel(@"Not connected", @"Sign in first.", @"OK", nil, nil);
        return;
    }
    if (!dmController) dmController = [[DMController alloc] initWithOwner:self];
    [dmController showPanel];
}

- (void)dmController:(DMController *)dc didSubmitUserId:(NSString *)userId
{
    if (kc && kc_open_dm(kc, [userId cString]) != 0) {
        [mainController appendNotice:@"That is not a valid user ID."];
    }
}

- (void)mainControllerWindowClosed:(MainController *)mc
{
    [self endSession];
    [NSApp terminate:nil];
}

- (BOOL)applicationShouldTerminate:(id)sender
{
    [self endSession];
    return YES;
}

@end
