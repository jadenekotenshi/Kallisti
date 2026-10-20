#import "ChannelPane.h"
#import "UIHelpers.h"
#include <string.h>
#include <stdlib.h>
#include "ktime.h"

#define INPUT_HEIGHT 24.0
#define INPUT_MAX_HEIGHT 110.0
#define TYPING_HEIGHT 16.0

/* The pane's view: lays its children out by hand since the input's height varies. */
@interface PaneView : NSView
{
    ChannelPane *pane;
}
- (void)setPane:(ChannelPane *)p;
@end

@implementation PaneView
- (void)setPane:(ChannelPane *)p { pane = p; }
- (void)resizeSubviewsWithOldSize:(NSSize)oldSize
{
    [pane layoutSubviews];
}
@end

/* "copy" is the bit 1 in NSDragOperation (OPENSTEP 4.2 has no such type: see KDragOp in Compat.h). */
#define DROP_COPY 1U

/* The paths on a drag's pasteboard, or nil if it carries no files. */
static NSArray *dropped_files(id sender)
{
    NSPasteboard *pb = [sender draggingPasteboard];

    if ([pb availableTypeFromArray:[NSArray arrayWithObject:NSFilenamesPboardType]] == nil) return nil;
    return [pb propertyListForType:NSFilenamesPboardType];
}

/* A text view that takes files dropped on it (the message log and the input): the pane sends them. Anything else dropped
 * is the text view's own business. */
@interface DropTextView : NSTextView
{
    ChannelPane *pane;
}
- (void)setPane:(ChannelPane *)p;
@end

@implementation DropTextView
- (void)setPane:(ChannelPane *)p { pane = p; }
- (KDragOp)draggingEntered:(id <NSDraggingInfo>)sender
{
    return dropped_files(sender) != nil ? DROP_COPY : [super draggingEntered:sender];
}
- (KDragOp)draggingUpdated:(id <NSDraggingInfo>)sender
{
    return dropped_files(sender) != nil ? DROP_COPY : [super draggingUpdated:sender];
}
- (BOOL)prepareForDragOperation:(id <NSDraggingInfo>)sender
{
    return dropped_files(sender) != nil ? YES : [super prepareForDragOperation:sender];
}
- (BOOL)performDragOperation:(id <NSDraggingInfo>)sender
{
    NSArray *files = dropped_files(sender);

    if (files == nil) return [super performDragOperation:sender];
    return [pane acceptDroppedFiles:files];
}
@end

/* The text view typed into: Return asks the pane to send, Shift- or Alt-Return makes a new line. */
@interface InputView : DropTextView
@end

@implementation InputView
- (void)insertNewline:(id)sender
{
    unsigned flags = [[NSApp currentEvent] modifierFlags];

    [pane inputReturnPressed:(flags & (NSShiftKeyMask | NSAlternateKeyMask)) != 0];
}
@end

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
- (void)appendReactions:(const Message *)m;
- (void)appendRich:(const char *)text customs:(const Emoji *)customs count:(int)ncustoms prefix:(NSString *)prefix color:(NSColor *)color
              font:(NSFont *)font;
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

    view = [[PaneView alloc] initWithFrame:frame];
    [(PaneView *)view setPane:self];
    [view setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

    messageScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, INPUT_HEIGHT, frame.size.width,
                                                                   frame.size.height - INPUT_HEIGHT)];
    [messageScroll setHasVerticalScroller:YES];
    [messageScroll setBorderType:NSBezelBorder];
    messageText = [[DropTextView alloc] initWithFrame:[[messageScroll contentView] frame]];
    [(DropTextView *)messageText setPane:self];
    [messageText registerForDraggedTypes:[NSArray arrayWithObject:NSFilenamesPboardType]];
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

    inputScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, INPUT_HEIGHT)];
    [inputScroll setHasVerticalScroller:NO];
    [inputScroll setBorderType:NSBezelBorder];
    inputText = [[InputView alloc] initWithFrame:[[inputScroll contentView] frame]];
    [(InputView *)inputText setPane:self];
    [inputText registerForDraggedTypes:[NSArray arrayWithObjects:NSFilenamesPboardType, NSStringPboardType, nil]];
    [inputText setRichText:NO];
    [inputText setFont:normalFont];
    [inputText setVerticallyResizable:YES];
    [inputText setHorizontallyResizable:NO];
    [inputText setAutoresizingMask:NSViewWidthSizable];
    [inputText setMinSize:NSMakeSize(0, 0)];
    [inputText setMaxSize:NSMakeSize(1.0e7, 1.0e7)];
    [[inputText textContainer] setWidthTracksTextView:YES];
    [inputText setDelegate:(id)self];
    [inputScroll setDocumentView:inputText];
    [view addSubview:inputScroll];
    typingLabel = [ui_label(@"", NSMakeRect(0, 0, frame.size.width, TYPING_HEIGHT)) retain];
    [typingLabel setFont:[NSFont systemFontOfSize:10.0]];
    [typingLabel setTextColor:[NSColor darkGrayColor]];
    [view addSubview:typingLabel];
    [self layoutSubviews];
    return self;
}

