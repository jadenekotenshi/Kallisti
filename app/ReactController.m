#import "ReactController.h"
#import "UIHelpers.h"

/* gcc 2.7.2 does not look ahead within an @implementation. */
@interface ReactController (Private)
- (void)buildPanel;
- (void)addClicked:(id)sender;
- (void)removeClicked:(id)sender;
- (void)closeClicked:(id)sender;
- (void)quickPicked:(id)sender;
- (void)updateInfo;
- (void)send:(BOOL)remove;
@end

@implementation ReactController

- (id)initWithOwner:(id)anOwner
{
    self = [super init];
    if (!self) return nil;
    owner = anOwner;
    ids = [[NSMutableArray alloc] init];
    labels = [[NSMutableArray alloc] init];
    infos = [[NSMutableArray alloc] init];
    return self;
}

- (void)dealloc
{
    [panel release]; [table release]; [emojiField release]; [info release]; [quick release];
    [ids release]; [labels release]; [infos release];
    [super dealloc];
}

- (void)buildPanel
{
    NSScrollView *scroll;
    NSTableColumn *col;
    static const char *common[] = { ":thumbsup:", ":heart:", ":joy:", ":tada:", ":eyes:", ":fire:", ":pray:", ":100:",
                                    ":thinking:", ":clap:", ":white_check_mark:", ":x:", ":cry:", ":rofl:", NULL };
    int i;

    panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 520, 380)
                                       styleMask:(NSTitledWindowMask | NSClosableWindowMask | NSResizableWindowMask)
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
    [panel setReleasedWhenClosed:NO];
    [panel setHidesOnDeactivate:NO];
    [panel setTitle:@"React to a Message"];
    [panel setMinSize:NSMakeSize(420, 280)];
    scroll = [[[NSScrollView alloc] initWithFrame:NSMakeRect(8, 124, 504, 248)] autorelease];
    col = [[[NSTableColumn alloc] initWithIdentifier:@"message"] autorelease];
    [[col headerCell] setStringValue:@"Messages (newest last)"];
    [col setWidth:480];
    [col setEditable:NO];
    table = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 504, 100)];
    [table addTableColumn:col];
    [table setDataSource:(id)self];
    [table setDelegate:(id)self];
    [table setAllowsMultipleSelection:NO];
    [scroll setHasVerticalScroller:YES];
    [scroll setDocumentView:table];
    [scroll setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [[panel contentView] addSubview:scroll];

    info = [ui_label(@"", NSMakeRect(8, 98, 504, 20)) retain];
    [info setAutoresizingMask:(NSViewWidthSizable | NSViewMaxYMargin)];
    [[panel contentView] addSubview:info];

    [[panel contentView] addSubview:ui_label(@"Emoji:", NSMakeRect(8, 66, 50, 20))];
    emojiField = [ui_field(NSMakeRect(60, 62, 250, 24)) retain];
    [emojiField setTarget:self];
    [emojiField setAction:@selector(addClicked:)];
    [[panel contentView] addSubview:emojiField];
    quick = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(320, 62, 190, 24) pullsDown:NO];
    [quick addItemWithTitle:@"Common..."];
    for (i = 0; common[i] != NULL; i++) [quick addItemWithTitle:[NSString stringWithCString:common[i]]];
    [quick setTarget:self];
    [quick setAction:@selector(quickPicked:)];
    [[panel contentView] addSubview:quick];

    [[panel contentView] addSubview:ui_button(@"Close", NSMakeRect(232, 10, 80, 28), self, @selector(closeClicked:))];
    [[panel contentView] addSubview:ui_button(@"Remove Mine", NSMakeRect(320, 10, 110, 28), self, @selector(removeClicked:))];
    [[panel contentView] addSubview:ui_button(@"React", NSMakeRect(440, 10, 72, 28), self, @selector(addClicked:))];
    [panel center];
}

- (void)showMessageIds:(NSArray *)messageIds labels:(NSArray *)messageLabels reactions:(NSArray *)reactionInfos
{
    if (!panel) [self buildPanel];
    [ids setArray:messageIds];
    [labels setArray:messageLabels];
    [infos setArray:reactionInfos];
    [table reloadData];
    if ([ids count] > 0) [table selectRow:(int)[ids count] - 1 byExtendingSelection:NO];
    [self updateInfo];
    [panel makeKeyAndOrderFront:nil];
    [panel makeFirstResponder:emojiField];
}

- (void)closePanel { [panel orderOut:nil]; }
- (void)closeClicked:(id)sender { [panel orderOut:nil]; }

- (void)updateInfo
{
    int row = [table selectedRow];

    if (row >= 0 && row < (int)[infos count]) {
        NSString *s = [infos objectAtIndex:row];

        [info setStringValue:([s length] > 0) ? [@"Reactions: " stringByAppendingString:s] : @"No reactions yet."];
    } else {
        [info setStringValue:@""];
    }
}

- (void)tableViewSelectionDidChange:(NSNotification *)note { [self updateInfo]; }

- (void)quickPicked:(id)sender
{
    if ([quick indexOfSelectedItem] > 0) {
        [emojiField setStringValue:[quick titleOfSelectedItem]];
        [quick selectItemAtIndex:0];
    }
}

- (int)numberOfRowsInTableView:(NSTableView *)tv { return (int)[labels count]; }

- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col row:(int)row
{
    return [labels objectAtIndex:row];
}

- (void)send:(BOOL)remove
{
    int row = [table selectedRow];
    NSString *text = ui_trim([emojiField stringValue]);

    if (row < 0 || row >= (int)[ids count] || [text length] == 0) {
        NSBeep();
        return;
    }
    if ([owner reactController:self reactWith:text toMessage:[ids objectAtIndex:row] remove:remove]) {
        [panel orderOut:nil];
    }
}

- (void)addClicked:(id)sender { [self send:NO]; }
- (void)removeClicked:(id)sender { [self send:YES]; }

@end
