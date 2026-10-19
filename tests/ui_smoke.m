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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
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

    /* a channel we cannot read: the error shows in the window, nothing breaks */
    [main selectChannelWithId:@"201"];
    {
        NSDate *end = [NSDate dateWithTimeIntervalSinceNow:2.0];

        while ([end timeIntervalSinceNow] > 0) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
    CHECK([[main messageString] rangeOfString:@"Missing Access"].location != NSNotFound);

    /* an empty line is ignored; before a channel is chosen there is a hint */
    [main typeText:@"   "];

    kill(server, SIGTERM);
    waitpid(server, &status, 0);
    printf("ui_smoke: %d passed, %d failed\n", t_pass, t_fail);
    [pool release];
    return t_fail ? 1 : 0;
}
