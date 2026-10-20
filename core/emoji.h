/* emoji.h -- Unicode emoji as pictures.  OPENSTEP's fonts have no emoji, so Kallisti draws them: app/emoji.pack holds a
 * 16x16 picture for each emoji Twemoji has (tools/gen_emoji.py makes it), and emoji_next() finds the emoji in a piece of
 * UTF-8 text -- including skin-tone, flag, keycap, tag and zero-width-joiner sequences -- and names the picture that
 * shows it.  The keys are Twemoji's file names: lower-case hexadecimal code points joined by '-', with the variation
 * selector U+FE0F dropped unless the sequence has a ZWJ ("1f600", "2764", "1f44d-1f3fd", "31-20e3",
 * "1f468-200d-1f469-200d-1f467"). */
#ifndef KALLISTI_EMOJI_H
#define KALLISTI_EMOJI_H

#include <stddef.h>

#define EMOJI_KEY_MAX 96

typedef struct EmojiPack EmojiPack;

/* Opens a pack (reads its index; the pictures are read from the file when asked for). NULL if it cannot be read or is
 * not a pack. */
EmojiPack *emoji_pack_open(const char *path);
void emoji_pack_close(EmojiPack *p);
int emoji_pack_count(const EmojiPack *p);
int emoji_pack_size(const EmojiPack *p);                      /* the pictures are size x size */
int emoji_pack_has(const EmojiPack *p, const char *key);
/* Key number i (0..count-1) in sorted order, or NULL. */
const char *emoji_pack_key(const EmojiPack *p, int i, size_t *len);
/* The picture as size*size*4 bytes of RGBA (out must hold that much). Returns 0, or -1 if there is no such picture or
 * the file is damaged. */
int emoji_pack_rgba(EmojiPack *p, const char *key, unsigned char *out);
/* The picture laid over white as an uncompressed TIFF (malloc'd; NULL on failure). */
unsigned char *emoji_pack_tiff(EmojiPack *p, const char *key, size_t *len);

/* Finds the first emoji at or after byte `from` of utf8[0..len). Returns 1 with its byte range [*start, *end) and the key
 * of its picture, or 0 if there is none. Plain digits, #, *, and the symbols that fonts do have (c) (r) (tm) and
 * the arrows are emoji only when followed by U+FE0F (or, for digits, in a keycap). */
int emoji_next(const EmojiPack *p, const char *utf8, size_t len, size_t from, size_t *start, size_t *end, char key[EMOJI_KEY_MAX]);

#endif
