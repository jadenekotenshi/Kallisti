#!/usr/bin/env python3
"""Builds app/emoji.pack: the emoji pictures Kallisti draws in place of Unicode emoji its fonts cannot show.

usage: tools/gen_emoji.py [CACHE_DIR]      (needs network the first time, and Pillow: pip install pillow)

The pictures are Twemoji 15.1.0 (the jdecked fork), 72x72 PNGs fetched from jsDelivr, shrunk to 16x16 with a
premultiplied-alpha filter and stored one by one as zlib-compressed raw RGBA.  Twemoji graphics are
Copyright Twitter, Inc and other contributors, licensed CC-BY 4.0 (https://creativecommons.org/licenses/by/4.0/);
see EMOJI-LICENSE.txt.

Pack layout (all numbers big-endian):
    "KEMJ"  u8 version(1)  u8 size(16)  u16 count  u32 index_bytes
    index (sorted by key, byte-wise):  count x { u8 keylen, key (ASCII, e.g. "1f468-200d-1f469-200d-1f467"),
                                                  u32 offset (from the start of the file), u16 length }
    data: each picture's zlib stream, which inflates to size*size*4 bytes of RGBA
"""
import concurrent.futures, hashlib, io, json, os, struct, sys, urllib.request, zlib
from PIL import Image

VERSION = '15.1.0'
SIZE = 16
CACHE = sys.argv[1] if len(sys.argv) > 1 else 'build/emoji_src'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'app', 'emoji.pack')

os.makedirs(CACHE, exist_ok=True)
listing = json.load(urllib.request.urlopen('https://data.jsdelivr.com/v1/package/gh/jdecked/twemoji@%s/flat' % VERSION))
names = sorted(f['name'][len('/assets/72x72/'):-4] for f in listing['files']
               if f['name'].startswith('/assets/72x72/') and f['name'].endswith('.png'))


def fetch(key):
    path = os.path.join(CACHE, key + '.png')
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        url = 'https://cdn.jsdelivr.net/gh/jdecked/twemoji@%s/assets/72x72/%s.png' % (VERSION, key)
        with open(path, 'wb') as f:
            f.write(urllib.request.urlopen(url).read())
    return key


with concurrent.futures.ThreadPoolExecutor(16) as ex:
    list(ex.map(fetch, names))

entries = []
samples = []
for key in names:
    if not all(c in '0123456789abcdef-' for c in key):
        continue
    im = Image.open(os.path.join(CACHE, key + '.png')).convert('RGBA').convert('RGBa')           # premultiplied: no dark fringes
    im = im.resize((SIZE, SIZE), Image.LANCZOS).convert('RGBA')
    entries.append((key.encode('ascii'), zlib.compress(im.tobytes(), 9)))
    samples.append((key, hashlib.sha256(im.tobytes()).hexdigest()))
entries.sort(key=lambda e: e[0])

index_bytes = sum(1 + len(k) + 6 for k, _ in entries)
header = b'KEMJ' + struct.pack('>BBHI', 1, SIZE, len(entries), index_bytes)
offset = len(header) + index_bytes
index = b''
for k, z in entries:
    index += bytes([len(k)]) + k + struct.pack('>IH', offset, len(z))
    offset += len(z)
with open(OUT, 'wb') as f:
    f.write(header + index + b''.join(z for _, z in entries))
# every 40th picture's expected pixels, for tests/test_emoji.c (the pack must decode to exactly these)
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests', 'emoji_samples.txt'), 'w') as f:
    for i, (key, sha) in enumerate(sorted(samples)):
        if i % 40 == 0 or key in ('1f600', '2764', '1f44d-1f3fd', '1f1fa-1f1f8', '31-20e3'):
            f.write('%s %s\n' % (key, sha))
print('%d pictures, %d bytes -> %s' % (len(entries), os.path.getsize(OUT), os.path.normpath(OUT)))
