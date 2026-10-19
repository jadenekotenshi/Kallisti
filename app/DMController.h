#import "Compat.h"

/* "New Direct Message": a small panel asking for a user's id (the long number Discord shows with Developer
 * Mode on). The owner gets -dmController:didSubmitUserId: with a string of digits. */
@interface DMController : NSObject
{
    id           owner;
    NSPanel     *panel;
    NSTextField *idField;
}
- (id)initWithOwner:(id)anOwner;
- (void)showPanel;
@end

@interface NSObject (DMControllerOwner)
- (void)dmController:(DMController *)dc didSubmitUserId:(NSString *)userId;
@end
