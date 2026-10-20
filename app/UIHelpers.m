#import "UIHelpers.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

void SSTraceV(NSString *path, const char *fmt, va_list ap)
{
    FILE *f = fopen([path cString], "r+");      /* "r+" never creates the file: tracing is opt-in */

    if (f == NULL) return;
    fseek(f, 0L, SEEK_END);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fclose(f);                                  /* closed every time so a crash keeps earlier lines */
}

void SSTrace(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    SSTraceV([NSHomeDirectory() stringByAppendingPathComponent:@".Kallisti.trace"], fmt, ap);
    va_end(ap);
}

NSTextField *ui_label(NSString *text, NSRect frame)
{
    NSTextField *l = [[NSTextField alloc] initWithFrame:frame];
    [l setStringValue:text];
    [l setEditable:NO];
    [l setSelectable:NO];
    [l setBezeled:NO];
    [l setBordered:NO];
    [l setDrawsBackground:NO];
    return [l autorelease];
}

NSTextField *ui_wrapping_label(NSString *text, NSRect frame, BOOL selectable)
{
    NSTextField *l = ui_label(text, frame);
    [[l cell] setWraps:YES];
    [l setSelectable:selectable];
    return l;
}

NSTextField *ui_field(NSRect frame)
{
    NSTextField *f = [[NSTextField alloc] initWithFrame:frame];
    [f setEditable:YES];
    [f setBezeled:YES];
    return [f autorelease];
}

/* A check box whose image is explicitly to the left of its title, on a row of its own. */
NSButton *ui_switch(NSString *title, NSRect frame)
{
    NSButton *b = [[NSButton alloc] initWithFrame:frame];
    [b setButtonType:NSSwitchButton];
    [b setTitle:title];
    [[b cell] setImagePosition:NSImageLeft];
    [b setAlignment:NSLeftTextAlignment];
    return [b autorelease];
}

NSButton *ui_button(NSString *title, NSRect frame, id target, SEL action)
{
    NSButton *b = [[NSButton alloc] initWithFrame:frame];
    [b setTitle:title];
    [b setTarget:target];
    [b setAction:action];
    return [b autorelease];
}

static int trim_char(unichar c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

NSString *ui_trim(NSString *s)
{
    unsigned a = 0, b = [s length];
    while (a < b && trim_char([s characterAtIndex:a])) a++;
    while (b > a && trim_char([s characterAtIndex:b - 1])) b--;
    return [s substringWithRange:NSMakeRange(a, b - a)];
}

static EmojiPack *g_emoji_pack;

void ui_set_emoji_pack(EmojiPack *pack)
{
    g_emoji_pack = pack;
}

static NSString *ui_string_from_utf8_plain(const char *s);

NSString *ui_string_from_utf8(const char *s)
{
    const unsigned char *u;
    size_t n = strlen(s);

    if (g_emoji_pack != NULL) {                          /* emoji that cannot be drawn here become :names: */
        for (u = (const unsigned char *)s; *u != '\0'; u++) {
            if (*u >= 0x80) {                                /* (a keycap's U+20E3 is non-ASCII too) */
                break;
            }
        }
        if (*u != '\0') {
            size_t cap = n * 20 + 64, got;
            char *buf = (char *)malloc(cap);
            NSString *r;

            if (buf != NULL) {
                got = emoji_to_names(g_emoji_pack, s, n, buf, cap);
                (void)got;
                r = ui_string_from_utf8_plain(buf);
                free(buf);
                return r;
            }
        }
    }
    return ui_string_from_utf8_plain(s);
}

static NSString *ui_string_from_utf8_plain(const char *s)
{
    size_t n = strlen(s), i = 0, o = 0;
    unichar *out = (unichar *)malloc((n + 1) * sizeof(unichar));
    NSString *r;
    while (i < n) {
        unsigned c = (unsigned char)s[i];
        unsigned cp = c;
        int extra = 0, k;
        if (c >= 0xf0 && c < 0xf8) { cp = c & 0x07; extra = 3; }
        else if (c >= 0xe0) { cp = c & 0x0f; extra = 2; }
        else if (c >= 0xc0) { cp = c & 0x1f; extra = 1; }
        if (c >= 0x80 && extra == 0) { out[o++] = (unichar)c; i++; continue; }          /* stray byte: Latin-1 */
        if (i + extra >= n + 0 && extra) { out[o++] = (unichar)c; i++; continue; }
        for (k = 1; k <= extra; k++) {
            unsigned cc = (unsigned char)s[i + k];
            if ((cc & 0xc0) != 0x80) break;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (k <= extra) { out[o++] = (unichar)c; i++; continue; }                        /* truncated: Latin-1 */
        if (cp == 0xfe0f || cp == 0x200d || cp == 0x20e3 || (cp >= 0xe0020 && cp <= 0xe007f)) { i += (size_t)extra + 1; continue; }   /* invisible: emoji selectors and joiners */
        out[o++] = cp > 0xffff ? (unichar)'?' : (unichar)cp;
        i += (size_t)extra + 1;
    }
    r = [NSString stringWithCharacters:out length:o];
    free(out);
    return r;
}

NSData *ui_utf8_cstring(NSString *s)
{
    NSMutableData *d = [NSMutableData dataWithData:[s dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:YES]];
    [d appendBytes:"" length:1];
    return d;
}

NSString *ui_timestamp(void)
{
    return [[NSCalendarDate calendarDate] descriptionWithCalendarFormat:@"%H:%M"];
}

NSAttributedString *ui_image_attachment(NSImage *image)
{
    NSTextAttachment *a = [[[NSTextAttachment alloc] init] autorelease];

#ifdef OPENSTEP
    [a setAttachmentCell:[[[NSTextAttachmentCell alloc] initImageCell:image] autorelease]];
#else
    [a setImage:image];
#endif
    return [NSAttributedString attributedStringWithAttachment:a];
}
