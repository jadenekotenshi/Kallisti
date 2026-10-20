#import "Compat.h"

/* One picture attached to a message: what to show in the list and what to fetch. */
@interface ImageItem : NSObject
{
@public
    NSString *label;
    NSString *url;
    NSString *saveURL;           /* where Save fetches the file from: Discord's own address, not the media proxy's */
    NSString *name;
    BOOL isImage;                /* a picture the viewer can open; anything else can only be saved */
    unsigned long width, height;
    BOOL jpeg;                   /* the original is a JPEG */
    BOOL gif;                    /* ... or a GIF, which may be an animation */
}
- (id)initWithLabel:(NSString *)l url:(NSString *)u saveURL:(NSString *)s name:(NSString *)n width:(unsigned long)w
             height:(unsigned long)h jpeg:(BOOL)isJpeg gif:(BOOL)isGif image:(BOOL)image;
@end

/* "Attachments in This Channel": a small list of the files attached to the messages the channel window shows. A
 * double-click (or Open) asks the owner to show a picture; Save asks it to download any of them. */
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
- (void)imageList:(ImageListController *)c didSaveItem:(ImageItem *)item;
@end
