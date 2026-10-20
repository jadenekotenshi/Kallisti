#import "Compat.h"

/* "React to a Message...": the latest messages of the channel in a list, a field for the emoji (a picture-less keyboard
 * types ":thumbsup:" or ":name:" of a custom emoji; an emoji pasted in works too) and buttons to add or take back our
 * reaction. The owner does the work, so this only collects the choice. */
@interface ReactController : NSObject
{
    id               owner;
    NSPanel         *panel;
    NSTableView     *table;
    NSTextField     *emojiField;
    NSTextField     *info;                 /* the reactions the chosen message already has */
    NSPopUpButton   *quick;
    NSMutableArray  *ids;                  /* message ids, in list order */
    NSMutableArray  *labels;
    NSMutableArray  *infos;
}
- (id)initWithOwner:(id)anOwner;
/* The messages to offer (oldest first), what to show for each, and each one's reactions as a line of text. */
- (void)showMessageIds:(NSArray *)messageIds labels:(NSArray *)messageLabels reactions:(NSArray *)reactionInfos;
- (void)closePanel;
@end

@interface NSObject (ReactOwner)
/* The person chose a message and typed an emoji; remove takes our reaction back. Returns whether it was accepted. */
- (BOOL)reactController:(ReactController *)c reactWith:(NSString *)emojiText toMessage:(NSString *)messageId remove:(BOOL)remove;
@end
