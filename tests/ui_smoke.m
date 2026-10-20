/*
 * ui_smoke.m -- the real AppController, MainController and client engine (REST, gateway, model) driven the way
 * a user would, against the scripted fake Discord of tests/client_smoke_server.py, on the development Mac
 * with the modern Cocoa.  It proves the glue (event/change plumbing, list building, message rendering,
 * unread counters, typing) -- not OPENSTEP's own AppKit, which only real hardware can show.
 *
 * usage: ui_smoke SERVER_SCRIPT PYTHON PORT
 */
#import <Cocoa/Cocoa.h>
#import "AppController.h"
#import "SecretField.h"
#import "ChannelWindow.h"
#import "ImageWindow.h"
#import "ImageListController.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "rng.h"

static int t_pass, t_fail;
#define CHECK(cond) do { if (cond) t_pass++; else { t_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* Runs the run loop until the block-free condition holds. */
static BOOL waitFor(BOOL (*cond)(AppController *), AppController *app, double seconds)
{
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];

    while ([end timeIntervalSinceNow] > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        if (cond(app)) return YES;
    }
    return NO;
}

static BOOL isConnected(AppController *app)
{
    return [[[app mainController] statusString] isEqualToString:@"Connected as Kallisti-bot"];
}

static BOOL hasHistory(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@"oldest"].location != NSNotFound;
}

static BOOL hasHello(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@"hello from the test"].location != NSNotFound;
}

static BOOL hasWindowMsg(AppController *app)
{
    ChannelWindow *w = [app windowForChannel:@"200"];

    return w != nil && [[w messageString] rangeOfString:@"from the window"].location != NSNotFound;
}

static BOOL hasPicMessage(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@"[image: cat.png 300x200]"].location != NSNotFound;
}

static BOOL hasFileMessage(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@"[attachment: notes.bin]"].location != NSNotFound;
}

static BOOL hasMyReaction(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@"1*]"].location != NSNotFound;
}

static BOOL hasNoMyReaction(AppController *app)
{
    NSString *s = [[app mainController] messageString];

    return [s rangeOfString:@"reactui"].location != NSNotFound && [s rangeOfString:@"1*]"].location == NSNotFound;
}

/* how many pixels of a rendered row are clearly coloured (text is black, white and grey; emoji are not) */
static int colourfulPixels(NSBitmapImageRep *rep)
{
    int x, y, n = 0;

    for (y = 0; y < [rep pixelsHigh]; y++) {
        for (x = 0; x < [rep pixelsWide]; x++) {
            NSColor *c = [[rep colorAtX:x y:y] colorUsingColorSpaceName:NSCalibratedRGBColorSpace];
            float hi = [c redComponent], lo = [c redComponent];

            if ([c greenComponent] > hi) hi = [c greenComponent];
            if ([c blueComponent] > hi) hi = [c blueComponent];
            if ([c greenComponent] < lo) lo = [c greenComponent];
            if ([c blueComponent] < lo) lo = [c blueComponent];
            if (hi - lo > 0.25) n++;
        }
    }
    return n;
}

/* pixels in columns [from, to) that differ from the background pixel at the row's top left: text and pictures */
static int inkInColumns(NSBitmapImageRep *rep, int from, int to)
{
    int x, y, n = 0;
    NSColor *bg = [[rep colorAtX:0 y:0] colorUsingColorSpaceName:NSCalibratedRGBColorSpace];

    for (y = 0; y < [rep pixelsHigh]; y++) {
        for (x = from; x < to && x < [rep pixelsWide]; x++) {
            NSColor *c = [[rep colorAtX:x y:y] colorUsingColorSpaceName:NSCalibratedRGBColorSpace];

            if (fabs([c redComponent] - [bg redComponent]) + fabs([c greenComponent] - [bg greenComponent]) +
                fabs([c blueComponent] - [bg blueComponent]) > 0.5) n++;
        }
    }
    return n;
}

/* a stand-in for what AppKit hands a drop target */
@interface FakeDrag : NSObject
{
    NSPasteboard *pb;
}
- (id)initWithPaths:(NSArray *)paths;
@end

@implementation FakeDrag
- (id)initWithPaths:(NSArray *)paths
{
    self = [super init];
    pb = [[NSPasteboard pasteboardWithUniqueName] retain];
    [pb declareTypes:[NSArray arrayWithObject:NSFilenamesPboardType] owner:nil];
    [pb setPropertyList:paths forType:NSFilenamesPboardType];
    return self;
}
- (NSPasteboard *)draggingPasteboard { return pb; }
@end

