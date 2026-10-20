#import "MainController.h"
#import "UIHelpers.h"
#include <string.h>

enum { ROW_GUILD, ROW_CATEGORY, ROW_CHANNEL };

#define LIST_WIDTH 200.0
#define STATUS_HEIGHT 20.0

static NSString *S(const char *s) { return ui_string_from_utf8(s); }

/* A row's text as UTF-8 for EmojiCell: lead + name + tail (the name in capitals, ASCII letters only, if `upper`). */
static NSData *raw_label(const char *lead, const char *name, const char *tail, int upper)
{
    size_t n = strlen(lead) + strlen(name) + strlen(tail);
    char *buf = (char *)malloc(n + 1), *p;
    NSData *d;

    if (buf == NULL) return nil;
    sprintf(buf, "%s%s%s", lead, name, tail);
    if (upper) {
        for (p = buf + strlen(lead); p < buf + strlen(lead) + strlen(name); p++) {
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        }
    }
    d = [NSData dataWithBytes:buf length:n + 1];
    free(buf);
    return d;
}

/* One line of the server/channel list. */
@interface ListRow : NSObject
{
@public
    int       kind;
    NSString *ident;
    NSString *label;
    NSData   *raw;               /* the same text as UTF-8 with its emoji still in it (for drawing them), or nil */
}
- (id)initWithKind:(int)k ident:(NSString *)i label:(NSString *)l raw:(NSData *)r;
@end

@implementation ListRow
- (id)initWithKind:(int)k ident:(NSString *)i label:(NSString *)l raw:(NSData *)r
{
    self = [super init];
    if (!self) return nil;
    kind = k;
    ident = [i retain];
    label = [l retain];
    raw = [r retain];
    return self;
}
- (void)dealloc
{
    [ident release];
    [label release];
    [raw release];
    [super dealloc];
}
@end

/* A cell that draws a row's text with its Unicode emoji as pictures. The text is cut at each emoji: the pieces are drawn by the
 * ordinary text cell and the pictures by image cells (the same NSImageCell/NSTextAttachmentCell machinery the message log
 * relies on). Without a pack, or for an emoji with no picture, the row is the plain text (emoji as ":names:"). */
@interface EmojiCell : NSTextFieldCell
{
    id      source;              /* answers -emojiPack and -unicodeEmojiImageForKey: (not retained) */
    NSData *raw;                 /* this row's text as UTF-8, or nil */
}
- (void)setSource:(id)s;
- (void)setRaw:(NSData *)r;
@end

#define EMOJI_ROW_PICTURE 16.0

@implementation EmojiCell
- (void)setSource:(id)s { source = s; }
- (void)setRaw:(NSData *)r
{
    if (r != raw) {
        [raw release];
        raw = [r retain];
    }
}
- (void)dealloc
{
    [raw release];
    [super dealloc];
}

/* NSCell copies itself bit for bit (AppKit copies cells while drawing, on OPENSTEP at least), which would leave the copy
 * pointing at `raw` without a retain of its own and so release it once too often when it dies: a crash a little later. */
- (id)copyWithZone:(NSZone *)zone
{
    EmojiCell *c = [super copyWithZone:zone];

    c->raw = [raw retain];
    return c;
}

/* The cell's own text drawing for one piece, in the cell's colour and font. */
- (float)drawPiece:(NSString *)piece at:(float)x frame:(NSRect)frame inView:(NSView *)view
{
    float w = ui_text_width([self font], piece);

    if ([piece length] == 0) return 0;
    [super setStringValue:piece];
    [super drawInteriorWithFrame:NSMakeRect(x, frame.origin.y, w + 8.0, frame.size.height) inView:view];
    return w;
}

