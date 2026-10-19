#import "LoginController.h"
#import "UIHelpers.h"
#include <string.h>
#include <stdlib.h>

/* gcc 2.7.2 does not look ahead within an @implementation. */
@interface LoginController (Private)
- (void)buildPanel;
- (void)submit:(id)sender;
- (void)cancel:(id)sender;
@end

@implementation LoginController

- (id)initWithOwner:(id)anOwner
{
    self = [super init];
    if (!self) return nil;
    owner = anOwner;
    return self;
}

- (void)dealloc
{
    [panel release];
    [tokenField release];
    [rememberSwitch release];
    [userSwitch release];
    [super dealloc];
}

- (void)buildPanel
{
    NSTextField *intro, *warning;

    panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 440, 292)
                                       styleMask:(NSTitledWindowMask | NSClosableWindowMask)
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
    [panel setReleasedWhenClosed:NO];
    [panel setTitle:@"Kallisti - Sign in to Discord"];
    [panel setHidesOnDeactivate:NO];

    intro = ui_wrapping_label(@"Paste your bot's token. Create the bot at discord.com/developers, switch on its Message Content intent, and invite it to your server.",
                              NSMakeRect(16, 224, 408, 58), NO);
    [[panel contentView] addSubview:intro];

    [[panel contentView] addSubview:ui_label(@"Token:", NSMakeRect(16, 194, 408, 18))];
    tokenField = [[SecretField alloc] initWithFrame:NSMakeRect(16, 166, 408, 24)];
    [tokenField setEnterTarget:self action:@selector(submit:)];
    [tokenField setCancelTarget:self action:@selector(cancel:)];
    [[panel contentView] addSubview:tokenField];

    rememberSwitch = [ui_switch(@"Remember the token on this computer", NSMakeRect(16, 136, 408, 20)) retain];
    [[panel contentView] addSubview:rememberSwitch];

    userSwitch = [ui_switch(@"This is a user account token, not a bot token", NSMakeRect(16, 108, 408, 20)) retain];
    [[panel contentView] addSubview:userSwitch];
    warning = ui_wrapping_label(@"Using a user token with a client Discord has not approved is against Discord's Terms of Service and can get the account banned. Support for it here is experimental.",
                                NSMakeRect(34, 52, 390, 52), NO);
    [[panel contentView] addSubview:warning];

    [[panel contentView] addSubview:ui_button(@"Cancel", NSMakeRect(250, 12, 80, 28), self, @selector(cancel:))];
    [[panel contentView] addSubview:ui_button(@"Connect", NSMakeRect(344, 12, 80, 28), self, @selector(submit:))];
    [panel center];
}

- (void)showPanelWithToken:(const char *)token isBot:(BOOL)isBot remember:(BOOL)remember
{
    if (!panel) [self buildPanel];
    [tokenField setSecret:token];
    [rememberSwitch setState:remember ? NSOnState : NSOffState];
    [userSwitch setState:isBot ? NSOffState : NSOnState];
    [panel makeKeyAndOrderFront:nil];
    [panel makeFirstResponder:tokenField];
}

- (void)closePanel
{
    [panel orderOut:nil];
    [tokenField wipe];
}

- (void)submit:(id)sender
{
    char *token = [tokenField takeSecret];
    BOOL isBot = [userSwitch state] != NSOnState;
    BOOL remember = [rememberSwitch state] == NSOnState;

    if (token == NULL || token[0] == '\0') {
        if (token) free(token);
        NSBeep();
        return;
    }
    [panel orderOut:nil];
    [owner loginController:self didSubmitToken:token isBot:isBot remember:remember];
}

- (void)cancel:(id)sender
{
    [tokenField wipe];
    [panel orderOut:nil];
}

@end
