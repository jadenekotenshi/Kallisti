#import "SecretField.h"
#include <string.h>
#include <stdlib.h>
#include "rng.h"

#define SECRET_MAX 255

@implementation SecretField

- (id)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    len = 0;
    memset(buf, 0, sizeof(buf));
    return self;
}

- (void)dealloc { [self wipe]; [super dealloc]; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque { return YES; }
- (void)setEnterTarget:(id)t action:(SEL)a { enterTarget = t; enterAction = a; }
- (void)setCancelTarget:(id)t action:(SEL)a { cancelTarget = t; cancelAction = a; }
- (int)length { return len; }

- (void)wipe
{
    volatile char *p = buf;
    int i;
    for (i = 0; i < (int)sizeof(buf); i++) p[i] = 0;
    len = 0;
    [self setNeedsDisplay:YES];
}

- (char *)copySecret
{
    char *copy = (char *)malloc((size_t)len + 1);
    if (copy) memcpy(copy, buf, (size_t)len + 1);
    return copy;
}

- (char *)takeSecret
{
    char *copy = [self copySecret];
    [self wipe];
    return copy;
}

- (void)keyDown:(NSEvent *)theEvent
{
    NSString *chars = [theEvent characters];
    unichar c;
    NSData *d;
    if (!chars || [chars length] == 0) return;
    c = [chars characterAtIndex:0];
    ssh_rng_add_timing(1);
    if (c == 0x0d || c == 0x03) { if (enterTarget) [enterTarget performSelector:enterAction withObject:self]; return; }
    if (c == 0x1b)              { if (cancelTarget) [cancelTarget performSelector:cancelAction withObject:self]; return; }
    if (c == 0x09)              { [[self window] selectNextKeyView:self]; return; }
    if (c == 0x19)              { [[self window] selectPreviousKeyView:self]; return; }
    if (c == 0x7f || c == 0x08) {                        /* delete one UTF-8 character */
        while (len > 0 && (((unsigned char)buf[len - 1]) & 0xc0) == 0x80) buf[--len] = '\0';
        if (len > 0) buf[--len] = '\0';
        [self setNeedsDisplay:YES];
        return;
    }
    if (c < 0x20 || (c >= 0xf700 && c <= 0xf8ff)) return;      /* ignore control and function keys */
    d = [chars dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:YES];
    if ((int)[d length] + len <= SECRET_MAX) {
        memcpy(buf + len, [d bytes], [d length]);
        len += [d length];
        buf[len] = '\0';
        [self setNeedsDisplay:YES];
    }
}

/* Puts text into the field as if typed (used to show a remembered secret). */
- (void)setSecret:(const char *)text
{
    [self wipe];
    if (text == NULL) return;
    while (*text && len < SECRET_MAX) buf[len++] = *text++;
    buf[len] = '\0';
    [self setNeedsDisplay:YES];
}

/* Edit > Paste: whatever text is on the pasteboard, with whitespace and control characters dropped (a token
 * has none, but copying it often drags a trailing newline along). */
- (void)paste:(id)sender
{
    NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSStringPboardType];
    NSData *d;
    const unsigned char *b;
    unsigned i, n;

    if (s == nil) return;
    d = [s dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:YES];
    b = (const unsigned char *)[d bytes];
    n = [d length];
    for (i = 0; i < n && len < SECRET_MAX; i++) {
        if (b[i] <= 0x20 || b[i] == 0x7f) continue;
        buf[len++] = (char)b[i];
    }
    buf[len] = '\0';
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)rect
{
    NSRect b = [self bounds];
    int i, chars = 0, x = 6;
    BOOL active = ([[self window] firstResponder] == self);
    [[NSColor whiteColor] set];
    NSRectFill(b);
    [[NSColor blackColor] set];
    NSFrameRect(b);
    for (i = 0; i < len; i++)                            /* one bullet per character, never the text */
        if ((((unsigned char)buf[i]) & 0xc0) != 0x80) chars++;
    for (i = 0; i < chars && x + 8 < (int)b.size.width - 6; i++, x += 9)
        NSRectFill(NSMakeRect(x, NSMidY(b) - 3.0, 6.0, 6.0));
    if (active && x + 2 < (int)b.size.width)             /* insertion bar only while focused */
        NSRectFill(NSMakeRect(x, 4.0, 1.0, b.size.height - 8.0));
}

- (BOOL)becomeFirstResponder { [self setNeedsDisplay:YES]; return YES; }
- (BOOL)resignFirstResponder { [self setNeedsDisplay:YES]; return YES; }
@end
