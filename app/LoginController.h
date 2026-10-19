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
}
- (id)initWithOwner:(id)anOwner;
- (void)showPanelWithToken:(const char *)token isBot:(BOOL)isBot remember:(BOOL)remember;
- (void)closePanel;
@end

@interface NSObject (LoginControllerOwner)
/* token is a malloc'd UTF-8 copy; the owner wipes and frees it. */
- (void)loginController:(LoginController *)lc didSubmitToken:(char *)token isBot:(BOOL)isBot remember:(BOOL)remember;
@end
