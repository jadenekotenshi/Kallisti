#import "Compat.h"
#include "model.h"
#include "emoji.h"

/* The messages of one channel and a field to type into: a scrolling, coloured log above an input that grows to fit what is typed.  Used
 * for the right-hand side of the main window (where -setChannel: follows the list's selection) and for each
 * separate channel window. Reads the Model directly and is told what changed through -modelChanged:. */
@interface ChannelPane : NSObject
{
    id            owner;
    Model        *model;
    NSView       *view;
    NSScrollView *messageScroll;
    NSTextView   *messageText;
    NSScrollView *inputScroll;           /* around inputText; grows with what is typed, up to a limit */
    NSTextView   *inputText;
    float         inputHeight;           /* current height of the input; 0 until laid out */
    NSString     *channel;               /* id of the channel shown, or nil */
    char          lastDay[16];           /* the day of the last message drawn, for the date separators */
    NSFont       *boldFont, *normalFont;
    id            imageSource;           /* supplies thumbnails for inline pictures; not retained */
    BOOL          inlineImages;
}
- (id)initWithOwner:(id)anOwner model:(Model *)aModel frame:(NSRect)frame;
- (NSView *)view;
- (NSString *)channel;
- (void)setChannel:(NSString *)channelId;     /* nil empties the log */
- (void)reload;                               /* draw the current channel again from the model */
- (void)modelChanged:(const ModelChange *)change;
- (void)appendNotice:(NSString *)text;        /* a grey line at the end of the log */
- (void)focusInput;
/* Pictures drawn inside the log (experimental): `source` answers -inlineImageForURL:width:height:jpeg:. */
- (void)setImageSource:(id)source showInline:(BOOL)showInline;
- (NSString *)messageString;                  /* the log's text, for the tests */
- (void)layoutSubviews;                       /* place the log and the input; the pane's view calls this on resizing */
- (void)inputReturnPressed:(BOOL)newline;     /* Return sends; Shift- or Alt-Return (newline YES) adds a line */
- (void)setDraft:(NSString *)text;            /* put text in the input without sending, for the tests */
- (NSString *)draft;
- (float)inputHeight;
- (void)typeText:(NSString *)text;            /* as if typed and Return pressed, for the tests */
@end

@interface NSObject (ChannelPaneOwner)
- (void)channelPane:(ChannelPane *)pane didSubmitText:(NSString *)text forChannel:(NSString *)channelId;
@end

@interface NSObject (ChannelPaneImageSource)
/* The thumbnail for an attachment address if it is on hand; otherwise nil, and the source starts fetching it and
 * will have the pane draw itself again when it arrives. */
- (NSImage *)inlineImageForURL:(NSString *)url width:(int)w height:(int)h jpeg:(BOOL)isJpeg;
/* Unicode emoji as pictures: the pack to look them up in (NULL if switched off or there is none), and the picture for a
 * key (nil if that cannot be made). */
- (EmojiPack *)emojiPack;
- (NSImage *)unicodeEmojiImageForKey:(NSString *)key;
/* The picture of a custom emoji, likewise (nil until it has been fetched). */
- (NSImage *)inlineEmojiImageForID:(NSString *)emojiId;
@end
