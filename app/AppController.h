#import "Compat.h"
#import "LoginController.h"
#import "MainController.h"
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
    NSString          *restHost, *gatewayURL;   /* tests aim the client at a fake server; nil: Discord */
    int                restPort, restTLS;
}
- (void)buildMenu;
/* For tests: talk to a fake server instead of Discord. */
- (void)useRestHost:(NSString *)host port:(int)port tls:(BOOL)tls gatewayURL:(NSString *)url;
/* As if the token had been typed into the sign-in panel (needs a seeded random pool). */
- (void)signInWithToken:(NSString *)token isBot:(BOOL)isBot;
- (MainController *)mainController;
- (KClient *)client;
/* EntropyMeter's callback once the random pool is credited enough to use. */
- (void)entropyReady;
@end