- (void)dealloc
{
    [view release]; [messageScroll release]; [messageText release]; [inputScroll release]; [inputText release]; [typingLabel release];
    [channel release]; [normalFont release]; [boldFont release];
    [super dealloc];
}

- (NSView *)view { return view; }
- (NSString *)channel { return channel; }
- (NSString *)messageString { return [messageText string]; }
- (void)focusInput { [[view window] makeFirstResponder:inputText]; }

/* The height the input wants for its text: the text's own height plus the bezel, between one line and the cap. */
- (float)wantedInputHeight
{
    /* Counted by hand, line by line with a greedy word wrap, because asking the text view itself is unreliable: inside
     * the scroll view its height never goes below the clip view's, so the input would grow on its own. */
    NSString *s = [inputText string];
    float avail = [inputText frame].size.width - 12.0;
    float lineH = [normalFont ascender] - [normalFont descender] + 2.0;
    float spaceW = ui_text_width(normalFont, @" ");
    unsigned i, n = [s length], start = 0;
    int lines = 0;
    float h;

    if (avail < 20.0) avail = 20.0;
    for (i = 0; i <= n; i++) {
        if (i == n || [s characterAtIndex:i] == '\n') {
            NSArray *words = [[s substringWithRange:NSMakeRange(start, i - start)] componentsSeparatedByString:@" "];
            unsigned wi;
            float used = 0;
            int pl = 1;

            for (wi = 0; wi < [words count]; wi++) {
                NSString *word = [words objectAtIndex:wi];
                float ww = ui_text_width(normalFont, word);

                if (used > 0 && used + spaceW + ww > avail) {
                    pl++;
                    used = 0;
                }
                if (used > 0) used += spaceW;
                while (ww > avail) {                 /* a word longer than the line is broken wherever it falls */
                    pl++;
                    ww -= avail;
                }
                used += ww;
            }
            lines += pl;
            start = i + 1;
        }
    }
    h = lines * lineH + 8.0;
    if (h < INPUT_HEIGHT) h = INPUT_HEIGHT;
    if (h > INPUT_MAX_HEIGHT) h = INPUT_MAX_HEIGHT;
    return h;
}

- (void)layoutSubviews
{
    NSRect b = [view bounds];
    float inH = (inputHeight > 0) ? inputHeight : INPUT_HEIGHT;
    NSScrollView *scroll = messageScroll;
    BOOL atEnd = NO;

    if (inH > b.size.height / 2) inH = b.size.height / 2;
    if (inH < INPUT_HEIGHT) inH = INPUT_HEIGHT;
    if (scroll != nil) {
        NSRect vis = [[scroll contentView] bounds];
        atEnd = NSMaxY(vis) >= [messageText frame].size.height - 2.0;
    }
    [inputScroll setFrame:NSMakeRect(0, 0, b.size.width, inH)];
    [typingLabel setFrame:NSMakeRect(2, inH + 1, b.size.width - 4, TYPING_HEIGHT - 2)];
    [messageScroll setFrame:NSMakeRect(0, inH + TYPING_HEIGHT, b.size.width, b.size.height - inH - TYPING_HEIGHT)];
    [inputScroll setHasVerticalScroller:(inH >= INPUT_MAX_HEIGHT)];
    if (atEnd) [messageText scrollRangeToVisible:NSMakeRange([[messageText string] length], 0)];
}

