#import "MainController.h"
#import "UIHelpers.h"
#include <string.h>
#include "ktime.h"

enum { ROW_GUILD, ROW_CATEGORY, ROW_CHANNEL };

#define LIST_WIDTH 200.0
#define INPUT_HEIGHT 24.0
#define STATUS_HEIGHT 20.0

static NSString *S(const char *s) { return ui_string_from_utf8(s); }

/* One line of the server/channel list. */
@interface ListRow : NSObject
{
@public
    int       kind;
    NSString *ident;
    NSString *label;
}
- (id)initWithKind:(int)k ident:(NSString *)i label:(NSString *)l;
@end

@implementation ListRow
- (id)initWithKind:(int)k ident:(NSString *)i label:(NSString *)l
{
    self = [super init];
    if (!self) return nil;
    kind = k;
    ident = [i retain];
    label = [l retain];
    return self;
}
- (void)dealloc
{
    [ident release];
    [label release];
    [super dealloc];
}
@end

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
@interface MainController (Private)
- (void)buildWindow;
- (void)reloadList;
- (void)selectChannelId:(NSString *)cid;
- (void)showCurrentChannel;
- (void)appendString:(NSString *)s color:(NSColor *)color font:(NSFont *)font;
- (void)appendMessage:(const Message *)m;
- (void)updateTitle;
- (void)inputSubmitted:(id)sender;
- (void)listClicked:(id)sender;
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
    normalFont = [[NSFont systemFontOfSize:12.0] retain];
    boldFont = [[NSFont boldSystemFontOfSize:12.0] retain];
    return self;
}

- (void)dealloc
{
    [window setDelegate:nil];
    [window release];
    [listTable release]; [messageScroll release]; [messageText release];
    [inputField release]; [statusLabel release];
    [rows release]; [collapsed release]; [currentChannel release];
    [normalFont release]; [boldFont release];
    [super dealloc];
}

/* ---------------------------------------------------------------- window */

- (void)buildWindow
{
    NSRect content = NSMakeRect(0, 0, 800, 520);
    NSScrollView *listScroll;
    NSTableColumn *col;
    NSRect scr = [[NSScreen mainScreen] frame];
    float top = STATUS_HEIGHT + INPUT_HEIGHT;

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
    [listTable addTableColumn:col];
    [listTable setDataSource:(id)self];
    [listTable setAllowsMultipleSelection:NO];
    [listTable setTarget:self];
    [listTable setAction:@selector(listClicked:)];
    [listScroll setHasVerticalScroller:YES];
    [listScroll setDocumentView:listTable];
    [listScroll setAutoresizingMask:NSViewHeightSizable];
    [[window contentView] addSubview:listScroll];

    messageScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(LIST_WIDTH + 4.0, top,
                                                                   content.size.width - LIST_WIDTH - 4.0,
                                                                   content.size.height - top)];
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
    [[window contentView] addSubview:messageScroll];

    inputField = [[NSTextField alloc] initWithFrame:NSMakeRect(LIST_WIDTH + 4.0, STATUS_HEIGHT,
                                                               content.size.width - LIST_WIDTH - 4.0, INPUT_HEIGHT)];
    [inputField setAutoresizingMask:(NSViewWidthSizable | NSViewMaxYMargin)];
    [inputField setTarget:self];
    [inputField setAction:@selector(inputSubmitted:)];
    [[window contentView] addSubview:inputField];

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
    [window makeFirstResponder:inputField];
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

