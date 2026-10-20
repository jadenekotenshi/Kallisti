#import "Compat.h"
#import "SecretField.h"

/* The sign-in panel: the Discord token (shown as bullets), whether to remember it, and whether it belongs to
 * a user account rather than a bot.  The owner learns the answer through
 * -loginController:didSubmitToken:isBot:remember:. */
@interface LoginController : NSObject
{
    id           owner;
    NSPanel     *panel;
    SecretField *tokenField;
    NSButton    *rememberSwitch, *userSwitch;
    NSTextField *proxyField, *proxyUserField;
    SecretField *proxyPassField;
}
- (id)initWithOwner:(id)anOwner;
- (void)showPanelWithToken:(const char *)token isBot:(BOOL)isBot remember:(BOOL)remember
                     proxy:(const char *)proxy proxyUser:(const char *)proxyUser proxyPassword:(const char *)proxyPass;
- (void)closePanel;
@end

@interface NSObject (LoginControllerOwner)
/* token and proxyPassword are malloc'd UTF-8 copies; the owner wipes and frees them. */
- (void)loginController:(LoginController *)lc didSubmitToken:(char *)token isBot:(BOOL)isBot remember:(BOOL)remember
                  proxy:(NSString *)proxy proxyUser:(NSString *)proxyUser proxyPassword:(char *)proxyPass;
@end
