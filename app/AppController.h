#import "Compat.h"
#import "LoginController.h"
#import "MainController.h"
#import "ChannelWindow.h"
#import "DMController.h"
#import "ImageWindow.h"
#import "ImageListController.h"
#include "client.h"
#include "kprefs.h"

/* Owns the one Discord client engine (core/client.c), the timer that drives it, the sign-in panel and the main
 * window.  Reads the engine's events and the model's change queue every tick and hands them to the window. */
@interface AppController : NSObject
{
    KClient           *kc;                 /* malloc'd; NULL when not connected */
    NSTimer           *timer;
    TrustStore         trust;
    KPrefs             prefs;
    LoginController   *loginController;
    MainController    *mainController;
    NSString          *kallistiDir;        /* ~/.kallisti */
    NSString          *prefsPath;
    NSString          *rngSeedPath;
    NSPanel           *entropyPanel;
    id                 entropyMeter;
    char              *pendingToken;       /* held while the entropy panel is up */
    NSMenuItem        *datesItem;
    NSMutableArray    *channelWindows;     /* every open ChannelWindow */
    DMController      *dmController;
    ImageListController *imageList;
    NSMutableArray    *imageWindows;       /* every open ImageWindow */
    NSMutableDictionary *imageNames;       /* url -> file name, for the pictures being fetched */
    NSMutableDictionary *thumbs;           /* url -> NSImage (or an empty string if it failed): inline pictures */
    NSMutableSet        *thumbsAsked;
    BOOL                inlineImages;      /* the experimental inline-pictures switch */
    NSMenuItem          *inlineItem;
    NSString          *restHost, *gatewayURL;   /* tests aim the client at a fake server; nil: Discord */
    int                restPort, restTLS;
    int                resizeAnyHost;      /* tests: the fake server stands in for Discord's media proxy */
}
- (void)buildMenu;
/* For tests: talk to a fake server instead of Discord. */
- (void)useRestHost:(NSString *)host port:(int)port tls:(BOOL)tls gatewayURL:(NSString *)url;
/* As if the token had been typed into the sign-in panel (needs a seeded random pool). */
- (void)signInWithToken:(NSString *)token isBot:(BOOL)isBot;
- (MainController *)mainController;
- (void)openWindowForChannel:(NSString *)channelId;     /* what a double-click in the list does */
- (ChannelWindow *)windowForChannel:(NSString *)channelId;
/* The pictures attached to the messages of the channel shown in the main window (ImageItem objects), and what
 * choosing one does: ask the engine to fetch it, then show it in an ImageWindow. */
- (NSArray *)imageItemsForCurrentChannel;
- (void)openImageItem:(ImageItem *)item;
- (NSArray *)imageWindows;
- (void)setInlineImages:(BOOL)on;           /* the experimental switch (Discord menu), also for the tests */
- (BOOL)inlineImagesEnabled;
- (KClient *)client;
/* EntropyMeter's callback once the random pool is credited enough to use. */
- (void)entropyReady;
@end
