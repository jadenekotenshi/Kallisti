#import "Compat.h"

/* One picture attached to a message: what to show in the list and what to fetch. */
@interface ImageItem : NSObject
{
@public
    NSString *label;
    NSString *url;
    NSString *name;
    unsigned long width, height;
}
- (id)initWithLabel:(NSString *)l url:(NSString *)u name:(NSString *)n width:(unsigned long)w height:(unsigned long)h;
@end

/* "Images in This Channel": a small list of the pictures attached to the messages the channel window shows. A
 * double-click (or Open) asks the owner to fetch one. */
@interface ImageListController : NSObject
{
    id            owner;
    NSPanel      *panel;
    NSTableView  *table;
    NSMutableArray *items;
}
- (id)initWithOwner:(id)anOwner;
- (void)showItems:(NSArray *)newItems title:(NSString *)title;
- (void)closePanel;
@end

@interface NSObject (ImageListOwner)
- (void)imageList:(ImageListController *)c didChooseItem:(ImageItem *)item;
@end
