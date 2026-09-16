import zstandard as zstd, struct, glob, sys, os, zlib
import numpy as np

sys.stdout.reconfigure(encoding='utf-8')

base = glob.glob(r'G:/Develope/peekGIS/build/windows/x64/release/bin/cache/bake/*')[0]
out = r'G:/Develope/peekGIS/build/l8_preview'
os.makedirs(out, exist_ok=True)

# 以 L8 的 (142,145) 为起点, 逐层取父片 -> 同一块地、不同层
tx8, ty8 = 142, 145


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


imgs = {}
for lv in range(0, 9):
    p = '%s/L%d.pak' % (base, lv)
    if not os.path.exists(p):
        print('L%d: 无文件' % lv)
        continue
    d = open(p, 'rb').read()
    magic, ver, hl, res, side, sc = struct.unpack_from('<8sIIIII', d, 0)
    shift = 8 - lv
    tx, ty = tx8 >> shift, ty8 >> shift
    i = ty * side + tx
    off, sz, valid = struct.unpack_from('<QIB', d, 64 + i * 16)
    if not valid:
        print('L%d: tile(%d,%d) 无数据' % (lv, tx, ty))
        continue
    a = np.frombuffer(zstd.ZstdDecompressor().decompress(d[off + 8:off + sz], max_output_size=1 << 20),
                      dtype=np.uint8).reshape(res, res)
    e = int((a >= 128).sum())
    f = int(((a > 0) & (a < 128)).sum())
    z = int((a == 0).sum())
    print('L%d tile(%d,%d) res=%d  edge=%d(%.1f%%) fill=%d empty=%d' %
          (lv, tx, ty, res, e, 100.0 * e / (res * res), f, z))
    imgs[lv] = colorize(a, res)
    write_png('%s/L%d_tile_%d_%d.png' % (out, lv, tx, ty), imgs[lv])

# 拼图: 4 列 x 2 行 (L0..L7)
if imgs:
    res = 512
    mont = np.zeros((res * 2, res * 4, 3), dtype=np.uint8)
    for lv in range(0, 8):
        if lv not in imgs:
            continue
        im = imgs[lv]
        if im.shape[0] != res:
            yi = (np.arange(res) * im.shape[0] // res)
            xi = (np.arange(res) * im.shape[1] // res)
            im = im[yi][:, xi]
        r, c = divmod(lv, 4)
        mont[r * res:(r + 1) * res, c * res:(c + 1) * res] = im
    write_png('%s/montage_L0_L7.png' % out, mont)
    print('saved %s/montage_L0_L7.png' % out)
