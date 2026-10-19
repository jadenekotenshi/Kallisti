#import "Compat.h"

/* A window showing one picture (a TIFF the client engine made from a downloaded PNG), in a scroll view in case it
 * is larger than the screen. */
@interface ImageWindow : NSObject
{
    id        owner;
    NSWindow *window;
    NSImage  *image;
}
/* Returns nil if AppKit cannot read the data. */
- (id)initWithOwner:(id)anOwner title:(NSString *)title tiffData:(NSData *)data;
- (NSSize)imageSize;
- (void)show;
- (void)closeWindow;
@end

@interface NSObject (ImageWindowOwner)
- (void)imageWindowClosed:(ImageWindow *)iw;
@end
