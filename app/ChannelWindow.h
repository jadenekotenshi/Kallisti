#import "Compat.h"
#include "kms.h"
#import "ChannelPane.h"
#include "model.h"

/* A window of its own for one channel (opened by double-clicking it in the main window): just a ChannelPane
 * that never changes channel. */
@interface ChannelWindow : NSObject
{
    id          owner;
    Model      *model;
    NSWindow   *window;
    ChannelPane *pane;
    NSString   *channelId;
}
- (id)initWithOwner:(id)anOwner model:(Model *)aModel channel:(NSString *)cid;
- (NSString *)channelId;
- (void)show;
- (void)closeWindow;
- (void)modelChanged:(const ModelChange *)change;
- (void)appendNotice:(NSString *)text;
- (void)setImageSource:(id)source showInline:(BOOL)showInline;
- (void)reloadMessages;
- (NSString *)messageString;
- (void)typeText:(NSString *)text;
- (void)updateTypingAt:(KMs)now;
- (BOOL)isKeyWindow;
@end

@interface NSObject (ChannelWindowOwner)
- (void)channelWindow:(ChannelWindow *)cw userTypingInChannel:(NSString *)cid;
- (BOOL)channelWindow:(ChannelWindow *)cw didDropFiles:(NSArray *)paths caption:(NSString *)caption forChannel:(NSString *)cid;
- (void)channelWindow:(ChannelWindow *)cw didSubmitText:(NSString *)text forChannel:(NSString *)cid;
- (void)channelWindowClosed:(ChannelWindow *)cw;
@end