- (void)drawInteriorWithFrame:(NSRect)frame inView:(NSView *)view
{
    EmojiPack *pack = (source != nil && raw != nil) ? [source emojiPack] : NULL;
    const char *s;
    size_t len, at = 0, scan = 0, us, ue;
    float x = frame.origin.x, right = NSMaxX(frame);
    char key[EMOJI_KEY_MAX];
    NSString *whole = [self stringValue];

    if (pack == NULL) {
        [super drawInteriorWithFrame:frame inView:view];
        return;
    }
    s = (const char *)[raw bytes];
    len = strlen(s);
    {
        static int drawn;

        if (drawn++ < 80 && [self isHighlighted]) SSTrace("list cell draws a selected row: \"%s\"", s);
    }
    while (x < right && emoji_next(pack, s, len, scan, &us, &ue, key)) {
        NSImage *img = [source unicodeEmojiImageForKey:S(key)];
        char *seg;
        static int traced;

        if (traced++ < 60) SSTrace("list row emoji: bytes %d-%d key %s, picture %s, row \"%s\"", (int)us, (int)ue, key, img ? "yes" : "no", s);

        scan = ue;
        if (img == nil) continue;                                      /* no picture: it stays in the text */
        seg = (char *)malloc(us - at + 1);
        if (seg == NULL) break;
        memcpy(seg, s + at, us - at);
        seg[us - at] = '\0';
        x += [self drawPiece:S(seg) at:x frame:frame inView:view];
        free(seg);
        {
            NSImageCell *ic = [[[NSImageCell alloc] initImageCell:img] autorelease];

            [ic setImageFrameStyle:NSImageFrameNone];
            [ic drawWithFrame:NSMakeRect(x + 1.0, frame.origin.y + (frame.size.height - EMOJI_ROW_PICTURE) / 2.0,
                                         EMOJI_ROW_PICTURE, EMOJI_ROW_PICTURE) inView:view];
            x += EMOJI_ROW_PICTURE + 3.0;
        }
        at = ue;
    }
    if (at < len && x < right) {
        [self drawPiece:S(s + at) at:x frame:frame inView:view];
    }
    [super setStringValue:whole];
}
@end

/* gcc 2.7.2 does not look ahead within an @implementation. */
@interface MainController (Private)
- (void)buildWindow;
- (void)reloadList;
- (void)selectChannelId:(NSString *)cid;
- (void)updateTitle;
- (void)listClicked:(id)sender;
- (void)listDoubleClicked:(id)sender;
@end

@implementation MainController

- (id)initWithOwner:(id)anOwner model:(Model *)aModel
{
    self = [super init];
    if (!self) return nil;
    owner = anOwner;
    model = aModel;
    rows = [[NSMutableArray alloc] init];
    collapsed = [[NSMutableSet alloc] init];
    return self;
}

- (void)dealloc
{
    [window setDelegate:nil];
    [window release];
    [listTable release]; [emojiCell release]; [pane release]; [statusLabel release];
    [rows release]; [collapsed release];
    [super dealloc];
}

/* ---------------------------------------------------------------- window */

