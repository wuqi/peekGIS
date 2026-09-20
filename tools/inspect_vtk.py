import struct, os, sys, collections

sys.stdout.reconfigure(encoding='utf-8')
p = sys.argv[1]
f = open(p, 'rb')
hdr = f.read(172)
ver, tileSize, pad, maxLevel, fineTileSize = struct.unpack_from('<IIIII', hdr, 8)
headerSize, slotOff, dataStart, dataEnd, fully = struct.unpack_from('<QQQQI', hdr, 108)
size = os.path.getsize(p)
print('file=%s' % os.path.basename(p))
print('  size=%d  tileSize=%d pad=%d maxLevel=%d fineTileSize=%d fully=0x%x' % (size, tileSize, pad, maxLevel, fineTileSize, fully))
print('  headerSize=%d slotOff=%d dataStart=%d dataEnd=%d dataBytes=%d' % (headerSize, slotOff, dataStart, dataEnd, dataEnd - dataStart))

off = slotOff
total = 0
entries = []
for L in range(maxLevel + 1):
    sc = 1 << (2 * L)
    f.seek(off)
    tbl = f.read(sc * 16)
    v = 0
    for i in range(sc):
        o, sz, val = struct.unpack_from('<QIB', tbl, i * 16)
        if val:
            v += 1
            total += sz
            entries.append((o, sz, L, i))
    print('  L%d slots=%d valid=%d' % (L, sc, v))
    off += sc * 16

n = len(entries)
dataBytes = dataEnd - dataStart
garbage = dataBytes - total
print('  validSlots=%d  sum(slot.size)=%d  dataBytes=%d  差额(垃圾)=%d (%.1f%%)'
      % (n, total, dataBytes, garbage, 100.0 * garbage / max(1, dataBytes)))
c = collections.Counter(o for o, _, _, _ in entries)
dup = sum(1 for k, cnt in c.items() if cnt > 1)
print('  重复 offset 的槽数=%d' % dup)
srt = sorted(entries)
gaps = sum(1 for a, b in zip(srt, srt[1:]) if a[0] + a[1] != b[0])
print('  按 offset 排序后不连续的段数=%d' % gaps)
# offset 范围
if entries:
    print('  offset 范围: %d .. %d' % (min(o for o, _, _, _ in entries), max(o + sz for o, sz, _, _ in entries)))