- (void)updateTitle
{
    Channel *c = currentChannel ? model_find_channel(model, [currentChannel cString]) : NULL;
    Guild *g = c ? model_find_guild(model, c->guild_id) : NULL;

    if (c && g) {
        [window setTitle:[NSString stringWithFormat:@"Kallisti - #%@ (%@)", S(c->name), S(g->name)]];
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

- (void)reloadList
{
    int gi, ci, selected = -1;
    unsigned i;

    [rows removeAllObjects];
    for (gi = 0; gi < model->nguilds; gi++) {
        Guild *g = &model->guilds[gi];
        NSString *gid = S(g->id);
        BOOL open = ![collapsed containsObject:gid];
        NSString *title = S(g->name);

        if (g->unavailable) title = [title stringByAppendingString:@" (unavailable)"];
        [rows addObject:[[[ListRow alloc] initWithKind:ROW_GUILD ident:gid
                                                 label:[(open ? @"- " : @"+ ") stringByAppendingString:title]] autorelease]];
        if (!open) continue;
        for (ci = 0; ci < g->nch; ci++) {
            Channel *c = &g->ch[ci];
            NSString *label;

            if (c->type == CH_CATEGORY) {
                label = [@"  " stringByAppendingString:[S(c->name) uppercaseString]];
                [rows addObject:[[[ListRow alloc] initWithKind:ROW_CATEGORY ident:S(c->id) label:label] autorelease]];
            } else {
                label = [NSString stringWithFormat:@"%@# %@", c->parent_id[0] ? @"    " : @"  ", S(c->name)];
                if (c->mentions > 0) {
                    label = [label stringByAppendingFormat:@" (%d!)", c->unread];
                } else if (c->unread > 0) {
                    label = [label stringByAppendingFormat:@" (%d)", c->unread];
                }
                [rows addObject:[[[ListRow alloc] initWithKind:ROW_CHANNEL ident:S(c->id) label:label] autorelease]];
            }
        }
    }
    [listTable reloadData];
    if (currentChannel) {
        for (i = 0; i < [rows count]; i++) {
            ListRow *r = (ListRow *)[rows objectAtIndex:i];
            if (r->kind == ROW_CHANNEL && [r->ident isEqualToString:currentChannel]) selected = (int)i;
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

- (void)selectChannelId:(NSString *)cid
{
    if (currentChannel != cid) {
        [currentChannel release];
        currentChannel = [cid retain];
    }
    model_mark_read(model, [currentChannel cString]);
    [self updateTitle];
    [self showCurrentChannel];
    [self reloadList];
    [owner mainController:self didSelectChannel:currentChannel];
    [window makeFirstResponder:inputField];
}

/* ---------------------------------------------------------------- the messages */

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
}

- (void)showCurrentChannel
{
    Channel *c = currentChannel ? model_find_channel(model, [currentChannel cString]) : NULL;
    int i;

    [messageText setString:@""];
    lastDay[0] = '\0';
    if (c == NULL) return;
    for (i = 0; i < c->nmsgs; i++) [self appendMessage:&c->msgs[i]];
    [messageText scrollRangeToVisible:NSMakeRange([[messageText textStorage] length], 0)];
}

- (void)appendNotice:(NSString *)text
{
    unsigned len = [[messageText textStorage] length];

    [self appendString:[NSString stringWithFormat:@"* %@\n", text] color:[NSColor darkGrayColor] font:normalFont];
    [messageText scrollRangeToVisible:NSMakeRange(len, 0)];
}

- (void)reloadEverything
{
    if (currentChannel && model_find_channel(model, [currentChannel cString]) == NULL) {
        [currentChannel release];
        currentChannel = nil;
        [messageText setString:@""];
        [self updateTitle];
    }
    [self reloadList];
    if (currentChannel) [self showCurrentChannel];
}

- (void)modelChanged:(const ModelChange *)change
{
    NSRect vis;
    BOOL atBottom;
    Channel *c;

    if (!window) return;
    switch (change->type) {
    case MC_RESET:
    case MC_GUILDS:
    case MC_CHANNELS:
        if (currentChannel && model_find_channel(model, [currentChannel cString]) == NULL) {
            [currentChannel release];
            currentChannel = nil;
            [messageText setString:@""];
            [self updateTitle];
        }
        [self reloadList];
        break;
    case MC_UNREAD:
        [self reloadList];
        break;
    case MC_MESSAGE_ADDED:
        if (currentChannel && strcmp(change->channel_id, [currentChannel cString]) == 0) {
            c = model_find_channel(model, change->channel_id);
            if (c != NULL && c->nmsgs > 0 && strcmp(c->msgs[c->nmsgs - 1].id, change->message_id) == 0) {
                vis = [messageScroll documentVisibleRect];
                atBottom = NSMaxY(vis) >= NSMaxY([messageText frame]) - 30.0;
                [self appendMessage:&c->msgs[c->nmsgs - 1]];
                if (atBottom) [messageText scrollRangeToVisible:NSMakeRange([[messageText textStorage] length], 0)];
            } else {
                [self showCurrentChannel];
            }
            model_mark_read(model, change->channel_id);
        }
        break;
    case MC_MESSAGE_CHANGED:
    case MC_MESSAGE_REMOVED:
    case MC_HISTORY:
        if (currentChannel && strcmp(change->channel_id, [currentChannel cString]) == 0) {
            [self showCurrentChannel];
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- driving the window from tests */

- (NSArray *)listLabels
{
    NSMutableArray *labels = [NSMutableArray array];
    unsigned i;

    for (i = 0; i < [rows count]; i++) [labels addObject:((ListRow *)[rows objectAtIndex:i])->label];
    return labels;
}

- (NSString *)messageString { return [messageText string]; }
- (NSString *)statusString { return [statusLabel stringValue]; }
- (void)selectChannelWithId:(NSString *)channelId { [self selectChannelId:channelId]; }

- (void)typeText:(NSString *)text
{
    [inputField setStringValue:text];
    [self inputSubmitted:inputField];
}

/* ---------------------------------------------------------------- typing */

- (void)inputSubmitted:(id)sender
{
    NSString *text = ui_trim([inputField stringValue]);

    if ([text length] == 0) return;
    if (currentChannel == nil) {
        [self appendNotice:@"Choose a channel first."];
        return;
    }
    [inputField setStringValue:@""];
    [owner mainController:self didSubmitText:text forChannel:currentChannel];
}

@end