- (void)buildWindow
{
    NSRect content = NSMakeRect(0, 0, 800, 520);
    NSScrollView *listScroll;
    NSTableColumn *col;
    NSRect scr = [[NSScreen mainScreen] frame];

    window = [[NSWindow alloc] initWithContentRect:content
                                         styleMask:(NSTitledWindowMask | NSClosableWindowMask |
                                                    NSMiniaturizableWindowMask | NSResizableWindowMask)
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    [window setReleasedWhenClosed:NO];
    [window setDelegate:(id)self];
    [window setMinSize:NSMakeSize(420, 240)];
    [window setTitle:@"Kallisti"];

    listScroll = [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, STATUS_HEIGHT, LIST_WIDTH, content.size.height - STATUS_HEIGHT)] autorelease];
    col = [[[NSTableColumn alloc] initWithIdentifier:@"name"] autorelease];
    [[col headerCell] setStringValue:@"Servers and channels"];
    [col setWidth:LIST_WIDTH - 20.0];
    [col setEditable:NO];
    listTable = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, LIST_WIDTH, 100)];
    emojiCell = [[EmojiCell alloc] init];
    [emojiCell setFont:[NSFont systemFontOfSize:12.0]];
    [col setDataCell:emojiCell];
    [listTable addTableColumn:col];
    [listTable setRowHeight:18.0];
    [listTable setDataSource:(id)self];
    [listTable setDelegate:(id)self];
    [listTable setAllowsMultipleSelection:NO];
    [listTable setTarget:self];
    [listTable setAction:@selector(listClicked:)];
    [listTable setDoubleAction:@selector(listDoubleClicked:)];
    [listScroll setHasVerticalScroller:YES];
    [listScroll setDocumentView:listTable];
    [listScroll setAutoresizingMask:NSViewHeightSizable];
    [[window contentView] addSubview:listScroll];

    pane = [[ChannelPane alloc] initWithOwner:self model:model
                                        frame:NSMakeRect(LIST_WIDTH + 4.0, STATUS_HEIGHT,
                                                         content.size.width - LIST_WIDTH - 4.0,
                                                         content.size.height - STATUS_HEIGHT)];
    [[window contentView] addSubview:[pane view]];

    statusLabel = [ui_label(@"", NSMakeRect(4, 0, content.size.width - 8.0, STATUS_HEIGHT - 2.0)) retain];
    [statusLabel setAutoresizingMask:(NSViewWidthSizable | NSViewMaxYMargin)];
    [[window contentView] addSubview:statusLabel];

    [window setFrameTopLeftPoint:NSMakePoint(scr.origin.x + 40, NSMaxY(scr) - 40)];
}

- (void)show
{
    if (!window) {
        [self buildWindow];
        [self reloadList];
    }
    [window makeKeyAndOrderFront:nil];
    [pane focusInput];
}

- (void)closeWindow
{
    [window setDelegate:nil];
    [window orderOut:nil];
}

- (void)windowWillClose:(NSNotification *)notification
{
    [window setDelegate:nil];
    [owner mainControllerWindowClosed:self];
}

- (void)setStatus:(NSString *)text
{
    [statusLabel setStringValue:text];
}

- (NSString *)currentChannel { return [pane channel]; }

- (void)setImageSource:(id)source showInline:(BOOL)showInline
{
    if (!window) {
        [self buildWindow];
        [self reloadList];
    }
    imageSource = source;
    [pane setImageSource:source showInline:showInline];
    [listTable reloadData];                          /* emoji in names appear or vanish with the setting */
}

- (void)reloadMessages { [pane reload]; }

- (void)updateTitle
{
    NSString *cid = [pane channel];
    Channel *c = cid ? model_find_channel(model, [cid cString]) : NULL;
    Guild *g = c ? model_find_guild(model, c->guild_id) : NULL;

    if (c && g) {
        [window setTitle:[NSString stringWithFormat:@"Kallisti - %@%@ (%@)", c->type == CH_DM ? @"" : @"#", S(c->name), S(g->name)]];
    } else {
        [window setTitle:@"Kallisti"];
    }
}

/* ---------------------------------------------------------------- the list */

- (int)numberOfRowsInTableView:(NSTableView *)tv { return (int)[rows count]; }

- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col row:(int)row
{
    return ((ListRow *)[rows objectAtIndex:row])->label;
}

/* Server names in bold, so they stand apart from the channels (and from each other's channels) in a long list. Cells are
 * reused, so every row sets its font. */
- (void)tableView:(NSTableView *)tv willDisplayCell:(id)cell forTableColumn:(NSTableColumn *)col row:(int)row
{
    BOOL isGuild = row >= 0 && row < (int)[rows count] && ((ListRow *)[rows objectAtIndex:row])->kind == ROW_GUILD;

    if ([cell respondsToSelector:@selector(setFont:)]) {
        [cell setFont:isGuild ? [NSFont boldSystemFontOfSize:12.0] : [NSFont systemFontOfSize:12.0]];
    }
    if ([cell isKindOfClass:[EmojiCell class]]) {
        [cell setSource:imageSource];
        [cell setRaw:(row >= 0 && row < (int)[rows count]) ? ((ListRow *)[rows objectAtIndex:row])->raw : nil];
    }
}

