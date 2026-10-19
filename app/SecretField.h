#import "Compat.h"

/* A password-entry view (not NSSecureTextField, which may not exist on OPENSTEP 4.2).
 * Holds the text in a plain buffer that is wiped when taken or released, and draws
 * one bullet per character.  Return and Escape send configurable actions; Tab moves
 * to the next key view. */
@interface SecretField : NSView
{
    char  buf[256];
    int   len;
    id    enterTarget, cancelTarget;
    SEL   enterAction, cancelAction;
}
- (void)setEnterTarget:(id)t action:(SEL)a;
- (void)setCancelTarget:(id)t action:(SEL)a;
- (char *)takeSecret;                 /* malloc'd copy; wipes the field.  Caller wipes + frees. */
- (char *)copySecret;                 /* malloc'd copy; field keeps its text. */
- (int)length;
- (void)wipe;
- (void)setSecret:(const char *)text;   /* shows a remembered secret */
- (void)paste:(id)sender;              /* Edit > Paste */
@end
