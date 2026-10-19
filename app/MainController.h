#import "Compat.h"
#include "model.h"

/* The main window: servers and channels on the left, the selected channel's messages on the right, a field
 * to type into, and a status line.  It reads the Model (a C structure owned by the client engine) directly
 * and is told what changed through -modelChanged:. */
@interface MainController : NSObject
{
    id            owner;
    Model        *model;
    NSWindow     *window;
    NSTableView  *listTable;
    NSScrollView *messageScroll;
    NSTextView   *messageText;
    NSTextField  *inputField, *statusLabel;
    NSMutableArray *rows;                 /* ListRow objects, the flattened server/channel tree */
    NSMutableSet   *collapsed;            /* ids of servers whose channels are hidden */
    NSString       *currentChannel;       /* id of the channel shown on the right, or nil */
    char            lastDay[16];          /* the day of the last message drawn, for the date separators */
    NSFont         *boldFont, *normalFont;
}
- (id)initWithOwner:(id)anOwner model:(Model *)aModel;
- (void)show;
- (void)closeWindow;
- (void)setStatus:(NSString *)text;
- (void)modelChanged:(const ModelChange *)change;
- (void)appendNotice:(NSString *)text;     /* a grey line at the end of the message view */
- (void)reloadEverything;
/* Also used by the tests, which drive the window the way a user would. */
- (NSArray *)listLabels;
- (NSString *)messageString;
- (NSString *)statusString;
- (void)selectChannelWithId:(NSString *)channelId;
- (void)typeText:(NSString *)text;
@end

@interface NSObject (MainControllerOwner)
- (void)mainController:(MainController *)mc didSelectChannel:(NSString *)channelId;
- (void)mainController:(MainController *)mc didSubmitText:(NSString *)text forChannel:(NSString *)channelId;
- (void)mainControllerWindowClosed:(MainController *)mc;
@end
