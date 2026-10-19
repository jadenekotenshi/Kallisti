#import "ImageListController.h"
#import "UIHelpers.h"

@implementation ImageItem
- (id)initWithLabel:(NSString *)l url:(NSString *)u name:(NSString *)n width:(unsigned long)w height:(unsigned long)h
{
    self = [super init];
    if (!self) return nil;
    label = [l retain];
    url = [u retain];
    name = [n retain];
    width = w;
    height = h;
    return self;
}
- (void)dealloc
{
    [label release]; [url release]; [name release];
    [super dealloc];
}
@end

/* gcc 2.7.2 does not look ahead within an @implementation. */
@interface ImageListController (Private)
- (void)buildPanel;
- (void)openChosen:(id)sender;
- (void)closeClicked:(id)sender;
@end

@implementation ImageListController

- (id)initWithOwner:(id)anOwner
{
    self = [super init];
    if (!self) return nil;
    owner = anOwner;
    items = [[NSMutableArray alloc] init];
    return self;
}

- (void)dealloc
{
    [panel release]; [table release]; [items release];
    [super dealloc];
}

- (void)buildPanel
{
    NSScrollView *scroll;
    NSTableColumn *col;

    panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 480, 300)
                                       styleMask:(NSTitledWindowMask | NSClosableWindowMask | NSResizableWindowMask)
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
    [panel setReleasedWhenClosed:NO];
    [panel setHidesOnDeactivate:NO];
    [panel setMinSize:NSMakeSize(300, 160)];
    scroll = [[[NSScrollView alloc] initWithFrame:NSMakeRect(8, 48, 464, 244)] autorelease];
    col = [[[NSTableColumn alloc] initWithIdentifier:@"image"] autorelease];
    [[col headerCell] setStringValue:@"Pictures (newest last)"];
    [col setWidth:440];
    [col setEditable:NO];
    table = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 464, 100)];
    [table addTableColumn:col];
    [table setDataSource:(id)self];
    [table setAllowsMultipleSelection:NO];
    [table setTarget:self];
    [table setDoubleAction:@selector(openChosen:)];
    [scroll setHasVerticalScroller:YES];
    [scroll setDocumentView:table];
    [scroll setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [[panel contentView] addSubview:scroll];
    [[panel contentView] addSubview:ui_button(@"Close", NSMakeRect(300, 10, 80, 28), self, @selector(closeClicked:))];
    [[panel contentView] addSubview:ui_button(@"Open", NSMakeRect(392, 10, 80, 28), self, @selector(openChosen:))];
    [panel center];
}

- (void)showItems:(NSArray *)newItems title:(NSString *)title
{
    if (!panel) [self buildPanel];
    [items setArray:newItems];
    [panel setTitle:title];
    [table reloadData];
    if ([items count] > 0) [table selectRow:(int)[items count] - 1 byExtendingSelection:NO];
    [panel makeKeyAndOrderFront:nil];
}

- (void)closePanel { [panel orderOut:nil]; }
- (void)closeClicked:(id)sender { [panel orderOut:nil]; }

- (int)numberOfRowsInTableView:(NSTableView *)tv { return (int)[items count]; }

- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col row:(int)row
{
    return ((ImageItem *)[items objectAtIndex:row])->label;
}

- (void)openChosen:(id)sender
{
    int row = [table selectedRow];

    if (row < 0 || row >= (int)[items count]) {
        NSBeep();
        return;
    }
    [owner imageList:self didChooseItem:[items objectAtIndex:row]];
}

@end
