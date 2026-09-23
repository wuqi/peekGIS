import struct, sys
import zstandard as zstd
from PIL import Image, ImageDraw
sys.stdout.reconfigure(encoding='utf-8')

# 用法: vt_preview.py <cache.vtk> <L> <tx> <ty> [out.png] [size]
path = sys.argv[1]
L = int(sys.argv[2]); TX = int(sys.argv[3]); TY = int(sys.argv[4])
out = sys.argv[5] if len(sys.argv) > 5 else 'build/tmp/tile.png'
SIZE = int(sys.argv[6]) if len(sys.argv) > 6 else 1600

d = open(path, 'rb').read()
maxLevel, = struct.unpack_from('<I', d, 20)
tileSize, pad, = struct.unpack_from('<II', d, 12)
fineTileSize, = struct.unpack_from('<I', d, 24)
hsize, slotOff, dataStart, dataEnd, fully = struct.unpack_from('<QQQQI', d, 108)
tsz = fineTileSize if L == maxLevel else tileSize

off = slotOff
for lv in range(L):
    off += (1 << (2 * lv)) * 16
sc = 1 << (2 * L)
n = 1 << L
idx = TY * n + TX           # slotIndex = (ty << L) | tx
o, sz, val = struct.unpack_from('<QIB', d, off + idx * 16)
if not val:
    print('no tile'); sys.exit(1)
raw = zstd.ZstdDecompressor().decompress(d[o:o + sz])

ox, oy = struct.unpack_from('<dd', raw, 0)
epsg, vc, rc = struct.unpack_from('<iII', raw, 16)
p = 28; xs = []; ys = []; px = py = 0
def unzig(z): return (z >> 1) ^ -(z & 1)
for _ in range(vc):
    s = 0; v = 0
    while True:
        b = raw[p]; p += 1; v |= (b & 0x7f) << s
        if not (b & 0x80): break
        s += 7
    px += unzig(v)
    s = 0; v = 0
    while True:
        b = raw[p]; p += 1; v |= (b & 0x7f) << s
        if not (b & 0x80): break
        s += 7
    py += unzig(v)
    xs.append(px); ys.append(py)
rings = []
for _ in range(rc):
    typ, hole, fl, fv, rvc, pg = struct.unpack_from('<BBHIII', raw, p); p += 16
    rings.append((typ, hole, fl, fv, rvc, pg))

lo = -22; hi = tsz + 22
scale = SIZE / (hi - lo)
def P(x, y): return ((x - lo) * scale, (hi - y) * scale)

img = Image.new('RGB', (SIZE, SIZE), (255, 255, 255))
dr = ImageDraw.Draw(img)
for (typ, hole, fl, fv, rvc, pg) in rings:          # 孔填白
    if typ != 1 or not hole or rvc < 3: continue
    dr.polygon([P(xs[fv+i], ys[fv+i]) for i in range(rvc)], fill=(255, 255, 255))
for (typ, hole, fl, fv, rvc, pg) in rings:          # 外环填色
    if typ != 1 or hole or rvc < 3: continue
    dr.polygon([P(xs[fv+i], ys[fv+i]) for i in range(rvc)], fill=(205, 222, 240))
for (typ, hole, fl, fv, rvc, pg) in rings:          # 边
    if typ != 1 or rvc < 2: continue
    pts = [P(xs[fv+i], ys[fv+i]) for i in range(rvc)]
    dr.line(pts + [pts[0]], fill=(180, 0, 0), width=1)
for (typ, hole, fl, fv, rvc, pg) in rings:          # 线层(RING_LINE=0)
    if typ != 0 or rvc < 2: continue
    pts = [P(xs[fv+i], ys[fv+i]) for i in range(rvc)]
    dr.line(pts, fill=(0, 0, 255), width=2)
for (typ, hole, fl, fv, rvc, pg) in rings:          # 点
    if typ != 2: continue
    for i in range(rvc):
        x, y = P(xs[fv+i], ys[fv+i])
        dr.ellipse([x-2, y-2, x+2, y+2], fill=(0, 128, 0))
img.save(out)
print('saved %s  L%d(%d,%d) tsz=%d rings=%d verts=%d' % (out, L, TX, TY, tsz, len(rings), vc))
