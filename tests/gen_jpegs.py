#!/usr/bin/env python3
"""Regenerates tests/jpg/*.jpg and tests/jpg/manifest.txt: JPEG fixtures for test_jpeg, written by libjpeg (through
Pillow): baseline and progressive, 4:4:4 / 4:2:2 / 4:2:0, grey, restart intervals, odd sizes, low and high quality.
The expected pixels are libjpeg's own decode of the same file (also through Pillow), as the sha256 of the RGBA bytes
(alpha 255), so the C decoder is held to exact agreement with libjpeg.  Needs: pip install pillow.
manifest.txt lines:  name width height sha256
"""
import hashlib, io, math, os, random
from PIL import Image

random.seed(20261020)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'jpg')
os.makedirs(OUT, exist_ok=True)
manifest = []


def picture(w, h, mode='RGB'):
    """Something photograph-like: smooth colour fields, an edge or two, and noise."""
    im = Image.new('RGB', (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            v = 127 + 100 * math.sin(x / 11.0) * math.cos(y / 13.0)
            edge = 70 if (x * 3 + y * 2) % 97 < 8 else 0
            px[x, y] = (max(0, min(255, int(v) + edge + random.randrange(-10, 10))),
                        max(0, min(255, int(v / 2) + 40 + random.randrange(-10, 10))),
                        max(0, min(255, 255 - int(v) - edge + random.randrange(-10, 10))))
    return im.convert('L') if mode == 'L' else im


def make(name, w, h, mode='RGB', **kw):
    im = picture(w, h, mode)
    buf = io.BytesIO()
    im.save(buf, 'JPEG', **kw)
    data = buf.getvalue()
    open(os.path.join(OUT, name + '.jpg'), 'wb').write(data)
    rgba = Image.open(io.BytesIO(data)).convert('RGBA')
    manifest.append('%s %d %d %s' % (name, w, h, hashlib.sha256(rgba.tobytes()).hexdigest()))


for sub, sname in ((0, '444'), (1, '422'), (2, '420')):
    for prog in (False, True):
        for (w, h) in ((64, 48), (37, 23), (1, 1), (17, 33), (200, 150)):
            make('c_%s_%s_%dx%d' % (sname, 'p' if prog else 'b', w, h), w, h, quality=(30, 75, 95)[(w + h) % 3],
                 subsampling=sub, progressive=prog)
for prog in (False, True):
    for (w, h) in ((64, 48), (13, 7), (1, 9)):
        make('g_%s_%dx%d' % ('p' if prog else 'b', w, h), w, h, 'L', quality=85, progressive=prog)
# restart intervals, with and without subsampling
make('rst_444_b', 96, 64, quality=80, subsampling=0, restart_marker_blocks=3)
make('rst_420_b', 96, 64, quality=80, subsampling=2, restart_marker_rows=1)
make('rst_420_p', 96, 64, quality=80, subsampling=2, progressive=True, restart_marker_blocks=5)
# optimised Huffman tables, extreme quality, and a bigger "photo"
make('opt_420', 130, 90, quality=90, subsampling=2, optimize=True)
make('q1_420', 80, 60, quality=1, subsampling=2)
make('q100_444', 80, 60, quality=100, subsampling=0)
make('big_420', 800, 600, quality=85, subsampling=2)
make('big_420_p', 640, 480, quality=85, subsampling=2, progressive=True, optimize=True)
open(os.path.join(OUT, 'manifest.txt'), 'w').write('\n'.join(manifest) + '\n')
print(len(manifest), 'pictures')