static BOOL hasDropped(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@"caption here [dropped.bin:70000:ok]"].location != NSNotFound;
}

static BOOL hasDroppedTwice(AppController *app)
{
    NSString *s = [[app mainController] messageString];
    NSRange first = [s rangeOfString:@"[dropped.bin:70000:ok]"];

    return first.location != NSNotFound && [s rangeOfString:@"[dropped.bin:70000:ok]" options:0
                                                       range:NSMakeRange(NSMaxRange(first), [s length] - NSMaxRange(first))].location != NSNotFound;
}

static BOOL isTyping(AppController *app)
{
    return [[[app mainController] typingString] isEqualToString:@"Typist is typing..."];
}

static BOOL notTyping(AppController *app)
{
    return [[[app mainController] typingString] length] == 0;
}

static long fileSize(NSString *path)
{
    struct stat st;

    return stat([path cString], &st) == 0 ? (long)st.st_size : -1L;
}

static BOOL savedFileArrived(AppController *app)
{
    return fileSize(@"/tmp/kallisti_ui_smoke_notes.bin") == 70000L;
}

/* the (last) line of `text` that contains `needle`, without its newline */
static NSString *lineWith(NSString *text, NSString *needle)
{
    NSRange hit = [text rangeOfString:needle options:NSBackwardsSearch];
    NSRange before, after;
    unsigned from = 0, to = [text length];

    if (hit.location == NSNotFound) return @"";
    before = [text rangeOfString:@"\n" options:NSBackwardsSearch range:NSMakeRange(0, hit.location)];
    if (before.location != NSNotFound) from = before.location + 1;
    after = [text rangeOfString:@"\n" options:0 range:NSMakeRange(hit.location, [text length] - hit.location)];
    if (after.location != NSNotFound) to = after.location;
    return [text substringWithRange:NSMakeRange(from, to - from)];
}

static int typingPosts(AppController *app, int *out)
{
    NSString *line;
    int n = -1;

    [[app mainController] typeText:@"typingposts"];
    {
        NSDate *end = [NSDate dateWithTimeIntervalSinceNow:10.0];

        while ([end timeIntervalSinceNow] > 0) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
            line = lineWith([[app mainController] messageString], @"typing posts: ");
            if (line != nil && sscanf([line cString] + [line rangeOfString:@"typing posts: "].location + 14, "%d", &n) == 1 && n != *out) {
                *out = n;
                return 1;
            }
        }
    }
    return 0;
}


static BOOL hasUnicodeEmojiMessage(AppController *app)
{
    NSString *t = [[app mainController] messageString];

    return [t rangeOfString:@"hi "].location != NSNotFound && [t rangeOfString:@" bye"].location != NSNotFound;
}

static BOOL hasShortcodeEcho(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@" here"].location != NSNotFound &&
           [[[app mainController] messageString] rangeOfString:@"sent "].location != NSNotFound;
}

static BOOL hasFancyAuthor(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@"Bob: from a fancy name"].location != NSNotFound;
}

static BOOL hasEmojiChannel(AppController *app)
{
    NSArray *labels = [[app mainController] listLabels];
    unsigned i;

    for (i = 0; i < [labels count]; i++) {
        if ([[labels objectAtIndex:i] rangeOfString:@":video_game:-games"].location != NSNotFound) return YES;
    }
    return NO;
}

static BOOL hasGifMessage(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@"[image: dance.gif 25x15]"].location != NSNotFound;
}

static BOOL gifMoved(AppController *app)
{
    return [[app imageWindows] count] == 1 && [[[app imageWindows] objectAtIndex:0] currentFrame] != 0;
}

static BOOL hasEmojiText(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@"cool :blob: and :party: yes"].location != NSNotFound;
}

static BOOL hasEmojiPictures(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@":blob:"].location == NSNotFound &&
           [[[app mainController] messageString] rangeOfString:@":party:"].location == NSNotFound;
}

static BOOL hasTwoAttachmentChars(AppController *app)
{
    unichar obj = 0xFFFC;
    NSString *t = [[app mainController] messageString];
    NSRange first = [t rangeOfString:[NSString stringWithCharacters:&obj length:1]];

    unsigned n = 0, pos = 0;
    NSRange r;
    NSString *marker = [NSString stringWithCharacters:&obj length:1];

    (void)first;
    while ((r = [t rangeOfString:marker options:0 range:NSMakeRange(pos, [t length] - pos)]).location != NSNotFound) {
        n++;
        pos = r.location + 1;
    }
    return n >= 4;
}

