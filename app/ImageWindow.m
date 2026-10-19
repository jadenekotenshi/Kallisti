#import "ImageWindow.h"
#import "UIHelpers.h"

@implementation ImageWindow

- (id)initWithOwner:(id)anOwner title:(NSString *)title tiffData:(NSData *)data
{
    static float offset = 0.0;
    NSSize size;
    NSRect scr = [[NSScreen mainScreen] frame], content;
    NSScrollView *scroll;
    NSImageView *iv;
    float maxw, maxh;

    self = [super init];
    if (!self) return nil;
    owner = anOwner;
    image = [[NSImage alloc] initWithData:data];
    size = image ? [image size] : NSMakeSize(0, 0);
    if (image == nil || size.width < 1.0 || size.height < 1.0) {
        [self release];
        return nil;
    }
    maxw = scr.size.width - 80.0;
    maxh = scr.size.height - 120.0;
    content = NSMakeRect(0, 0, size.width < maxw ? size.width + 4.0 : maxw, size.height < maxh ? size.height + 4.0 : maxh);

    window = [[NSWindow alloc] initWithContentRect:content
                                         styleMask:(NSTitledWindowMask | NSClosableWindowMask |
                                                    NSMiniaturizableWindowMask | NSResizableWindowMask)
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    [window setReleasedWhenClosed:NO];
    [window setDelegate:(id)self];
    [window setMinSize:NSMakeSize(80, 60)];
    [window setTitle:title];

    scroll = [[[NSScrollView alloc] initWithFrame:[[window contentView] frame]] autorelease];
    [scroll setHasVerticalScroller:YES];
    [scroll setHasHorizontalScroller:YES];
    [scroll setBorderType:NSNoBorder];
    [scroll setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    iv = [[[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)] autorelease];
    [iv setImage:image];
    [scroll setDocumentView:iv];
    [[window contentView] addSubview:scroll];
    [window setFrameTopLeftPoint:NSMakePoint(scr.origin.x + 80 + offset, NSMaxY(scr) - 60 - offset)];
    offset += 24.0;
    if (offset > 240.0) offset = 0.0;
    return self;
}

- (void)dealloc
{
    [window setDelegate:nil];
    [window release];
    [image release];
    [super dealloc];
}

- (NSSize)imageSize { return image ? [image size] : NSMakeSize(0, 0); }
- (void)show { [window makeKeyAndOrderFront:nil]; }

- (void)closeWindow
{
    [window setDelegate:nil];
    [window orderOut:nil];
}

- (void)windowWillClose:(NSNotification *)notification
{
    [window setDelegate:nil];
    [owner imageWindowClosed:self];
}

@end
