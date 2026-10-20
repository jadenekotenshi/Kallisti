#!/usr/bin/env python3
"""Regenerates tests/gif/*.gif and tests/gif/manifest.txt: GIF fixtures for test_gif, written by an encoder of our own
(LZW with table resets, global/local palettes, interlacing, transparency, frame offsets, animation, extension blocks)
from random palette indices.  The expected first-frame RGBA on the logical screen (transparent where nothing is
drawn) is computed straight from those indices, never by decoding.  manifest.txt lines:
    name width height frames sha256-of-the-RGBA-bytes
"""
import hashlib, os, random, struct

random.seed(20261021)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gif')
os.makedirs(OUT, exist_ok=True)
manifest = []


def lzw_encode(indices, min_code):
    clear, eoi = 1 << min_code, (1 << min_code) + 1
    out = bytearray()
    bitbuf = 0
    bitcnt = 0

    def emit(code, size):
        nonlocal bitbuf, bitcnt
        bitbuf |= code << bitcnt
        bitcnt += size
        while bitcnt >= 8:
            out.append(bitbuf & 255)
            bitbuf >>= 8
            bitcnt -= 8

    size = min_code + 1
    table = {}
    nxt = eoi + 1
    emit(clear, size)
    cur = None
    for v in indices:
        if cur is None:
            cur = (v,)
            continue
        key = cur + (v,)
        if key in table or len(key) == 1:
            if key in table:
                cur = key
                continue
        if key in table:
            cur = key
            continue
        # emit the code for cur
        code = cur[0] if len(cur) == 1 else table[cur]
        emit(code, size)
        if nxt < 4096:
            table[key] = nxt
            nxt += 1
            if nxt - 1 == (1 << size) and size < 12:
                size += 1
        else:
            emit(clear, size)
            table = {}
            nxt = eoi + 1
            size = min_code + 1
        cur = (v,)
    if cur is not None:
        emit(cur[0] if len(cur) == 1 else table[cur], size)
    emit(eoi, size)
    if bitcnt:
        out.append(bitbuf & 255)
    return bytes(out)


def blocks(data):
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


def interlaced_rows(h):
    return list(range(0, h, 8)) + list(range(4, h, 8)) + list(range(2, h, 4)) + list(range(1, h, 2))


def make(name, sw, sh, frames, gct=None, trans=None, extras=False, version=b'GIF89a', force_min_code=None):
    """frames: list of dicts(x, y, w, h, pal (local) or None, interlace, idx rows (list of lists))"""
    data = bytearray(version)
    packed = 0
    gct_bytes = b''
    if gct:
        bits = max(1, (len(gct) - 1).bit_length())
        size = 1 << bits
        gct_pad = gct + [(0, 0, 0)] * (size - len(gct))
        packed = 0x80 | ((bits - 1) << 4) | (bits - 1)
        gct_bytes = b''.join(bytes(c) for c in gct_pad)
    data += struct.pack('<HHBBB', sw, sh, packed, 0, 0) + gct_bytes
    first = frames[0]
    for fi, fr in enumerate(frames):
        if extras and fi == 0:
            data += b'\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00'            # application extension
            data += b'\x21\xfe' + blocks(b'a comment that must be skipped')
            data += b'\x21\x01\x0c' + bytes(12) + blocks(b'plain text')        # plain-text extension
        if trans is not None or fi > 0:
            t = trans if trans is not None else 0
            data += b'\x21\xf9\x04' + bytes([0x01 if trans is not None else 0]) + struct.pack('<H', 10) + bytes([t if trans is not None else 0]) + b'\x00'
        pal = fr.get('pal')
        local = b''
        lflag = 0
        palette = gct
        if pal:
            bits = max(1, (len(pal) - 1).bit_length())
            size = 1 << bits
            lflag = 0x80 | (bits - 1)
            local = b''.join(bytes(c) for c in (pal + [(0, 0, 0)] * (size - len(pal))))
            palette = pal
        if fr.get('interlace'):
            lflag |= 0x40
        data += b'\x2c' + struct.pack('<HHHHB', fr['x'], fr['y'], fr['w'], fr['h'], lflag) + local
        rows = fr['rows']
        order = interlaced_rows(fr['h']) if fr.get('interlace') else list(range(fr['h']))
        flat = [v for r in order for v in rows[r]]            # rows in the order they are stored
        ncol = len(palette)
        min_code = force_min_code or max(2, (ncol - 1).bit_length())
        data += bytes([min_code]) + blocks(lzw_encode(flat, min_code))
    data += b'\x3b'
    # expected: first frame on the screen
    pal0 = first.get('pal') or gct
    exp = bytearray(sw * sh * 4)
    for y in range(first['h']):
        for x in range(first['w']):
            ox, oy = first['x'] + x, first['y'] + y
            if ox >= sw or oy >= sh:
                continue
            v = first['rows'][y][x]
            if trans is not None and v == trans:
                continue
            r, g, b = pal0[v] if v < len(pal0) else (0, 0, 0)
            o = (oy * sw + ox) * 4
            exp[o:o + 4] = bytes((r, g, b, 255))
    open(os.path.join(OUT, name + '.gif'), 'wb').write(bytes(data))
    manifest.append('%s %d %d %d %s' % (name, sw, sh, len(frames), hashlib.sha256(bytes(exp)).hexdigest()))


