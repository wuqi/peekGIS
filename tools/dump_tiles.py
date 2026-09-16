import zstandard as zstd, struct, glob, sys, os, zlib
import numpy as np

sys.stdout.reconfigure(encoding='utf-8')

base = glob.glob(r'G:/Develope/peekGIS/build/windows/x64/release/bin/cache/bake/*')[0]
out = r'G:/Develope/peekGIS/build/l8_preview/tiles'
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


def load(lv, tx, ty):
    p = '%s/L%d.pak' % (base, lv)
    if not os.path.exists(p):
        return None, None
    d = open(p, 'rb').read()
    magic, ver, hl, res, side, sc = struct.unpack_from('<8sIIIII', d, 0)
    if tx < 0 or ty < 0 or tx >= side or ty >= side:
        return None, res
    i = ty * side + tx
    off, sz, valid = struct.unpack_from('<QIB', d, 64 + i * 16)
    if not valid:
        return None, res
    a = np.frombuffer(zstd.ZstdDecompressor().decompress(d[off + 8:off + sz], max_output_size=1 << 22),
                      dtype=np.uint8).reshape(res, res)
    return a, res


ctx, cty = 236, 246          # L9 内容最多的片
lv = 9
print('=== L%d 中心片 (%d,%d) 及 3x3 邻居 ===' % (lv, ctx, cty))
for dy in (-1, 0, 1):
    for dx in (-1, 0, 1):
        tx, ty = ctx + dx, cty + dy
        a, res = load(lv, tx, ty)
        if a is None:
            print('  (%d,%d): 无数据' % (tx, ty))
            continue
        e = int((a >= 128).sum()); f = int(((a > 0) & (a < 128)).sum()); z = int((a == 0).sum())
        fn = '%s/L%d_%d_%d.png' % (out, lv, tx, ty)
        write_png(fn, colorize(a, res))
        print('  (%d,%d) res=%d edge=%d(%.1f%%) fill=%d empty=%d -> %s' %
              (tx, ty, res, e, 100.0 * e / (res * res), f, z, os.path.basename(fn)))

print('=== L8 父片 ===')
for dy in (0, 1):
    for dx in (0, 1):
        tx, ty = ctx // 2 + dx, cty // 2 + dy
        a, res = load(8, tx, ty)
        if a is None:
            print('  (%d,%d): 无数据' % (tx, ty))
            continue
        fn = '%s/L8_%d_%d.png' % (out, tx, ty)
        write_png(fn, colorize(a, res))
        print('  (%d,%d) res=%d -> %s' % (tx, ty, res, os.path.basename(fn)))

print('输出目录: %s' % out)