- (void)reloadList
{
    int gi, ci, selected = -1;
    unsigned i;
    NSString *current = [pane channel];

    [rows removeAllObjects];
    for (gi = 0; gi < model->nguilds; gi++) {
        Guild *g = &model->guilds[gi];
        NSString *gid = S(g->id);
        BOOL open = ![collapsed containsObject:gid];
        NSString *title = S(g->name);

        if (g->unavailable) title = [title stringByAppendingString:@" (unavailable)"];
        [rows addObject:[[[ListRow alloc] initWithKind:ROW_GUILD ident:gid
                                                 label:[(open ? @"- " : @"+ ") stringByAppendingString:title]
                                                   raw:raw_label(open ? "- " : "+ ", g->name, g->unavailable ? " (unavailable)" : "", 0)] autorelease]];
        if (!open) continue;
        for (ci = 0; ci < g->nch; ci++) {
            Channel *c = &g->ch[ci];
            NSString *label;
            char count[24], lead[16];

            count[0] = '\0';
            if (c->type == CH_CATEGORY) {
                label = [@"  " stringByAppendingString:[S(c->name) uppercaseString]];
                [rows addObject:[[[ListRow alloc] initWithKind:ROW_CATEGORY ident:S(c->id) label:label
                                                           raw:raw_label("  ", c->name, "", 1)] autorelease]];
            } else {
                label = [NSString stringWithFormat:@"%@%@%@", c->parent_id[0] ? @"    " : @"  ",
                         c->type == CH_DM ? @"@" : @"# ", S(c->name)];
                if (c->mentions > 0) {
                    label = [label stringByAppendingFormat:@" (%d!)", c->unread];
                    sprintf(count, " (%d!)", c->unread);
                } else if (c->unread > 0) {
                    label = [label stringByAppendingFormat:@" (%d)", c->unread];
                    sprintf(count, " (%d)", c->unread);
                }
                sprintf(lead, "%s%s", c->parent_id[0] ? "    " : "  ", c->type == CH_DM ? "@" : "# ");
                [rows addObject:[[[ListRow alloc] initWithKind:ROW_CHANNEL ident:S(c->id) label:label
                                                           raw:raw_label(lead, c->name, count, 0)] autorelease]];
            }
        }
    }
    SSTrace("list: reloadData (%d rows)", (int)[rows count]);
    [listTable reloadData];
    SSTrace("list: reloaded");
    if (current) {
        for (i = 0; i < [rows count]; i++) {
            ListRow *r = (ListRow *)[rows objectAtIndex:i];
            if (r->kind == ROW_CHANNEL && [r->ident isEqualToString:current]) selected = (int)i;
        }
    }
    if (selected >= 0) [listTable selectRow:selected byExtendingSelection:NO];
    else [listTable deselectAll:nil];
}

- (void)listClicked:(id)sender
{
    int row = [listTable selectedRow];
    ListRow *r;

    if (row < 0 || row >= (int)[rows count]) return;
    r = (ListRow *)[rows objectAtIndex:row];
    SSTrace("list click: row %d kind %d \"%s\"", row, r->kind, [r->label cString]);
    if (r->kind == ROW_GUILD) {
        if ([collapsed containsObject:r->ident]) [collapsed removeObject:r->ident];
        else [collapsed addObject:r->ident];
        [self reloadList];
    } else if (r->kind == ROW_CHANNEL) {
        [self selectChannelId:r->ident];
    } else {
        [self reloadList];                      /* a category heading: put the selection back */
    }
}