def rand_pal(n):
    return [tuple(random.randrange(256) for _ in range(3)) for _ in range(n)]


def rand_rows(w, h, n, smooth=False):
    if smooth:
        return [[(x // 3 + y // 2 + random.randrange(2)) % n for x in range(w)] for y in range(h)]
    return [[random.randrange(n) for _ in range(w)] for _ in range(h)]


def frame(w, h, n, x=0, y=0, pal=None, interlace=False, smooth=False):
    return dict(x=x, y=y, w=w, h=h, pal=pal, interlace=interlace, rows=rand_rows(w, h, n, smooth))


for n in (2, 3, 4, 7, 16, 100, 256):
    for il in (False, True):
        for (w, h) in ((13, 7), (1, 1), (33, 17)):
            make('p%d_i%d_%dx%d' % (n, int(il), w, h), w, h, [frame(w, h, n, interlace=il)], gct=rand_pal(n))
# transparency, local palettes only, offsets, clipping, screen size zero, extension blocks, 87a
make('trans', 20, 10, [frame(20, 10, 8)], gct=rand_pal(8), trans=3)
make('trans_zero', 9, 9, [frame(9, 9, 4)], gct=rand_pal(4), trans=0)
make('local_only', 16, 12, [frame(16, 12, 16, pal=rand_pal(16))])
make('local_over_global', 16, 12, [frame(16, 12, 4, pal=rand_pal(4))], gct=rand_pal(4))
make('offset', 30, 20, [frame(10, 8, 8, x=7, y=5)], gct=rand_pal(8), trans=1)
make('clipped', 12, 10, [frame(20, 16, 8, x=4, y=3)], gct=rand_pal(8))
make('screen_zero', 0, 0, [frame(11, 6, 4, x=2, y=1)], gct=rand_pal(4))
make('extras', 24, 14, [frame(24, 14, 16)], gct=rand_pal(16), extras=True)
make('gif87a', 15, 15, [frame(15, 15, 8)], gct=rand_pal(8), version=b'GIF87a')
make('anim', 25, 15, [frame(25, 15, 16, smooth=True), frame(25, 15, 16, smooth=True), frame(10, 10, 16, x=3, y=3, pal=rand_pal(16))],
     gct=rand_pal(16), trans=0, extras=True)
make('bad_index', 8, 8, [frame(8, 8, 8)], gct=rand_pal(4), force_min_code=3)   # indices up to 7 with a 4-colour palette
# long data: the LZW table fills up and is reset (several times), 255-byte block boundaries everywhere
make('big', 320, 200, [frame(320, 200, 256, smooth=True)], gct=rand_pal(256))
make('big_interlaced', 200, 150, [frame(200, 150, 64, interlace=True, smooth=True)], gct=rand_pal(64))
make('noise', 100, 100, [frame(100, 100, 256)], gct=rand_pal(256))
open(os.path.join(OUT, 'manifest.txt'), 'w').write('\n'.join(manifest) + '\n')
print(len(manifest), 'pictures')
