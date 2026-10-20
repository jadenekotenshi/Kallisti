#import "ChannelPane.h"
#import "UIHelpers.h"
#include <string.h>
#include "ktime.h"

#define INPUT_HEIGHT 24.0

static NSString *S(const char *s) { return ui_string_from_utf8(s); }

/* A fixed palette for author names, picked by a cheap hash so a name keeps its colour. */
static NSColor *author_color(const char *name)
{
    unsigned hash = 0;
    const char *p;

    for (p = name; *p; p++) hash = hash * 31 + (unsigned char)*p;
    switch (hash % 5) {
    case 0:  return [NSColor blueColor];
    case 1:  return [NSColor redColor];
    case 2:  return [NSColor purpleColor];
    case 3:  return [NSColor brownColor];
    default: return [NSColor magentaColor];
    }
}

/* gcc 2.7.2 does not look ahead within an @implementation. */
@interface ChannelPane (Private)
- (void)appendString:(NSString *)s color:(NSColor *)color font:(NSFont *)font;
- (void)appendMessage:(const Message *)m;
- (void)inputSubmitted:(id)sender;
@end

@implementation ChannelPane

- (id)initWithOwner:(id)anOwner model:(Model *)aModel frame:(NSRect)frame
{
    self = [super init];
    if (!self) return nil;
    owner = anOwner;
    model = aModel;
    normalFont = [[NSFont systemFontOfSize:12.0] retain];
    boldFont = [[NSFont boldSystemFontOfSize:12.0] retain];

    view = [[NSView alloc] initWithFrame:frame];
    [view setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

    messageScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, INPUT_HEIGHT, frame.size.width,
                                                                   frame.size.height - INPUT_HEIGHT)];
    [messageScroll setHasVerticalScroller:YES];
    [messageScroll setBorderType:NSBezelBorder];
    [messageScroll setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    messageText = [[NSTextView alloc] initWithFrame:[[messageScroll contentView] frame]];
    [messageText setEditable:NO];
    [messageText setSelectable:YES];
    [messageText setRichText:YES];
    [messageText setFont:normalFont];
    [messageText setVerticallyResizable:YES];
    [messageText setHorizontallyResizable:NO];
    [messageText setAutoresizingMask:NSViewWidthSizable];
    [messageText setMinSize:NSMakeSize(0, 0)];
    [messageText setMaxSize:NSMakeSize(1.0e7, 1.0e7)];
    [[messageText textContainer] setWidthTracksTextView:YES];
    [messageScroll setDocumentView:messageText];
    [view addSubview:messageScroll];

    inputField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, INPUT_HEIGHT)];
    [inputField setAutoresizingMask:(NSViewWidthSizable | NSViewMaxYMargin)];
    [inputField setTarget:self];
    [inputField setAction:@selector(inputSubmitted:)];
    [view addSubview:inputField];
    return self;
}

- (void)dealloc
{
    [view release]; [messageScroll release]; [messageText release]; [inputField release];
    [channel release]; [normalFont release]; [boldFont release];
    [super dealloc];
}

- (NSView *)view { return view; }
- (NSString *)channel { return channel; }
- (NSString *)messageString { return [messageText string]; }
- (void)focusInput { [[view window] makeFirstResponder:inputField]; }

- (void)setImageSource:(id)source showInline:(BOOL)showInline
{
    imageSource = source;
    inlineImages = showInline;
}

/* ---------------------------------------------------------------- drawing */

- (void)appendString:(NSString *)s color:(NSColor *)color font:(NSFont *)font
{
    unsigned len = [[messageText textStorage] length];
    NSRange r = NSMakeRange(len, [s length]);

    [messageText replaceCharactersInRange:NSMakeRange(len, 0) withString:s];
    [messageText setTextColor:color range:r];
    [messageText setFont:font range:r];
}