/* The text changed: grow or shrink to fit. */
- (void)textDidChange:(NSNotification *)note
{
    float h = [self wantedInputHeight];

    if (channel != nil && [[inputText string] length] > 0 && [owner respondsToSelector:@selector(channelPane:userTypingInChannel:)]) {
        [owner channelPane:self userTypingInChannel:channel];
    }

    if (h != inputHeight) {
        inputHeight = h;
        [self layoutSubviews];
    }
    [inputText scrollRangeToVisible:[inputText selectedRange]];
}

- (void)inputReturnPressed:(BOOL)newline
{
    if (newline) {
        [inputText insertText:@"\n"];
        [self textDidChange:nil];
    } else {
        [self inputSubmitted:nil];
    }
}

- (void)setDraft:(NSString *)text
{
    [inputText setString:text];
    [self textDidChange:nil];
}

/* Who is typing in this channel, on the line between the log and the input; called from the polling timer. */
- (void)updateTypingAt:(KMs)now
{
    char text[200];
    Channel *c = channel ? model_find_channel(model, [channel cString]) : NULL;
    NSString *s;

    model_typing_text(c, now, text, sizeof(text));
    s = S(text);
    if (![s isEqualToString:[typingLabel stringValue]]) [typingLabel setStringValue:s];
}

- (NSString *)typingString { return [typingLabel stringValue]; }

- (NSString *)draft { return [inputText string]; }
- (id)inputView { return inputText; }
- (id)logView { return messageText; }

- (BOOL)acceptDroppedFiles:(NSArray *)paths
{
    if ([paths count] == 0) return NO;
    if (channel == nil) {
        [self appendNotice:@"Choose a channel first."];
        return NO;
    }
    if (![owner respondsToSelector:@selector(channelPane:didDropFiles:caption:forChannel:)] ||
        ![owner channelPane:self didDropFiles:paths caption:[inputText string] forChannel:channel]) {
        return NO;
    }
    [self setDraft:@""];
    return YES;
}
- (float)inputHeight { return (inputHeight > 0) ? inputHeight : INPUT_HEIGHT; }

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

/* Text with emoji drawn as pictures: Unicode emoji (found in the text itself) and custom emoji (at the given positions),
 * each only where its picture is on hand; everything else is text. */
- (void)appendRich:(const char *)text customs:(const Emoji *)customs count:(int)ncustoms prefix:(NSString *)prefix
             color:(NSColor *)color font:(NSFont *)font
{
    EmojiPack *pack = imageSource != nil ? [imageSource emojiPack] : NULL;
    size_t len = strlen(text), at = 0, scan = 0, us = 0, ue = 0;
    int ci = 0, have_u = 0;
    char key[EMOJI_KEY_MAX];

    if (prefix) [self appendString:prefix color:color font:font];
    if (pack == NULL && (customs == NULL || ncustoms == 0)) {
        [self appendString:S(text) color:color font:font];
        return;
    }
    for (;;) {
        size_t next_custom = (customs != NULL && ci < ncustoms) ? (size_t)customs[ci].off : len + 1;
        NSImage *img = nil;
        size_t from, to;

        if (pack != NULL && !have_u) {
            have_u = emoji_next(pack, text, len, scan, &us, &ue, key) ? 1 : 2;       /* 2: no more */
        }
        if (customs != NULL && ci < ncustoms && next_custom < len + 1 && (have_u != 1 || next_custom <= us)) {
            /* a custom emoji comes first (if its picture has arrived) */
            const Emoji *e = &customs[ci++];

            if (next_custom < at || (size_t)(e->off + e->len) > len) continue;
            img = [imageSource inlineEmojiImageForID:S(e->id)];
            from = (size_t)e->off;
            to = (size_t)(e->off + e->len);
            if (img == nil) {
                scan = scan > to ? scan : to;
                continue;
            }
            if (have_u == 1 && us < to) have_u = 0;                               /* (cannot overlap, but be safe) */
        } else if (have_u == 1) {
            img = [imageSource unicodeEmojiImageForKey:S(key)];
            from = us;
            to = ue;
            have_u = 0;
            scan = ue;
            if (img == nil) continue;                                             /* no picture: leave it as text */
        } else {
            break;
        }
        if (from > at) {
            char *seg = (char *)malloc(from - at + 1);

            if (seg == NULL) return;
            memcpy(seg, text + at, from - at);
            seg[from - at] = '\0';
            [self appendString:S(seg) color:color font:font];
            free(seg);
        }
        [[messageText textStorage] appendAttributedString:ui_image_attachment(img)];
        at = to;
        if (scan < to) scan = to;
    }
    [self appendString:S(text + at) color:color font:font];
}

