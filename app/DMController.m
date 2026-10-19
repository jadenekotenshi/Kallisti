#import "DMController.h"
#import "UIHelpers.h"

/* gcc 2.7.2 does not look ahead within an @implementation. */
@interface DMController (Private)
- (void)buildPanel;
- (void)submit:(id)sender;
- (void)cancel:(id)sender;
@end

@implementation DMController

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
    [idField release];
    [super dealloc];
}

- (void)buildPanel
{
    panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 400, 150)
                                       styleMask:(NSTitledWindowMask | NSClosableWindowMask)
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
    [panel setReleasedWhenClosed:NO];
    [panel setTitle:@"New Direct Message"];
    [panel setHidesOnDeactivate:NO];
    [[panel contentView] addSubview:ui_wrapping_label(@"The user's ID: turn on Developer Mode in Discord's settings, then right-click the person and choose Copy User ID. A bot can only message people it shares a server with.",
                                                      NSMakeRect(16, 74, 368, 66), NO)];
    idField = [ui_field(NSMakeRect(16, 46, 368, 24)) retain];
    [idField setTarget:self];
    [idField setAction:@selector(submit:)];
    [[panel contentView] addSubview:idField];
    [[panel contentView] addSubview:ui_button(@"Cancel", NSMakeRect(214, 10, 80, 28), self, @selector(cancel:))];
    [[panel contentView] addSubview:ui_button(@"Open", NSMakeRect(304, 10, 80, 28), self, @selector(submit:))];
    [panel center];
}

- (void)showPanel
{
    if (!panel) [self buildPanel];
    [idField setStringValue:@""];
    [panel makeKeyAndOrderFront:nil];
    [panel makeFirstResponder:idField];
}

- (void)submit:(id)sender
{
    NSString *s = ui_trim([idField stringValue]);
    unsigned i, n = [s length];
    BOOL ok = n >= 1 && n <= 20;

    for (i = 0; ok && i < n; i++) {
        unichar c = [s characterAtIndex:i];
        if (c < '0' || c > '9') ok = NO;
    }
    if (!ok) {
        NSBeep();
        return;
    }
    [panel orderOut:nil];
    [owner dmController:self didSubmitUserId:s];
}

- (void)cancel:(id)sender
{
    [panel orderOut:nil];
}

@end
