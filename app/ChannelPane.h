#import "Compat.h"
#include "model.h"

/* The messages of one channel and a field to type into: a scrolling, coloured log above a one-line input.  Used
 * for the right-hand side of the main window (where -setChannel: follows the list's selection) and for each
 * separate channel window. Reads the Model directly and is told what changed through -modelChanged:. */
@interface ChannelPane : NSObject
{
    id            owner;
    Model        *model;
    NSView       *view;
    NSScrollView *messageScroll;
    NSTextView   *messageText;
    NSTextField  *inputField;
    NSString     *channel;               /* id of the channel shown, or nil */
    char          lastDay[16];           /* the day of the last message drawn, for the date separators */
    NSFont       *boldFont, *normalFont;
}
- (id)initWithOwner:(id)anOwner model:(Model *)aModel frame:(NSRect)frame;
- (NSView *)view;
- (NSString *)channel;
- (void)setChannel:(NSString *)channelId;     /* nil empties the log */
- (void)reload;                               /* draw the current channel again from the model */
- (void)modelChanged:(const ModelChange *)change;
- (void)appendNotice:(NSString *)text;        /* a grey line at the end of the log */
- (void)focusInput;
- (NSString *)messageString;                  /* the log's text, for the tests */
- (void)typeText:(NSString *)text;            /* as if typed and Return pressed, for the tests */
@end

@interface NSObject (ChannelPaneOwner)
- (void)channelPane:(ChannelPane *)pane didSubmitText:(NSString *)text forChannel:(NSString *)channelId;
@end
