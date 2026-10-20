#import "Compat.h"

/* A window showing one picture (a TIFF the client engine made from a downloaded PNG), in a scroll view in case it
 * is larger than the screen. */
@interface ImageWindow : NSObject
{
    id        owner;
    NSWindow *window;
    NSImage  *image;
    NSImageView *imageView;
    NSMutableArray *frames;       /* NSImage per frame, for an animation (else nil) */
    NSMutableArray *delays;       /* NSNumber: milliseconds each frame is shown */
    int       current;
    NSTimer  *timer;
    double    nextChange;         /* NSDate reference time at which the next frame is due */
}
/* Returns nil if AppKit cannot read the data. */
- (id)initWithOwner:(id)anOwner title:(NSString *)title tiffData:(NSData *)data;
/* An animation: `tiffs` is an array of NSData (one TIFF per frame), `delaysMs` of NSNumber. nil if AppKit cannot read it. */
- (id)initWithOwner:(id)anOwner title:(NSString *)title frames:(NSArray *)tiffs delays:(NSArray *)delaysMs;
- (BOOL)isAnimated;
- (int)currentFrame;
- (void)advance;                  /* show the next frame now (the timer does this on schedule) */
- (NSSize)imageSize;
- (void)show;
- (void)closeWindow;
@end

@interface NSObject (ImageWindowOwner)
- (void)imageWindowClosed:(ImageWindow *)iw;
@end
