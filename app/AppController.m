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
- (void)closeImageWindows;
- (void)sendText:(NSString *)text toChannel:(NSString *)channelId notify:(id)target;
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
    return self;
}

- (void)dealloc
{
    [self endSession];
    trust_free(&trust);
    memset(&prefs, 0, sizeof(prefs));
    [loginController release];
    [mainController release];
    [channelWindows release];
    [dmController release];
    [imageList release]; [imageWindows release]; [imageNames release];
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
    [self addItem:@"Images in This Channel..." action:@selector(showImages:) key:@"i" target:self toMenu:m];
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
    [loginController showPanelWithToken:prefs.token isBot:(prefs.is_bot ? YES : NO) remember:(prefs.remember ? YES : NO)];
}

- (void)loginController:(LoginController *)lc didSubmitToken:(char *)token isBot:(BOOL)isBot remember:(BOOL)remember
{
    prefs.is_bot = isBot ? 1 : 0;
    prefs.remember = remember ? 1 : 0;
    memset(prefs.token, 0, sizeof(prefs.token));
    if (remember) strncpy(prefs.token, token, sizeof(prefs.token) - 1);
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
    if (restHost) {
        strncpy(kc->rest_host, [restHost cString], sizeof(kc->rest_host) - 1);
        kc->rest_port = restPort;
        kc->rest_tls = restTLS;
        if (gatewayURL) strncpy(kc->gateway_url, [gatewayURL cString], sizeof(kc->gateway_url) - 1);
    }
    [mainController release];                           /* a new session has a new model */
    mainController = [[MainController alloc] initWithOwner:self model:kc_model(kc)];
    [mainController show];
    [mainController setStatus:@"Starting..."];
    kc_start(kc, app_now_ms());
    timer = [[NSTimer scheduledTimerWithTimeInterval:0.02 target:self selector:@selector(tick:)
                                            userInfo:nil repeats:YES] retain];
}

- (void)tick:(NSTimer *)t
{
    if (!kc) return;
    kc_tick(kc, app_now_ms());
    [self drainEngine];
}

- (void)drainEngine
{
    KEvent ev;
    KImage img;
    ModelChange ch;
    BOOL fatal = NO;
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

        if (img.ok) {
            NSData *data = [NSData dataWithBytes:img.tiff length:img.tiff_len];

            w = [[[ImageWindow alloc] initWithOwner:self title:(name ? name : @"Picture") tiffData:data] autorelease];
            if (w) {
                [imageWindows addObject:w];
                [w show];
                [mainController setStatus:@"Picture loaded"];
            } else {
                [mainController appendNotice:@"The picture arrived but AppKit could not read it."];
            }
        } else {
            [mainController setStatus:S(img.error)];
            [mainController appendNotice:S(img.error)];
        }
        free(img.tiff);
        [imageNames removeObjectForKey:url];
    }
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

    if (!kc || kc->state != KC_ONLINE) {
        [target appendNotice:@"Not connected; the message was not sent."];
        return;
    }
    rc = kc_send_message(kc, [channelId cString], UI_CPATH(text));
    if (rc == REST_E_TOO_LONG) {
        [target appendNotice:@"That message is longer than Discord's limit of 2000 characters."];
    } else if (rc == REST_E_FULL) {
        [target appendNotice:@"Too many messages are waiting to be sent; try again in a moment."];
    } else if (rc != 0) {
        [target appendNotice:@"That message cannot be sent."];
    }
}

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

- (void)closeImageWindows
{
    int i;

    for (i = 0; i < (int)[imageWindows count]; i++) [[imageWindows objectAtIndex:i] closeWindow];
    [imageWindows removeAllObjects];
    [imageNames removeAllObjects];
}

- (NSArray *)imageWindows { return imageWindows; }

- (NSArray *)imageItemsForCurrentChannel
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

            if (!a->is_image) continue;
            label = [NSString stringWithFormat:@"[%s] %@: %@%@", hm, S(m->author), S(a->name),
                     (a->width > 0 && a->height > 0) ? [NSString stringWithFormat:@"  (%dx%d)", a->width, a->height] : @""];
            [list addObject:[[[ImageItem alloc] initWithLabel:label url:S(a->url) name:S(a->name)
                                                        width:(unsigned long)a->width height:(unsigned long)a->height] autorelease]];
        }
    }
    return list;
}

- (void)showImages:(id)sender
{
    NSArray *items = [self imageItemsForCurrentChannel];

    if (![mainController currentChannel]) {
        NSRunAlertPanel(@"No channel", @"Choose a channel first.", @"OK", nil, nil);
        return;
    }
    if ([items count] == 0) {
        NSRunAlertPanel(@"No pictures", @"None of the messages shown in this channel has a picture attached.", @"OK", nil, nil);
        return;
    }
    if (!imageList) imageList = [[ImageListController alloc] initWithOwner:self];
    [imageList showItems:items title:@"Images in This Channel"];
}

- (void)openImageItem:(ImageItem *)item
{
    if (!kc || kc_fetch_image(kc, [item->url cString], item->width, item->height) != 0) {
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
