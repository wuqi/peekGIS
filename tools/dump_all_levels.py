import zstandard as zstd, struct, glob, sys, os, zlib
import numpy as np

sys.stdout.reconfigure(encoding='utf-8')

base = glob.glob(r'G:/Develope/peekGIS/build/windows/x64/release/bin/cache/bake/*')[0]
out = r'G:/Develope/peekGIS/build/l8_preview/levels'
os.makedirs(out, exist_ok=True)


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


paks = {}
for lv in range(0, 10):
    p = '%s/L%d.pak' % (base, lv)
    if os.path.exists(p):
        paks[lv] = open(p, 'rb').read()


def load(lv, tx, ty):
    d = paks[lv]
    magic, ver, hl, res, side, sc = struct.unpack_from('<8sIIIII', d, 0)
    if tx < 0 or ty < 0 or tx >= side or ty >= side:
        return None, res
    off, sz, valid = struct.unpack_from('<QIB', d, 64 + (ty * side + tx) * 16)
    if not valid:
        return None, res
    a = np.frombuffer(zstd.ZstdDecompressor().decompress(d[off + 8:off + sz], max_output_size=1 << 22),
                      dtype=np.uint8).reshape(res, res)
    return a, res


anchor9 = (236, 246)
for lv in range(0, 10):
    if lv not in paks:
        print('L%d: 无 pak' % lv)
        continue
    magic, ver, hl, res, side, sc = struct.unpack_from('<8sIIIII', paks[lv], 0)
    shift = 9 - lv
    ax, ay = anchor9[0] >> shift, anchor9[1] >> shift
    print('=== L%d  res=%d side=%d  锚点父片 (%d,%d) 及右侧/下侧邻居 ===' % (lv, res, side, ax, ay))
    got = 0
    for (tx, ty) in [(ax, ay), (ax + 1, ay), (ax, ay + 1), (ax + 1, ay + 1)]:
        a, r = load(lv, tx, ty)
        if a is None:
            print('  (%d,%d): 无数据' % (tx, ty))
            continue
        e = int((a >= 128).sum()); f = int(((a > 0) & (a < 128)).sum()); z = int((a == 0).sum())
        fn = '%s/L%d_%d_%d.png' % (out, lv, tx, ty)
        write_png(fn, colorize(a, r))
        print('  (%d,%d) edge=%d(%.1f%%) fill=%d empty=%d -> %s' %
              (tx, ty, e, 100.0 * e / (r * r), f, z, os.path.basename(fn)))
        got += 1
    if got == 0:
        print('  (该层锚点附近无数据)')

print('输出目录: %s' % out)