/* A double-click on a channel opens it in a window of its own (the first click of the pair has already
 * selected it in this window). */
- (void)listDoubleClicked:(id)sender
{
    int row = [listTable selectedRow];
    ListRow *r;

    if (row < 0 || row >= (int)[rows count]) return;
    r = (ListRow *)[rows objectAtIndex:row];
    if (r->kind == ROW_CHANNEL) [owner mainController:self didRequestWindowForChannel:r->ident];
}

- (void)selectChannelId:(NSString *)cid
{
    SSTrace("select %s: pane", [cid cString]);
    [pane setChannel:cid];
    SSTrace("select: title");
    [self updateTitle];
    SSTrace("select: list");
    [self reloadList];
    SSTrace("select: owner");
    [owner mainController:self didSelectChannel:cid];
    SSTrace("select: focus");
    [pane focusInput];
    SSTrace("select: done");
}

- (void)selectChannelWithId:(NSString *)channelId { [self selectChannelId:channelId]; }

- (void)appendNotice:(NSString *)text { [pane appendNotice:text]; }

- (void)modelChanged:(const ModelChange *)change
{
    if (!window) return;
    switch (change->type) {
    case MC_RESET:
    case MC_GUILDS:
    case MC_CHANNELS:
        [pane modelChanged:change];
        [self updateTitle];
        [self reloadList];
        break;
    case MC_UNREAD:
        [self reloadList];
        break;
    default:
        [pane modelChanged:change];
        break;
    }
}

/* ---------------------------------------------------------------- the pane's owner protocol, and the tests */

- (void)channelPane:(ChannelPane *)p userTypingInChannel:(NSString *)channelId
{
    [owner mainController:self userTypingInChannel:channelId];
}

- (BOOL)channelPane:(ChannelPane *)p didDropFiles:(NSArray *)paths caption:(NSString *)caption forChannel:(NSString *)channelId
{
    return [owner mainController:self didDropFiles:paths caption:caption forChannel:channelId];
}

- (void)channelPane:(ChannelPane *)p didSubmitText:(NSString *)text forChannel:(NSString *)channelId
{
    [owner mainController:self didSubmitText:text forChannel:channelId];
}

#ifndef OPENSTEP
/* For the tests: the cell is copied and the copy dropped, as AppKit may do while drawing. */
- (void)copyCellForTest
{
    id c = [emojiCell copy];

    [c release];
}

/* For the tests: row `row` of the list drawn the way the table draws it, as pixels. */
- (NSBitmapImageRep *)renderRow:(int)row width:(float)w
{
    NSRect r = [listTable rectOfRow:row];
    NSBitmapImageRep *rep;

    r.size.width = w;
    rep = [listTable bitmapImageRepForCachingDisplayInRect:r];
    [listTable cacheDisplayInRect:r toBitmapImageRep:rep];
    return rep;
}
#endif

- (NSArray *)listLabels
{
    NSMutableArray *labels = [NSMutableArray array];
    unsigned i;

    for (i = 0; i < [rows count]; i++) [labels addObject:((ListRow *)[rows objectAtIndex:i])->label];
    return labels;
}

- (NSString *)messageString { return [pane messageString]; }
- (NSString *)statusString { return [statusLabel stringValue]; }
- (void)typeText:(NSString *)text { [pane typeText:text]; }
- (void)setDraft:(NSString *)text { [pane setDraft:text]; }
- (void)attachFiles:(NSArray *)paths { [pane acceptDroppedFiles:paths]; }
- (id)inputView { return [pane inputView]; }
- (id)logView { return [pane logView]; }
- (void)updateTypingAt:(KMs)now { [pane updateTypingAt:now]; }
- (NSString *)typingString { return [pane typingString]; }
- (NSString *)draft { return [pane draft]; }
- (float)inputHeight { return [pane inputHeight]; }
- (void)inputReturnPressed:(BOOL)newline { [pane inputReturnPressed:newline]; }

@end
