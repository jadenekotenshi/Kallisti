#import "Compat.h"
#include "kms.h"
#import "ChannelPane.h"
#include "model.h"

/* The main window: servers and channels on the left, the selected channel on the right (a ChannelPane), and
 * a status line.  It reads the Model (a C structure owned by the client engine) directly and is told what
 * changed through -modelChanged:. */
@interface MainController : NSObject
{
    id            owner;
    Model        *model;
    NSWindow     *window;
    NSTableView  *listTable;
    ChannelPane  *pane;
    NSTextField  *statusLabel;
    NSMutableArray *rows;                 /* ListRow objects, the flattened server/channel tree */
    NSMutableSet   *collapsed;            /* ids of servers whose channels are hidden */
    id              emojiCell;            /* the list's cell, which draws emoji as pictures */
    id              imageSource;          /* supplies the emoji pictures (the AppController); not retained */
}
- (id)initWithOwner:(id)anOwner model:(Model *)aModel;
- (void)show;
- (void)closeWindow;
- (void)setStatus:(NSString *)text;
- (void)modelChanged:(const ModelChange *)change;
- (void)appendNotice:(NSString *)text;     /* a grey line at the end of the message view */
- (NSString *)currentChannel;
- (void)setImageSource:(id)source showInline:(BOOL)showInline;
- (void)reloadMessages;
- (void)selectChannelWithId:(NSString *)channelId;
/* Also used by the tests, which drive the window the way a user would. */
- (NSArray *)listLabels;
#ifndef OPENSTEP
- (void)copyCellForTest;
- (NSBitmapImageRep *)renderRow:(int)row width:(float)w;      /* the list row as drawn, for the tests */
#endif
- (NSString *)messageString;
- (NSString *)statusString;
- (void)typeText:(NSString *)text;
- (void)setDraft:(NSString *)text;
- (void)anchorOnOldest;                          /* see ChannelPane */
- (void)attachFiles:(NSArray *)paths;            /* as if dropped on the message box */
- (id)inputView;                                 /* the views that take dropped files, for the tests */
- (id)logView;
- (void)updateTypingAt:(KMs)now;                /* the "is typing" line */
- (NSString *)typingString;
- (NSString *)draft;
- (float)inputHeight;
- (void)inputReturnPressed:(BOOL)newline;
@end

@interface NSObject (MainControllerOwner)
- (void)mainController:(MainController *)mc didSelectChannel:(NSString *)channelId;
- (void)mainController:(MainController *)mc userTypingInChannel:(NSString *)channelId;
- (BOOL)mainController:(MainController *)mc didDropFiles:(NSArray *)paths caption:(NSString *)caption forChannel:(NSString *)channelId;
- (void)mainController:(MainController *)mc didSubmitText:(NSString *)text forChannel:(NSString *)channelId;
- (void)mainController:(MainController *)mc didRequestWindowForChannel:(NSString *)channelId;
- (void)mainControllerWindowClosed:(MainController *)mc;
@end
