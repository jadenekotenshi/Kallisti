#import "ChannelWindow.h"
#import "UIHelpers.h"
#include <string.h>

static NSString *S(const char *s) { return ui_string_from_utf8(s); }

/* gcc 2.7.2 does not look ahead within an @implementation. */
@interface ChannelWindow (Private)
- (void)updateTitle;
@end

@implementation ChannelWindow

- (id)initWithOwner:(id)anOwner model:(Model *)aModel channel:(NSString *)cid
{
    static float offset = 0.0;
    NSRect content = NSMakeRect(0, 0, 520, 400);
    NSRect scr = [[NSScreen mainScreen] frame];

    self = [super init];
    if (!self) return nil;
    owner = anOwner;
    model = aModel;
    channelId = [cid retain];

    window = [[NSWindow alloc] initWithContentRect:content
                                         styleMask:(NSTitledWindowMask | NSClosableWindowMask |
                                                    NSMiniaturizableWindowMask | NSResizableWindowMask)
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    [window setReleasedWhenClosed:NO];
    [window setDelegate:(id)self];
    [window setMinSize:NSMakeSize(260, 140)];
    pane = [[ChannelPane alloc] initWithOwner:self model:model frame:content];
    [[window contentView] addSubview:[pane view]];
    [pane setChannel:channelId];
    [self updateTitle];
    [window setFrameTopLeftPoint:NSMakePoint(scr.origin.x + 120 + offset, NSMaxY(scr) - 100 - offset)];
    offset += 24.0;
    if (offset > 240.0) offset = 0.0;
    return self;
}

- (void)dealloc
{
    [window setDelegate:nil];
    [window release]; [pane release]; [channelId release];
    [super dealloc];
}

- (NSString *)channelId { return channelId; }
- (NSString *)messageString { return [pane messageString]; }
- (void)typeText:(NSString *)text { [pane typeText:text]; }
- (void)updateTypingAt:(KMs)now { [pane updateTypingAt:now]; }
- (void)appendNotice:(NSString *)text { [pane appendNotice:text]; }
- (void)setImageSource:(id)source showInline:(BOOL)showInline { [pane setImageSource:source showInline:showInline]; }
- (void)reloadMessages { [pane reload]; }

- (void)show
{
    [window makeKeyAndOrderFront:nil];
    [pane focusInput];
}

- (void)closeWindow
{
    [window setDelegate:nil];
    [window orderOut:nil];
}

- (void)updateTitle
{
    Channel *c = model_find_channel(model, [channelId cString]);
    Guild *g = c ? model_find_guild(model, c->guild_id) : NULL;

    if (c && g) {
        [window setTitle:[NSString stringWithFormat:@"%@%@ (%@)", c->type == CH_DM ? @"" : @"#", S(c->name), S(g->name)]];
    } else {
        [window setTitle:@"Kallisti - channel gone"];
    }
}

- (void)modelChanged:(const ModelChange *)change
{
    if (change->type == MC_UNREAD) return;
    [pane modelChanged:change];
    if (change->type == MC_RESET || change->type == MC_GUILDS || change->type == MC_CHANNELS) [self updateTitle];
}

- (void)channelPane:(ChannelPane *)p didSubmitText:(NSString *)text forChannel:(NSString *)cid
{
    [owner channelWindow:self didSubmitText:text forChannel:cid];
}

- (void)windowWillClose:(NSNotification *)notification
{
    [window setDelegate:nil];
    [owner channelWindowClosed:self];
}

@end