static BOOL hasEmbedText(AppController *app)
{
    NSString *t = [[app mainController] messageString];

    return [t rangeOfString:@"    | Example\n"].location != NSNotFound && [t rangeOfString:@"    | Cool Page\n"].location != NSNotFound &&
           [t rangeOfString:@"    | about cats\n"].location != NSNotFound && [t rangeOfString:@"    [image: cat.png 300x200]\n"].location != NSNotFound;
}

static BOOL hasUnfurled(AppController *app)
{
    return [[[app mainController] messageString] rangeOfString:@"    | Unfurled Title\n"].location != NSNotFound;
}

static BOOL hasAttachmentChar(AppController *app)
{
    unichar obj = 0xFFFC;

    return [[[app mainController] messageString] rangeOfString:[NSString stringWithCharacters:&obj length:1]].location != NSNotFound;
}

static BOOL hasImageWindow(AppController *app)
{
    return [[app imageWindows] count] == 1;
}

static BOOL hasDM(AppController *app)
{
    NSArray *labels = [[app mainController] listLabels];

    return [labels count] > 1 && [[labels objectAtIndex:0] isEqualToString:@"- Direct Messages"];
}

static BOOL hasDMSelected(AppController *app)
{
    return [[[app mainController] currentChannel] isEqualToString:@"900"];
}

