/* kprefs.h -- the few settings Kallisti remembers, in a small "key=value" text file (~/.kallisti/prefs).
 * The token is written only if the user ticked "remember"; the file is created readable by its owner alone. */
#ifndef KALLISTI_KPREFS_H
#define KALLISTI_KPREFS_H

typedef struct {
    char token[200];
    int remember;                /* keep the token in the file */
    int is_bot;                  /* 1: a bot token (the default), 0: a user-account token */
    int check_dates;             /* 1 (default): reject certificates outside their validity dates; turn off only
                                  * when the computer's clock is wrong */
} KPrefs;

void kprefs_defaults(KPrefs *p);
/* Reads the file over the defaults. A missing or unreadable file is not an error (returns 0); unknown
 * keys and malformed lines are skipped. Returns -1 only for a bad argument. */
int kprefs_load(KPrefs *p, const char *path);
/* Writes the file (token only if remember is set). Returns 0 or -1. */
int kprefs_save(const KPrefs *p, const char *path);

#endif
