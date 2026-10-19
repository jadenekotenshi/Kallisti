#!/usr/bin/env python3
"""Regenerates tests/img/*.png and tests/img/manifest.txt: PNG fixtures for test_png, one per colour type, bit depth
and interlace mode, written by an encoder of our own (every filter type in rotation, several zlib levels, split IDAT
chunks) from random samples.  The expected 8-bit RGBA of each picture is computed straight from those samples, never
by decoding, so the C decoder is checked against an independent derivation.  manifest.txt lines:
    name width height sha256-of-the-RGBA-bytes
"""
import hashlib, os, random, struct, zlib

random.seed(20261019)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'img')
os.makedirs(OUT, exist_ok=True)
manifest = []


def chunk(t, d):
    c = struct.pack('>I', len(d)) + t + d
    return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def filter_rows(rows, bpp):
    """rows: list of bytes; returns the filtered stream with a filter byte per row, filter = row index mod 5."""
    out = bytearray()
    prev = bytes(len(rows[0])) if rows else b''
    for y, row in enumerate(rows):
        f = y % 5
        out.append(f)
        for i, v in enumerate(row):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            pred = (0, a, b, (a + b) // 2, paeth(a, b, c))[f]
            out.append((v - pred) & 0xff)
        prev = row
    return bytes(out)


def pack_row(samples, depth):
    if depth == 16:
        return b''.join(struct.pack('>H', s) for s in samples)
    if depth == 8:
        return bytes(samples)
    out = bytearray()
    per = 8 // depth
    for i in range(0, len(samples), per):
        v = 0
        group = samples[i:i + per] + [0] * (per - len(samples[i:i + per]))
        for s in group:
            v = (v << depth) | s
        out.append(v)
    return bytes(out)


CH = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
ADAM = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def make(name, w, h, ctype, depth, interlace=0, trns=False, level=6, idat_split=0, smooth=False):
    ch = CH[ctype]
    maxv = (1 << depth) - 1
    npal = 0
    pal = []
    palpha = []
    if ctype == 3:
        npal = min(1 << depth, 200)
        pal = [tuple(random.randrange(256) for _ in range(3)) for _ in range(npal)]
        if trns:
            palpha = [random.randrange(256) for _ in range(random.randrange(1, npal + 1))]
    # pixels: [y][x] -> tuple of channel samples
    top = npal if ctype == 3 else maxv + 1
    if smooth:      # a gradient with a little noise: compresses like a photograph, so Huffman tables get used
        pix = [[tuple((x * (3 + c) + y * (2 + c) + random.randrange(4)) % top for c in range(ch)) for x in range(w)] for y in range(h)]
    else:
        pix = [[tuple(random.randrange(top) for _ in range(ch)) for _ in range(w)] for _ in range(h)]
    key = None
    if trns and ctype in (0, 2):
        key = pix[random.randrange(h)][random.randrange(w)]       # a colour that does occur
    # expected RGBA
    exp = bytearray()
    for y in range(h):
        for x in range(w):
            s = pix[y][x]
            sc = lambda v: v >> 8 if depth == 16 else (v * 255 // maxv if depth < 8 else v)
            if ctype == 0:
                g = sc(s[0]); a = 0 if key and s == key else 255
                exp += bytes((g, g, g, a))
            elif ctype == 2:
                a = 0 if key and s == key else 255
                exp += bytes((sc(s[0]), sc(s[1]), sc(s[2]), a))
            elif ctype == 3:
                r, g, b = pal[s[0]]
                a = palpha[s[0]] if s[0] < len(palpha) else 255
                exp += bytes((r, g, b, a))
            elif ctype == 4:
                g = sc(s[0]); exp += bytes((g, g, g, sc(s[1])))
            else:
                exp += bytes((sc(s[0]), sc(s[1]), sc(s[2]), sc(s[3])))
    bpp = max(1, ch * depth // 8)
    raw = b''
    if not interlace:
        rows = [pack_row([v for p in pix[y] for v in p], depth) for y in range(h)]
        raw = filter_rows(rows, bpp)
    else:
        for x0, y0, dx, dy in ADAM:
            ys = list(range(y0, h, dy))
            xs = list(range(x0, w, dx))
            if not ys or not xs:
                continue
            rows = [pack_row([v for x in xs for v in pix[y][x]], depth) for y in ys]
            raw += filter_rows(rows, bpp)
    comp = zlib.compress(raw, level)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, interlace))
    if ctype == 3:
        png += chunk(b'PLTE', b''.join(bytes(c) for c in pal))
        if palpha:
            png += chunk(b'tRNS', bytes(palpha))
    if key and ctype == 0:
        png += chunk(b'tRNS', struct.pack('>H', key[0]))
    if key and ctype == 2:
        png += chunk(b'tRNS', struct.pack('>HHH', *key))
    png += chunk(b'tEXt', b'Comment\x00an ancillary chunk that must be skipped')
    if idat_split:
        for i in range(0, len(comp), idat_split):
            png += chunk(b'IDAT', comp[i:i + idat_split])
    else:
        png += chunk(b'IDAT', comp)
    png += chunk(b'IEND', b'')
    open(os.path.join(OUT, name + '.png'), 'wb').write(png)
    manifest.append('%s %d %d %s' % (name, w, h, hashlib.sha256(bytes(exp)).hexdigest()))
    return png


for ctype, depths in ((0, (1, 2, 4, 8, 16)), (2, (8, 16)), (3, (1, 2, 4, 8)), (4, (8, 16)), (6, (8, 16))):
    for depth in depths:
        for il in (0, 1):
            for (w, h) in ((13, 7), (1, 1), (33, 17)):
                make('t%d_d%d_i%d_%dx%d' % (ctype, depth, il, w, h), w, h, ctype, depth, il, trns=(ctype in (0, 2, 3) and w == 13),
                     level=(0, 1, 9)[(w + h) % 3])
make('big_rgba', 320, 200, 6, 8, 0, level=9, idat_split=4000, smooth=True)
make('big_rgb_interlaced', 300, 200, 2, 8, 1, level=6, idat_split=1500, smooth=True)
make('wide_gray', 2000, 3, 0, 8, 0, smooth=True)
make('tall_palette', 3, 2000, 3, 8, 1, trns=True)
open(os.path.join(OUT, 'manifest.txt'), 'w').write('\n'.join(manifest) + '\n')
print(len(manifest), 'pictures')
