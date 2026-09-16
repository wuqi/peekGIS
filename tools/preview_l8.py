import zstandard as zstd, struct, glob, sys, os, zlib
import numpy as np

sys.stdout.reconfigure(encoding='utf-8')

p = glob.glob(r'G:/Develope/peekGIS/build/windows/x64/release/bin/cache/bake/*/L8.pak')[0]
d = open(p, 'rb').read()
magic, ver, lv, res, side, sc = struct.unpack_from('<8sIIIII', d, 0)
print('pak=%s ver=%d level=%d res=%d side=%d slots=%d' % (os.path.basename(p), ver, lv, res, side, sc))

tiles = []
for i in range(sc):
    off, sz, valid = struct.unpack_from('<QIB', d, 64 + i * 16)
    if not valid:
        continue
    raw = zstd.ZstdDecompressor().decompress(d[off + 8:off + sz], max_output_size=1 << 20)
    a = np.frombuffer(raw, dtype=np.uint8)
    e = int((a >= 128).sum())
    f = int(((a > 0) & (a < 128)).sum())
    tiles.append((e, f, i % side, i // side))

tiles.sort(reverse=True)
print('valid tiles=%d  edge 最多的前6: %s' % (len(tiles), [(t[2], t[3], t[0], t[1]) for t in tiles[:6]]))


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


def colorize(a):
    rgb = np.zeros((res, res, 3), dtype=np.uint8)
    rgb[:, :] = (255, 255, 255)                       # 空 = 白
    fill = (a > 0) & (a < 128)
    rgb[fill] = (150, 200, 235)                       # 填充 = 浅蓝
    edge = a >= 128
    rgb[edge] = (210, 30, 30)                         # 边线 = 红
    return rgb


out = r'G:/Develope/peekGIS/build/l8_preview'
os.makedirs(out, exist_ok=True)

picks = tiles[:4] + tiles[len(tiles) // 2:len(tiles) // 2 + 2]
for (e, f, tx, ty) in picks:
    i = ty * side + tx
    off, sz, valid = struct.unpack_from('<QIB', d, 64 + i * 16)
    a = np.frombuffer(zstd.ZstdDecompressor().decompress(d[off + 8:off + sz], max_output_size=1 << 20),
                      dtype=np.uint8).reshape(res, res)
    fn = '%s/tile_%d_%d_e%d_f%d.png' % (out, tx, ty, e, f)
    write_png(fn, colorize(a))
    print('saved', fn)

# 2x2 拼图(取 edge 最多的4片)
mont = np.zeros((res * 2, res * 2, 3), dtype=np.uint8)
for k, (e, f, tx, ty) in enumerate(tiles[:4]):
    i = ty * side + tx
    off, sz, valid = struct.unpack_from('<QIB', d, 64 + i * 16)
    a = np.frombuffer(zstd.ZstdDecompressor().decompress(d[off + 8:off + sz], max_output_size=1 << 20),
                      dtype=np.uint8).reshape(res, res)
    r, c = divmod(k, 2)
    mont[r * res:(r + 1) * res, c * res:(c + 1) * res] = colorize(a)
write_png('%s/montage_2x2.png' % out, mont)
print('saved %s/montage_2x2.png' % out)