static unsigned countOf(NSString *hay, NSString *needle)
{
    unsigned n = 0, pos = 0;
    NSRange r;

    while ((r = [hay rangeOfString:needle options:0 range:NSMakeRange(pos, [hay length] - pos)]).location != NSNotFound) {
        n++;
        pos = r.location + r.length;
    }
    return n;
}

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    AppController *app;
    MainController *main;
    pid_t server;
    NSArray *labels;
    NSString *text;
    int status;

    if (argc != 4) {
        fprintf(stderr, "usage: ui_smoke SERVER PYTHON PORT\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    putenv((char *)"http_proxy="); putenv((char *)"https_proxy="); putenv((char *)"all_proxy=");     /* the shell's proxies stay out of it */
    putenv((char *)"HTTP_PROXY="); putenv((char *)"HTTPS_PROXY="); putenv((char *)"ALL_PROXY=");
    ssh_rng_seed_system();
    ssh_rng_add("ui smoke", 8, 256);
    server = fork();
    if (server == 0) {
        execlp(argv[2], argv[2], argv[1], argv[3], (char *)NULL);
        _exit(127);
    }
    usleep(1500000);

    [NSApplication sharedApplication];
    app = [[AppController alloc] init];
    [app buildMenu];
    [app useRestHost:@"localhost" port:atoi(argv[3]) tls:NO
          gatewayURL:[NSString stringWithFormat:@"ws://localhost:%d/?v=10&encoding=json", atoi(argv[3]) + 1]];
    [app signInWithToken:@"goodtoken" isBot:YES];
    main = [app mainController];
    CHECK(main != nil);
    CHECK(waitFor(isConnected, app, 15.0));

    /* the token field takes pasted text (whitespace dropped) from the menu route and from Command-V */
    {
        NSWindow *w = [[[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 200, 60) styleMask:NSTitledWindowMask
                                                     backing:NSBackingStoreBuffered defer:NO] autorelease];
        SecretField *f = [[[SecretField alloc] initWithFrame:NSMakeRect(10, 10, 180, 24)] autorelease];
        NSEvent *e;
        char *got;

        [[w contentView] addSubview:f];
        [w makeFirstResponder:f];
        [[NSPasteboard generalPasteboard] declareTypes:[NSArray arrayWithObject:NSStringPboardType] owner:nil];
        [[NSPasteboard generalPasteboard] setString:@"  abc.DEF-123\n" forType:NSStringPboardType];
        [f paste:nil];
        CHECK([f length] == 11);
        got = [f copySecret];
        CHECK(got != NULL && strcmp(got, "abc.DEF-123") == 0);
        free(got);
        [f wipe];
        e = [NSEvent keyEventWithType:NSKeyDown location:NSMakePoint(0, 0) modifierFlags:NSCommandKeyMask timestamp:0
                         windowNumber:[w windowNumber] context:nil characters:@"v" charactersIgnoringModifiers:@"v"
                            isARepeat:NO keyCode:9];
        CHECK([f performKeyEquivalent:e] == YES && [f length] == 11);
        [f wipe];
    }

    /* the list: the server, its category, and its two text channels with the unread counts from the gateway */
    labels = [main listLabels];
    CHECK([labels count] == 4);
    CHECK([labels count] == 4 && [[labels objectAtIndex:0] isEqualToString:@"- Test Guild"]);
    CHECK([labels count] == 4 && [[labels objectAtIndex:1] isEqualToString:@"  TEXT"]);
    CHECK([labels count] == 4 && [[labels objectAtIndex:2] isEqualToString:@"    # general (1!)"]);
    CHECK([labels count] == 4 && [[labels objectAtIndex:3] isEqualToString:@"    # secret"]);

    /* opening a channel loads its history, shows it in order, and clears the unread mark */
    [main selectChannelWithId:@"200"];
    CHECK(waitFor(hasHistory, app, 10.0));
    text = [main messageString];
    CHECK([text rangeOfString:@"oldest"].location < [text rangeOfString:@"middle"].location);
    CHECK([text rangeOfString:@"middle"].location < [text rangeOfString:@"newest @Kallisti-bot"].location);
    CHECK([text rangeOfString:@"newest @Kallisti-bot"].location < [text rangeOfString:@"hi @Kallisti-bot from #secret"].location);
    CHECK([text rangeOfString:@"someone"].location != NSNotFound && [text rangeOfString:@"[10:00]"].location != NSNotFound ||
          [text rangeOfString:@"someone: oldest"].location != NSNotFound);
    CHECK([text rangeOfString:@"---- 2026-10-19 ----"].location != NSNotFound || [text rangeOfString:@"---- 2026-10-18 ----"].location != NSNotFound ||
          [text rangeOfString:@"---- 2026-10-20 ----"].location != NSNotFound);
    labels = [main listLabels];
    CHECK([labels count] == 4 && [[labels objectAtIndex:2] isEqualToString:@"    # general"]);

    /* typing a line sends it and it appears once (POST result and gateway echo are merged) */
    [main typeText:@"hello from the test"];
    CHECK(waitFor(hasHello, app, 10.0));
    {
        NSDate *end = [NSDate dateWithTimeIntervalSinceNow:0.6];

        while ([end timeIntervalSinceNow] > 0) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
    CHECK(countOf([main messageString], @"hello from the test") == 1);
    CHECK([[main messageString] rangeOfString:@"Kallisti-bot: hello from the test"].location != NSNotFound);

    /* a channel window of its own shows the same messages and takes input too */
    {
        ChannelWindow *w;

        [app openWindowForChannel:@"200"];
        w = [app windowForChannel:@"200"];
        CHECK(w != nil && [app windowForChannel:@"999"] == nil);
        CHECK([[w messageString] rangeOfString:@"hello from the test"].location != NSNotFound);
        [app openWindowForChannel:@"200"];                       /* a second request reuses the window */
        CHECK([[app windowForChannel:@"200"] isEqual:w]);
        [w typeText:@"from the window"];
        CHECK(waitFor(hasWindowMsg, app, 10.0));
        CHECK(countOf([w messageString], @"from the window") == 1 && countOf([main messageString], @"from the window") == 1);
        [w closeWindow];
        [app channelWindowClosed:w];
        CHECK([app windowForChannel:@"200"] == nil);
    }

    /* a picture attachment: listed, fetched on request, shown in a window of its own */
    [main typeText:@"pic"];
    CHECK(waitFor(hasPicMessage, app, 10.0));
    {
        NSArray *items = [app imageItemsForCurrentChannel];
        ImageItem *item;

        CHECK([items count] == 1);
        if ([items count] == 1) {
            item = [items objectAtIndex:0];
            CHECK([item->name isEqualToString:@"cat.png"] && item->width == 300 && item->height == 200);
            CHECK([item->label rangeOfString:@"someone: cat.png  (300x200)"].location != NSNotFound);
            [app openImageItem:item];
            CHECK(waitFor(hasImageWindow, app, 20.0));
            if ([[app imageWindows] count] == 1) {
                NSSize sz = [[[app imageWindows] objectAtIndex:0] imageSize];

                CHECK((int)sz.width == 300 && (int)sz.height == 200);
                [[[app imageWindows] objectAtIndex:0] closeWindow];
                [app imageWindowClosed:[[app imageWindows] objectAtIndex:0]];
                CHECK([[app imageWindows] count] == 0);
            }
        }
    }

    /* typing in the box tells Discord, but not more than once every 8 seconds */
    {
        int before = -1, after = -1;

        CHECK(typingPosts(app, &before));
        [main setDraft:@"hel"];
        [main setDraft:@"hello"];
        [main setDraft:@"hello there"];
        after = before;
        CHECK(typingPosts(app, &after));
        CHECK(after == before + 1);       /* one post for all that typing (and none for the next message's first keystroke) */
        [main setDraft:@""];
    }

    /* the server rows of the list are bold, the rest are not */
    {
        NSArray *labels = [main listLabels];
        NSTextFieldCell *cell = [[[NSTextFieldCell alloc] init] autorelease];
        unsigned i;
        int bold = 0, plain = 0;

        for (i = 0; i < [labels count]; i++) {
            BOOL isGuild = [[labels objectAtIndex:i] hasPrefix:@"- "] || [[labels objectAtIndex:i] hasPrefix:@"+ "];

            [(id)main tableView:nil willDisplayCell:cell forTableColumn:nil row:(int)i];
            if ([[[cell font] fontName] rangeOfString:@"Bold"].location != NSNotFound) {
                bold++;
                CHECK(isGuild);
            } else {
                plain++;
                CHECK(!isGuild);
            }
        }
        CHECK(bold >= 1 && plain >= 1);
    }

    /* the log can be saved as text */
    {
        NSString *path = @"/tmp/kallisti_ui_log.txt";
        NSString *text;

        remove([path cString]);
        CHECK([app saveLogOfChannel:@"200" toPath:path]);
        text = [NSString stringWithContentsOfFile:path];
        CHECK(text != nil && [text rangeOfString:@"hello from the test"].location != NSNotFound && [text hasPrefix:@"#general - Test Guild\n"]);
        CHECK([text rangeOfString:@"[image: cat.png 300x200]"].location != NSNotFound);
        CHECK(![app saveLogOfChannel:@"200" toPath:@"/nonexistent_dir_zz/x.txt"]);
        CHECK(![app saveLogOfChannel:@"999" toPath:path]);
        remove([path cString]);
    }

    /* files dropped on the input go with what is typed there as the caption; on the log they go alone; a folder is refused */
    {
        NSString *dir = @"/tmp/kallisti_ui_drop";
        NSString *file = @"/tmp/kallisti_ui_drop/dropped.bin";
        unsigned char *bytes = (unsigned char *)malloc(70000);
        FILE *f;
        int k;
        FakeDrag *d;

        mkdir("/tmp/kallisti_ui_drop", 0755);
        for (k = 0; k < 70000; k++) bytes[k] = (unsigned char)((k * 7 + k / 256) & 255);
        f = fopen([file cString], "wb");
        CHECK(f != NULL);
        if (f != NULL) {
            fwrite(bytes, 1, 70000, f);
            fclose(f);
        }
        free(bytes);
        [main setDraft:@"caption here"];
        d = [[[FakeDrag alloc] initWithPaths:[NSArray arrayWithObject:file]] autorelease];
        CHECK([[main inputView] draggingEntered:(id)d] == NSDragOperationCopy);
        CHECK([[main inputView] prepareForDragOperation:(id)d]);
        CHECK([[main inputView] performDragOperation:(id)d]);
        CHECK([[main draft] length] == 0);                           /* the caption went with the file */
        CHECK(waitFor(hasDropped, app, 20.0));
        d = [[[FakeDrag alloc] initWithPaths:[NSArray arrayWithObject:file]] autorelease];
        CHECK([[main logView] performDragOperation:(id)d]);
        CHECK(waitFor(hasDroppedTwice, app, 20.0));
        d = [[[FakeDrag alloc] initWithPaths:[NSArray arrayWithObject:dir]] autorelease];
        CHECK(![[main inputView] performDragOperation:(id)d]);
        CHECK([[[app mainController] messageString] rangeOfString:@"kallisti_ui_drop is a folder"].location != NSNotFound);
        d = [[[FakeDrag alloc] initWithPaths:[NSArray arrayWithObject:@"/tmp/kallisti_ui_drop/nonexistent"]] autorelease];
        CHECK(![[main inputView] performDragOperation:(id)d]);
        CHECK([[[app mainController] messageString] rangeOfString:@"Cannot find nonexistent"].location != NSNotFound);
        remove([file cString]);
        rmdir("/tmp/kallisti_ui_drop");
    }

    /* who is typing shows on the line above the input, and a message from them ends it */
    CHECK([[[app mainController] typingString] length] == 0);
    [main typeText:@"typing"];
    CHECK(waitFor(isTyping, app, 10.0));
    [main typeText:@"typingdone"];
    CHECK(waitFor(notTyping, app, 10.0));

    /* an attachment that is not a picture is listed, and saved from Discord's own address (not the media proxy's) */
    [main typeText:@"savefile"];
    CHECK(waitFor(hasFileMessage, app, 10.0));
    {
        NSArray *all = [app attachmentItemsForCurrentChannel];
        ImageItem *file = nil;
        unsigned i;

        for (i = 0; i < [all count]; i++) {
            if ([((ImageItem *)[all objectAtIndex:i])->name isEqualToString:@"notes.bin"]) file = [all objectAtIndex:i];
        }
        CHECK(file != nil && !file->isImage && [file->label rangeOfString:@"someone: notes.bin  (file)"].location != NSNotFound);
        if (file != nil) {
            remove("/tmp/kallisti_ui_smoke_notes.bin");
            [app saveItem:file toPath:@"/tmp/kallisti_ui_smoke_notes.bin"];
            CHECK(waitFor(savedFileArrived, app, 20.0));
            {
                FILE *f = fopen("/tmp/kallisti_ui_smoke_notes.bin", "rb");
                int ok = f != NULL, k;

                for (k = 0; ok && k < 70000; k++) {
                    if (fgetc(f) != ((k * 7 + k / 256) & 255)) ok = 0;
                }
                if (f != NULL) fclose(f);
                CHECK(ok);
            }
            remove("/tmp/kallisti_ui_smoke_notes.bin");
        }
    }

    /* reacting: the emoji is worked out here (a pasted custom emoji tag), the REST call goes out, the gateway's event
     * puts it under the message */
    [main typeText:@"reactui"];
    CHECK(waitFor(hasNoMyReaction, app, 10.0));
    {
        Channel *ch = model_find_channel(kc_model([app client]), "200");
        NSString *mid = nil;
        int i;

        for (i = 0; ch != NULL && i < ch->nmsgs; i++) {
            if (strcmp(ch->msgs[i].text, "reactui") == 0) mid = [NSString stringWithCString:ch->msgs[i].id];
        }
        CHECK(mid != nil);
        CHECK(![app reactToMessage:mid withText:@"not an emoji" remove:NO]);
        CHECK(![app reactToMessage:mid withText:@"" remove:NO]);
        if (mid != nil) {
            CHECK([app reactToMessage:mid withText:@"<:blobcat:555>" remove:NO]);
            CHECK(waitFor(hasMyReaction, app, 10.0));
            CHECK([[[app mainController] messageString] rangeOfString:@"blobcat"].location != NSNotFound);
            CHECK([app reactToMessage:mid withText:@"<:blobcat:555>" remove:YES]);
            CHECK(waitFor(hasNoMyReaction, app, 10.0));
        }
    }

    /* an animated GIF opens in a window that plays it: frames in order, wrapping round, on a timer */
    [main typeText:@"gifpic"];
    CHECK(waitFor(hasGifMessage, app, 10.0));
    {
        NSArray *items = [app imageItemsForCurrentChannel];
        ImageItem *gif = nil;
        unsigned i;

        for (i = 0; i < [items count]; i++) {
            if ([((ImageItem *)[items objectAtIndex:i])->name isEqualToString:@"dance.gif"]) gif = [items objectAtIndex:i];
        }
        CHECK(gif != nil && gif->gif && !gif->jpeg);
        if (gif != nil) {
            [app openImageItem:gif];
            CHECK(waitFor(hasImageWindow, app, 20.0));
            if ([[app imageWindows] count] == 1) {
                ImageWindow *w = [[app imageWindows] objectAtIndex:0];

                CHECK([w isAnimated] && (int)[w imageSize].width == 25 && (int)[w imageSize].height == 15);
                CHECK(waitFor(gifMoved, app, 5.0));                 /* the timer moved it on by itself (100 ms a frame) */
                [w advance];
                [w advance];
                [w advance];                                         /* 3 frames: wraps round */
                CHECK([w currentFrame] >= 0 && [w currentFrame] < 3);
                [w closeWindow];
                [app imageWindowClosed:w];
            }
        }
    }

    /* custom emoji: ":name:" text, and with the inline switch the pictures stand in for them */
    [app setEmojiBase:[NSString stringWithFormat:@"http://localhost:%d/img/emoji/", atoi(argv[3])]];
    [main typeText:@"emoji"];
    CHECK(waitFor(hasEmojiText, app, 10.0));
    {
        unichar obj = 0xFFFC;
        NSString *marker = [NSString stringWithCharacters:&obj length:1];
        NSUInteger before = countOf([main messageString], marker);

        [app setInlineImages:YES];
        CHECK(waitFor(hasEmojiPictures, app, 20.0));
        CHECK([[main messageString] rangeOfString:@"cool "].location != NSNotFound && [[main messageString] rangeOfString:@" yes"].location != NSNotFound);
        CHECK([[main messageString] rangeOfString:@":blob:"].location == NSNotFound && [[main messageString] rangeOfString:@":party:"].location == NSNotFound);
        CHECK(countOf([main messageString], marker) >= before + 2);
        [app setInlineImages:NO];
        CHECK([[main messageString] rangeOfString:@"cool :blob: and :party: yes"].location != NSNotFound);
    }

    /* embeds: the preview's lines under the message, its picture in the list, and a late preview (MESSAGE_UPDATE) */
    [main typeText:@"embed"];
    CHECK(waitFor(hasEmbedText, app, 10.0));
    {
        NSArray *items = [app imageItemsForCurrentChannel];
        unsigned i, embedItems = 0;

        for (i = 0; i < [items count]; i++) {
            if ([((ImageItem *)[items objectAtIndex:i])->label rangeOfString:@"cat.png"].location != NSNotFound) embedItems++;
        }
        CHECK(embedItems == 2);                                    /* the attachment of "pic", and the embed's thumbnail */
    }
    [main typeText:@"unfurl"];
    CHECK(waitFor(hasUnfurled, app, 10.0));

    /* the experimental inline pictures: switched on, the thumbnail is fetched and the log shows an attachment */
    {
        unichar obj = 0xFFFC;
        NSString *marker = [NSString stringWithCharacters:&obj length:1];

        CHECK([[main messageString] rangeOfString:marker].location == NSNotFound);
        [app setInlineImages:YES];
        CHECK([app inlineImagesEnabled]);
        CHECK(waitFor(hasTwoAttachmentChars, app, 20.0));              /* the picture's thumbnail and the embed's ... */
        CHECK(countOf([main messageString], marker) == 4);             /* ... and the two custom emoji above */
        [app setInlineImages:NO];
        CHECK([[main messageString] rangeOfString:marker].location == NSNotFound);
    }

    /* Unicode emoji: pictures (no network involved), invisible selectors dropped; as text they become '?' */
    {
        unichar obj = 0xFFFC;
        NSString *marker = [NSString stringWithCharacters:&obj length:1];
        NSString *t;
        NSRange r;

        CHECK([app loadEmojiPackFromPath:@"app/emoji.pack"] && [app emojiPicturesEnabled]);
        [main typeText:@"uemoji"];
        CHECK(waitFor(hasUnicodeEmojiMessage, app, 10.0));
        t = lineWith([main messageString], @" bye");
        r = [t rangeOfString:@"hi "];
        CHECK(r.location != NSNotFound);
        t = [t substringFromIndex:r.location];
        {
            NSString *want = [NSString stringWithFormat:@"hi %@ %@ %@ bye", marker, marker, marker];

            CHECK([t isEqualToString:want]);                                                                    /* three pictures, nothing else */
        }
        [app setEmojiPictures:NO];
        t = lineWith([main messageString], @" bye");
        r = [t rangeOfString:@"hi "];
        t = [t substringFromIndex:r.location];
        CHECK([t isEqualToString:@"hi :grinning: :heart: :thumbsup::skin-tone-3: bye"]);                  /* names, not question marks */
        CHECK([t rangeOfString:marker].location == NSNotFound);
        CHECK(![app emojiPicturesEnabled]);
        [app setEmojiPictures:YES];

        /* typed shortcodes become emoji on the way out: the echo comes back as a picture */
        [main typeText:@"sent :thumbsup: and :nosuchname: here"];
        CHECK(waitFor(hasShortcodeEcho, app, 10.0));
        t = lineWith([main messageString], @" here");
        CHECK([t rangeOfString:marker].location != NSNotFound && [t rangeOfString:@":nosuchname:"].location != NSNotFound &&
              [t rangeOfString:@":thumbsup:"].location == NSNotFound);

        /* an emoji in a name: drawn in the log, spelt out where no picture can go */
        [main typeText:@"emojiauthor"];
        CHECK(waitFor(hasFancyAuthor, app, 10.0));
        t = lineWith([main messageString], @"Bob: from a fancy name");
        CHECK([t rangeOfString:marker].location != NSNotFound && [t rangeOfString:@"?"].location == NSNotFound);
        [app setEmojiPictures:NO];
        t = lineWith([main messageString], @"Bob: from a fancy name");
        CHECK([t rangeOfString:@":grinning: Bob: from a fancy name"].location != NSNotFound);
        [app setEmojiPictures:YES];
        [main typeText:@"newchan"];
        CHECK(waitFor(hasEmojiChannel, app, 10.0));
        /* selecting channels whose names hold emoji (a joined sequence too) rebuilds the list and must not crash */
        [(id)main copyCellForTest];                         /* a copied cell must hold its own reference to its text */
        [(id)main copyCellForTest];
        [main selectChannelWithId:@"230"];
        CHECK([[main currentChannel] isEqualToString:@"230"]);
        [main selectChannelWithId:@"231"];
        CHECK([[main currentChannel] isEqualToString:@"231"]);
        [main selectChannelWithId:@"200"];

        /* the same emoji in a channel's name is drawn in the list: coloured pixels where the text alone has only greys */
        {
            NSArray *ls = [main listLabels];
            int row = -1, colourful = 0, colourfulOff = 0;
            unsigned li;

            for (li = 0; li < [ls count]; li++) {
                if ([[ls objectAtIndex:li] rangeOfString:@":video_game:-games"].location != NSNotFound) row = (int)li;
            }
            CHECK(row >= 0);
            if (row >= 0) {
                colourful = colourfulPixels([main renderRow:row width:200.0]);
                [app setEmojiPictures:NO];
                colourfulOff = colourfulPixels([main renderRow:row width:200.0]);
                [app setEmojiPictures:YES];
                CHECK(colourful >= 3);                                  /* (a game controller is mostly grey: a few coloured buttons) */
                CHECK(colourfulOff == 0);
                CHECK(inkInColumns([main renderRow:row width:200.0], 56, 200) > 20);           /* the name after the picture is there too */
                CHECK(inkInColumns([main renderRow:row width:200.0], 4, 30) > 5);             /* and the "# " before it */
                CHECK(colourfulPixels([main renderRow:0 width:200.0]) == 0);       /* a row with no emoji stays plain */
            }
        }
    }

    /* a DM from a stranger lands in Direct Messages, first in the list, as a mention */
    [main typeText:@"dmme"];
    CHECK(waitFor(hasDM, app, 10.0));
    labels = [main listLabels];
    CHECK([[labels objectAtIndex:0] isEqualToString:@"- Direct Messages"] && [[labels objectAtIndex:1] isEqualToString:@"  @eight (1!)"]);
    /* starting a DM by user id selects the new conversation */
    [app dmController:nil didSubmitUserId:@"7"];
    CHECK(waitFor(hasDMSelected, app, 10.0));

    /* a channel we cannot read: the error shows in the window, nothing breaks */
    [main selectChannelWithId:@"201"];
    {
        NSDate *end = [NSDate dateWithTimeIntervalSinceNow:2.0];

        while ([end timeIntervalSinceNow] > 0) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
    CHECK([[main messageString] rangeOfString:@"Missing Access"].location != NSNotFound);

    /* an empty line is ignored; before a channel is chosen there is a hint */
    [main typeText:@"   "];

    /* the input grows with its text, up to a limit, and shrinks back once the line is sent */
    {
        float one = [main inputHeight];
        float mid, cap;

        [main setDraft:@"a\nb\nc\nd"];
        mid = [main inputHeight];
        CHECK(mid > one);
        [main setDraft:@"1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30"];
        cap = [main inputHeight];
        CHECK(cap > mid);
        CHECK(cap <= 110.0);
        [main setDraft:@""];
        CHECK([main inputHeight] == one);
        [main setDraft:@"x"];
        [main inputReturnPressed:YES];
        CHECK([[main draft] isEqualToString:@"x\n"]);
        [main setDraft:@""];
    }

    kill(server, SIGTERM);
    waitpid(server, &status, 0);
    printf("ui_smoke: %d passed, %d failed\n", t_pass, t_fail);
    [pool release];
    return t_fail ? 1 : 0;
}
