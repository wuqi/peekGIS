import zstandard as zstd, struct, glob, sys, os, zlib
import numpy as np

sys.stdout.reconfigure(encoding='utf-8')

base = glob.glob(r'G:/Develope/peekGIS/build/windows/x64/release/bin/cache/bake/*')[0]
out = r'G:/Develope/peekGIS/build/l8_preview'


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\x00' + rgb[y].tobytes() for y in range(h))

    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def colorize(a, res):
    rgb = np.zeros((res, res, 3), dtype=np.uint8)
    rgb[:, :] = (255, 255, 255)
    rgb[(a > 0) & (a < 128)] = (150, 200, 235)
    rgb[a >= 128] = (210, 30, 30)
    return rgb


def load(lv, tx, ty):
    p = '%s/L%d.pak' % (base, lv)
    if not os.path.exists(p):
        return None, None
    d = open(p, 'rb').read()
    magic, ver, hl, res, side, sc = struct.unpack_from('<8sIIIII', d, 0)
    i = ty * side + tx
    off, sz, valid = struct.unpack_from('<QIB', d, 64 + i * 16)
    if not valid:
        return None, res
    a = np.frombuffer(zstd.ZstdDecompressor().decompress(d[off + 8:off + sz], max_output_size=1 << 22),
                      dtype=np.uint8).reshape(res, res)
    return a, res


# 找 L9 里内容最多的一片
d9 = open('%s/L9.pak' % base, 'rb').read()
magic, ver, hl, res9, side9, sc9 = struct.unpack_from('<8sIIIII', d9, 0)
best = (0, 0, 0)
for i in range(sc9):
    off, sz, valid = struct.unpack_from('<QIB', d9, 64 + i * 16)
    if not valid:
        continue
    nz = sz
    if sz > best[0]:
        best = (sz, i % side9, i // side9)
print('L9 res=%d side=%d  内容最多的片: (%d,%d) 压缩=%d B' % (res9, side9, best[1], best[2], best[0]))
tx9, ty9 = best[1], best[2]

imgs = {}
for lv in (9, 8, 7, 6, 5):
    shift = 9 - lv
    tx, ty = tx9 >> shift, ty9 >> shift
    a, res = load(lv, tx, ty)
    if a is None:
        print('L%d tile(%d,%d): 无数据' % (lv, tx, ty))
        continue
    e = int((a >= 128).sum()); f = int(((a > 0) & (a < 128)).sum()); z = int((a == 0).sum())
    print('L%d tile(%d,%d) res=%d  edge=%d(%.1f%%) fill=%d empty=%d' %
          (lv, tx, ty, res, e, 100.0 * e / (res * res), f, z))
    imgs[lv] = colorize(a, res)
    write_png('%s/L%d_%d_%d.png' % (out, lv, tx, ty), imgs[lv])

if imgs:
    res = 256
    order = [lv for lv in (5, 6, 7, 8, 9) if lv in imgs]
    mont = np.zeros((res, res * len(order), 3), dtype=np.uint8)
    for k, lv in enumerate(order):
        im = imgs[lv]
        if im.shape[0] != res:
            yi = (np.arange(res) * im.shape[0] // res)
            xi = (np.arange(res) * im.shape[1] // res)
            im = im[yi][:, xi]
        mont[:, k * res:(k + 1) * res] = im
    write_png('%s/montage_L%d_%d.png' % (out, order[0], order[-1]), mont)
    print('saved montage (左->右: %s)' % order)