- (void)appendMessage:(const Message *)m
{
    char hm[8], day[16];

    ktime_local(m->time, hm, day);
    if (day[0] != '\0' && strcmp(day, lastDay) != 0) {
        [self appendString:[NSString stringWithFormat:@"---- %s ----\n", day] color:[NSColor darkGrayColor] font:normalFont];
        strcpy(lastDay, day);
    }
    [self appendString:[NSString stringWithFormat:@"[%s] ", hm] color:[NSColor darkGrayColor] font:normalFont];
    [self appendString:S(m->author) color:author_color(m->author) font:boldFont];
    [self appendString:@": " color:[NSColor blackColor] font:normalFont];
    [self appendString:S(m->text) color:[NSColor blackColor] font:normalFont];
    if (m->edited) [self appendString:@" (edited)" color:[NSColor darkGrayColor] font:normalFont];
    [self appendString:@"\n" color:[NSColor blackColor] font:normalFont];
    if (m->embeds != NULL && m->embeds[0] != '\0') {          /* link previews and other embeds, one grey line each */
        NSArray *lines = [S(m->embeds) componentsSeparatedByString:@"\n"];
        unsigned li;

        for (li = 0; li < [lines count]; li++) {
            [self appendString:[NSString stringWithFormat:@"    %@\n", [lines objectAtIndex:li]] color:[NSColor darkGrayColor] font:normalFont];
        }
    }
    if (inlineImages && imageSource != nil) {
        int i;

        for (i = 0; i < m->natt; i++) {
            NSImage *img;

            if (!m->att[i].is_image) continue;
            img = [imageSource inlineImageForURL:S(m->att[i].url) width:m->att[i].width height:m->att[i].height
                                           jpeg:(m->att[i].is_jpeg ? YES : NO)];
            if (img == nil) continue;
            [[messageText textStorage] appendAttributedString:ui_image_attachment(img)];
            [self appendString:@"\n" color:[NSColor blackColor] font:normalFont];
        }
    }
}

- (void)reload
{
    Channel *c = channel ? model_find_channel(model, [channel cString]) : NULL;
    int i;

    [messageText setString:@""];
    lastDay[0] = '\0';
    if (c == NULL) return;
    for (i = 0; i < c->nmsgs; i++) [self appendMessage:&c->msgs[i]];
    [messageText scrollRangeToVisible:NSMakeRange([[messageText textStorage] length], 0)];
}

- (void)setChannel:(NSString *)channelId
{
    if (channel != channelId) {
        [channel release];
        channel = [channelId retain];
    }
    if (channel) model_mark_read(model, [channel cString]);
    [self reload];
}

- (void)appendNotice:(NSString *)text
{
    unsigned len = [[messageText textStorage] length];

    [self appendString:[NSString stringWithFormat:@"* %@\n", text] color:[NSColor darkGrayColor] font:normalFont];
    [messageText scrollRangeToVisible:NSMakeRange(len, 0)];
}

- (void)modelChanged:(const ModelChange *)change
{
    NSRect vis;
    BOOL atBottom;
    Channel *c;

    if (channel == nil) return;
    switch (change->type) {
    case MC_RESET:
    case MC_GUILDS:
    case MC_CHANNELS:
        if (model_find_channel(model, [channel cString]) == NULL) {
            [channel release];
            channel = nil;
            [messageText setString:@""];
        }
        break;
    case MC_MESSAGE_ADDED:
        if (strcmp(change->channel_id, [channel cString]) != 0) break;
        c = model_find_channel(model, change->channel_id);
        if (c != NULL && c->nmsgs > 0 && strcmp(c->msgs[c->nmsgs - 1].id, change->message_id) == 0) {
            vis = [messageScroll documentVisibleRect];
            atBottom = NSMaxY(vis) >= NSMaxY([messageText frame]) - 30.0;
            [self appendMessage:&c->msgs[c->nmsgs - 1]];
            if (atBottom) [messageText scrollRangeToVisible:NSMakeRange([[messageText textStorage] length], 0)];
        } else {
            [self reload];
        }
        model_mark_read(model, change->channel_id);
        break;
    case MC_MESSAGE_CHANGED:
    case MC_MESSAGE_REMOVED:
    case MC_HISTORY:
        if (strcmp(change->channel_id, [channel cString]) == 0) [self reload];
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- typing */

- (void)inputSubmitted:(id)sender
{
    NSString *text = ui_trim([inputField stringValue]);

    if ([text length] == 0) return;
    if (channel == nil) {
        [self appendNotice:@"Choose a channel first."];
        return;
    }
    [inputField setStringValue:@""];
    [owner channelPane:self didSubmitText:text forChannel:channel];
}

- (void)typeText:(NSString *)text
{
    [inputField setStringValue:text];
    [self inputSubmitted:inputField];
}

@end
