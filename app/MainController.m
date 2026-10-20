#import "MainController.h"
#import "UIHelpers.h"
#include <string.h>

enum { ROW_GUILD, ROW_CATEGORY, ROW_CHANNEL };

#define LIST_WIDTH 200.0
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
    [listTable release]; [pane release]; [statusLabel release];
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
    [listTable addTableColumn:col];
    [listTable setDataSource:(id)self];
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
    [pane setImageSource:source showInline:showInline];
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
                                                 label:[(open ? @"- " : @"+ ") stringByAppendingString:title]] autorelease]];
        if (!open) continue;
        for (ci = 0; ci < g->nch; ci++) {
            Channel *c = &g->ch[ci];
            NSString *label;

            if (c->type == CH_CATEGORY) {
                label = [@"  " stringByAppendingString:[S(c->name) uppercaseString]];
                [rows addObject:[[[ListRow alloc] initWithKind:ROW_CATEGORY ident:S(c->id) label:label] autorelease]];
            } else {
                label = [NSString stringWithFormat:@"%@%@%@", c->parent_id[0] ? @"    " : @"  ",
                         c->type == CH_DM ? @"@" : @"# ", S(c->name)];
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
    [pane setChannel:cid];
    [self updateTitle];
    [self reloadList];
    [owner mainController:self didSelectChannel:cid];
    [pane focusInput];
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

- (void)channelPane:(ChannelPane *)p didSubmitText:(NSString *)text forChannel:(NSString *)channelId
{
    [owner mainController:self didSubmitText:text forChannel:channelId];
}

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
- (void)updateTypingAt:(KMs)now { [pane updateTypingAt:now]; }
- (NSString *)typingString { return [pane typingString]; }
- (NSString *)draft { return [pane draft]; }
- (float)inputHeight { return [pane inputHeight]; }
- (void)inputReturnPressed:(BOOL)newline { [pane inputReturnPressed:newline]; }

@end
