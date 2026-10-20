#import "ImageWindow.h"
#import "UIHelpers.h"
#include <sys/time.h>

/* gcc 2.7.2 does not look ahead within an @implementation. */
@interface ImageWindow (Private)
- (BOOL)buildWindowWithTitle:(NSString *)title;
- (void)tick:(NSTimer *)t;
- (void)stopTimer;
- (double)now;
@end

@implementation ImageWindow

- (double)now
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

/* The window around `image`: a scroll view in case the picture is larger than the screen. */
- (BOOL)buildWindowWithTitle:(NSString *)title
{
    static float offset = 0.0;
    NSSize size = image ? [image size] : NSMakeSize(0, 0);
    NSRect scr = [[NSScreen mainScreen] frame], content;
    NSScrollView *scroll;
    float maxw, maxh;

    if (image == nil || size.width < 1.0 || size.height < 1.0) return NO;
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
    imageView = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    [imageView setImage:image];
    [scroll setDocumentView:imageView];
    [[window contentView] addSubview:scroll];
    [window setFrameTopLeftPoint:NSMakePoint(scr.origin.x + 80 + offset, NSMaxY(scr) - 60 - offset)];
    offset += 24.0;
    if (offset > 240.0) offset = 0.0;
    return YES;
}

- (id)initWithOwner:(id)anOwner title:(NSString *)title tiffData:(NSData *)data
{
    self = [super init];
    if (!self) return nil;
    owner = anOwner;
    image = [[NSImage alloc] initWithData:data];
    if (![self buildWindowWithTitle:title]) {
        [self release];
        return nil;
    }
    return self;
}

- (id)initWithOwner:(id)anOwner title:(NSString *)title frames:(NSArray *)tiffs delays:(NSArray *)delaysMs
{
    unsigned i;

    self = [super init];
    if (!self) return nil;
    owner = anOwner;
    frames = [[NSMutableArray alloc] init];
    delays = [[NSMutableArray alloc] init];
    for (i = 0; i < [tiffs count]; i++) {
        NSImage *f = [[[NSImage alloc] initWithData:[tiffs objectAtIndex:i]] autorelease];

        if (f == nil) {
            [self release];
            return nil;
        }
        [frames addObject:f];
        [delays addObject:[delaysMs objectAtIndex:i]];
    }
    if ([frames count] == 0) {
        [self release];
        return nil;
    }
    image = [[frames objectAtIndex:0] retain];
    if (![self buildWindowWithTitle:[NSString stringWithFormat:@"%@ (animated, %u frames)", title, (unsigned)[frames count]]]) {
        [self release];
        return nil;
    }
    if ([frames count] > 1) {
        nextChange = [self now] + [[delays objectAtIndex:0] doubleValue] / 1000.0;
        timer = [[NSTimer scheduledTimerWithTimeInterval:0.02 target:self selector:@selector(tick:) userInfo:nil repeats:YES] retain];
    }
    return self;
}

- (void)dealloc
{
    [self stopTimer];
    [window setDelegate:nil];
    [window release];
    [imageView release];
    [image release];
    [frames release];
    [delays release];
    [super dealloc];
}

- (void)stopTimer
{
    [timer invalidate];
    [timer release];
    timer = nil;
}

- (BOOL)isAnimated { return frames != nil && [frames count] > 1; }
- (int)currentFrame { return current; }
- (NSSize)imageSize { return image ? [image size] : NSMakeSize(0, 0); }

/* shows the next frame (wrapping round) and schedules the one after */
- (void)advance
{
    if (![self isAnimated]) return;
    current = (current + 1) % (int)[frames count];
    [imageView setImage:[frames objectAtIndex:current]];
    nextChange = [self now] + [[delays objectAtIndex:current] doubleValue] / 1000.0;
}

- (void)tick:(NSTimer *)t
{
    if ([self now] >= nextChange) {
        [self advance];
        [imageView setNeedsDisplay:YES];
    }
}

- (void)show { [window makeKeyAndOrderFront:nil]; }

- (void)closeWindow
{
    [self stopTimer];
    [window setDelegate:nil];
    [window orderOut:nil];
}

- (void)windowWillClose:(NSNotification *)notification
{
    [self stopTimer];
    [window setDelegate:nil];
    [owner imageWindowClosed:self];
}

@end