/* One grey line of "[emoji count]" boxes under a message; a * after the count marks one we made. */
- (void)appendReactions:(const Message *)m
{
    char line[MODEL_MAX_REACTS * 112 + 8];
    Emoji ce[MODEL_MAX_REACTS];
    int i, n = 0, nce = 0;
    size_t at = 0;

    for (i = 0; i < m->nreact; i++) {
        const Reaction *r = &m->react[i];
        size_t nlen;

        line[at++] = '[';
        if (r->id[0] != '\0') {
            nlen = strlen(r->name) + 2;
            ce[nce].off = (int)at;
            ce[nce].len = (int)nlen;
            strncpy(ce[nce].id, r->id, sizeof(ce[nce].id) - 1);
            ce[nce].id[sizeof(ce[nce].id) - 1] = '\0';
            ce[nce].animated = r->animated;
            nce++;
            line[at++] = ':';
            memcpy(line + at, r->name, nlen - 2);
            at += nlen - 2;
            line[at++] = ':';
        } else {
            nlen = strlen(r->name);
            memcpy(line + at, r->name, nlen);
            at += nlen;
        }
        at += (size_t)sprintf(line + at, " %d%s]%s", r->count, r->me ? "*" : "", i + 1 < m->nreact ? " " : "");
        n++;
    }
    line[at] = '\0';
    if (n == 0) return;
    [self appendRich:line customs:(inlineImages && imageSource != nil) ? ce : NULL count:nce prefix:@"    " color:[NSColor darkGrayColor] font:normalFont];
    [self appendString:@"\n" color:[NSColor darkGrayColor] font:normalFont];
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
    [self appendRich:m->author customs:NULL count:0 prefix:nil color:author_color(m->author) font:boldFont];
    [self appendString:@": " color:[NSColor blackColor] font:normalFont];
    [self appendRich:m->text customs:(inlineImages && imageSource != nil) ? m->emoji : NULL count:m->nemoji prefix:nil color:[NSColor blackColor] font:normalFont];
    if (m->edited) [self appendString:@" (edited)" color:[NSColor darkGrayColor] font:normalFont];
    [self appendString:@"\n" color:[NSColor blackColor] font:normalFont];
    if (m->nreact > 0) [self appendReactions:m];
    if (m->embeds != NULL && m->embeds[0] != '\0') {          /* link previews and other embeds, one grey line each */
        NSArray *lines = [S(m->embeds) componentsSeparatedByString:@"\n"];
        unsigned li;

        for (li = 0; li < [lines count]; li++) {
            NSData *u8 = ui_utf8_cstring([lines objectAtIndex:li]);

            [self appendRich:(const char *)[u8 bytes] customs:NULL count:0 prefix:@"    " color:[NSColor darkGrayColor] font:normalFont];
            [self appendString:@"\n" color:[NSColor darkGrayColor] font:normalFont];
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
    if (inlineImages && imageSource != nil) {
        /* ask for the newest pictures first: the queue is short, and those are the ones being looked at */
        for (i = c->nmsgs - 1; i >= 0; i--) {
            const Message *m = &c->msgs[i];
            int j;

            for (j = 0; j < m->natt; j++) {
                if (m->att[j].is_image) {
                    [imageSource inlineImageForURL:S(m->att[j].url) width:m->att[j].width height:m->att[j].height
                                              jpeg:(m->att[j].is_jpeg ? YES : NO)];
                }
            }
        }
    }
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
    NSString *text = ui_trim([inputText string]);

    if ([text length] == 0) return;
    if (channel == nil) {
        [self appendNotice:@"Choose a channel first."];
        return;
    }
    [self setDraft:@""];
    [owner channelPane:self didSubmitText:text forChannel:channel];
}

- (void)typeText:(NSString *)text
{
    [self setDraft:text];
    [self inputSubmitted:nil];
}

@end
