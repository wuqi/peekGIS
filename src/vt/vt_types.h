#pragma once
// 矢量瓦片(v2)基础类型与常量。纯渲染用途: 只保证"画出来对", 不维护拓扑/属性。
// 设计见 docs/矢量缓存方案.md。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace peekg::vt {

constexpr int TILE_SIZE = 512;             // 每瓦片净区格数(粗层)
constexpr int TILE_PAD  = 10;              // 扩边格数(粗层)
constexpr int FINE_TILE_SIZE = 1024;       // 最深层格网(2x 精度); 最深层跳过合并, 与粗层互不影响
constexpr int FINE_TILE_PAD  = 20;         // 最深层扩边格数(按比例)
constexpr int GRID_MIN  = -TILE_PAD;       // -10
constexpr int GRID_MAX  = TILE_SIZE + TILE_PAD;  // 522
// 按层取格网大小/扩边: 只有最深层用 FINE_*
inline int tileSizeAt(int level, int maxLevel) { return level >= maxLevel ? FINE_TILE_SIZE : TILE_SIZE; }
inline int tilePadAt(int level, int maxLevel) { return level >= maxLevel ? FINE_TILE_PAD : TILE_PAD; }
// 隔层构建: 每 step 层保留一层(从 L0 起, 即保留 L%step==0), 并始终保留最深层。
// step<=1 表示每层都建。渲染端 chooseVtLevel 会从目标层回退到最近的已建层。
inline bool levelKept(int level, int maxLevel, int step) {
    if (step <= 1) return true;
    if (level == maxLevel) return true;
    return (level % step) == 0;
}

constexpr uint32_t VT_VERSION = 2;   // 2: 顶点改 delta+zigzag+varint 编码
constexpr char VT_MAGIC[8] = {'P','E','E','K','V','T','0','1'};

enum RingType : uint8_t {
    RING_LINE  = 0,
    RING_FACE  = 1,
    RING_POINT = 2,
};

// 一段环(或点)。面: 外环/孔环同 polyGroup; hole 标记孔。
struct VtRing {
    uint8_t  type = RING_LINE;
    uint8_t  hole = 0;
    uint16_t _pad = 0;
    uint32_t firstVertex = 0;   // 顶点下标(顶点=xy对)
    uint32_t vertexCount = 0;
    uint32_t polyGroup = 0;     // 面: 同一原始多边形的外环+孔共享; 其他=0
};

// 单瓦片(内存形态与序列化形态一致)。顶点为瓦片内 int16 相对格坐标, 净区 0..512, 扩边 -10..522。
struct VtTile {
    double originX = 0, originY = 0;   // 瓦片左下角(显示 CRS)
    int32_t epsg = 0;
    std::vector<int16_t> verts;        // x,y 交替(缓存路径; 量化格坐标)
    std::vector<VtRing> rings;
    // 直读专用: 世界坐标 float(CRS 见 epsg)。非空时 buildTileGeometry 走此分支,
    // 不量化/不压共线/不改退化面(直读=源几何, firstVertex/vertexCount 指向 fverts)。
    // 永不序列化(直读瓦片不落盘), serializeTile 假定它为空。
    std::vector<float> fverts;
    // 覆盖度(存在性)栅格: 粗层里"小于一格"的要素(小水面/小地块)没有形状可言, 逐个画成
    // 1x1 面会在屏幕上变成百万级噪点, 丢掉又会看着像数据缺失。改为按格记存在性:
    // 每条目 = (净区格号 << 8) | 计数(饱和 255), 渲染时按屏幕像素聚合成密度标记。
    // 与层号无关: 任何层、任何数据, 只要要素在该层小于一格就走这里。
    std::vector<uint32_t> cover;

    uint32_t vertexCount() const { return (uint32_t)(verts.size() / 2); }
    void clear() { verts.clear(); rings.clear(); fverts.clear(); cover.clear(); }
    bool empty() const { return rings.empty() && cover.empty(); }
};

#pragma pack(push, 1)
// 固定文件头(写文件头区; dataEnd/fullyBuiltLevels 在构建期更新)。
struct VtFileHeader {
    char     magic[8];        // VT_MAGIC
    uint32_t version;
    uint32_t tileSize;
    uint32_t pad;
    uint32_t maxLevel;
    uint32_t fineTileSize;             // 最深层格网(2x); 旧缓存无此字段 -> headerSize 不符 -> 自动失效重建
    int32_t  srcEpsg;
    int32_t  dstEpsg;
    double   minx, miny, maxx, maxy;   // 显示 CRS 数据范围
    double   originX, originY;         // 全局网格原点(=L0 瓦片左下角)
    double   tileW0;                   // L0 瓦片边长(方形)
    uint64_t srcHash;                  // 源身份(路径+mtime+size 的 hash)
    int64_t  buildTime;
    uint64_t headerSize;               // = sizeof(VtFileHeader)
    uint64_t dataStart;                // 数据段起始
    uint64_t dataEnd;                  // 追加水位(下一个可写位置)
    uint64_t slotAreaOffset;           // 稀疏槽区起始(文件绝对偏移, finalize 写)
    uint64_t rowDirOffset;             // 行目录起始(紧接槽区, finalize 写)
    uint32_t fullyBuiltLevels;         // 已完整构建的层 bitmask
    uint32_t rowDirCount;              // 行目录条目数(每 (level,ty) 一条)
    char     srcName[28];              // 源文件名(不含路径, 便于缓存管理显示; 旧缓存为全0)
};

// 行目录条目: 描述某层某行(ty)实际用到的槽区间。
// 瓦片数据有强空间局部性 —— 同一 ty 行内只有一段 tx 有数据, 所以只给
// [txLo, txLo+count) 分配槽, 两端不分配。深层(如 L12 满额 4^12=1677万槽)
// 的实际使用率常低于 1%, 稀疏化后槽区大小从 O(4^L) 降到 O(实际片数)。
struct VtRowDir {
    uint32_t slotStart;   // 该行第一个槽在槽区内的下标
    uint32_t txLo;        // 第一个有数据的 tx
    uint16_t count;       // 槽数(= 该行用到的 tx 个数)
    uint16_t ty;          // 行号(层内)
    uint8_t  level;       // 层号
    uint8_t  _pad[3];
};

// 每瓦片一个槽
struct VtSlot {
    uint64_t offset;    // 数据段内绝对偏移(0=未写)
    uint32_t size;      // 压缩块字节数
    uint8_t  valid;     // 1=可读
    uint8_t  _pad[3];
};
#pragma pack(pop)

static_assert(sizeof(VtSlot) == 16, "VtSlot must be 16 bytes");
static_assert(sizeof(VtRowDir) == 16, "VtRowDir must be 16 bytes");
static_assert(sizeof(VtFileHeader) == 184,
              "VtFileHeader layout changed; headerSize check will invalidate old caches");

// 每层槽表槽数
inline uint64_t slotCount(int level) { return (uint64_t)1 << (2 * level); }
// 槽表区总字节
inline uint64_t slotTableBytes(int maxLevel) {
    uint64_t n = 0;
    for (int L = 0; L <= maxLevel; ++L) n += slotCount(L) * sizeof(VtSlot);
    return n;
}

// ---- 顶点编码: delta + zigzag + varint ----
// 同一环内相邻顶点格坐标接近, delta 后数值小、熵低, zstd 能多压数倍。
inline void putVarint(std::vector<uint8_t>& out, uint32_t v) {
    while (v >= 0x80) { out.push_back((uint8_t)(v | 0x80)); v >>= 7; }
    out.push_back((uint8_t)v);
}
inline uint32_t getVarint(const uint8_t*& p, const uint8_t* end) {
    uint32_t v = 0; int shift = 0;
    while (p < end && shift < 35) {
        uint8_t b = *p++;
        v |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return v;
        shift += 7;
    }
    return v;
}
inline uint32_t zigzag32(int32_t d) { return (uint32_t)((d << 1) ^ (d >> 31)); }
inline int32_t unzigzag32(uint32_t z) { return (int32_t)(z >> 1) ^ -(int32_t)(z & 1); }

// ---- VtTile 序列化(紧凑) ----
// 布局: [originX:8][originY:8][epsg:4][vertexCount:4][ringCount:4]
//       [verts: 每顶点 (zigzag(dx),zigzag(dy)) varint][rings: ringCount * 16B]
inline void serializeTile(const VtTile& t, std::vector<uint8_t>& out) {
    uint32_t vc = t.vertexCount();
    uint32_t rc = (uint32_t)t.rings.size();
    out.clear();
    out.reserve(28 + (size_t)vc * 2 + (size_t)rc * 16);
    auto putRaw = [&](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        out.insert(out.end(), b, b + n);
    };
    putRaw(&t.originX, 8);
    putRaw(&t.originY, 8);
    putRaw(&t.epsg, 4);
    putRaw(&vc, 4);
    putRaw(&rc, 4);
    int32_t px = 0, py = 0;
    for (uint32_t i = 0; i < vc; ++i) {
        int32_t x = t.verts[(size_t)i * 2];
        int32_t y = t.verts[(size_t)i * 2 + 1];
        putVarint(out, zigzag32(x - px));
        putVarint(out, zigzag32(y - py));
        px = x; py = y;
    }
    for (const VtRing& r : t.rings) putRaw(&r, 16);
    // 覆盖度块(可选, 追加在末尾): [count:4] + count * (varint delta格号, u8 计数)
    // 旧缓存没有这一段 -> 反序列化时按"剩余字节 < 4"判定为无覆盖度, 向前兼容。
    uint32_t cc = (uint32_t)t.cover.size();
    putRaw(&cc, 4);
    int32_t lastIdx = 0;
    for (uint32_t e : t.cover) {
        const int32_t idx = (int32_t)(e >> 8);
        const uint8_t val = (uint8_t)(e & 0xff);
        putVarint(out, zigzag32(idx - lastIdx));
        out.push_back(val);
        lastIdx = idx;
    }
}

inline bool deserializeTile(const uint8_t* data, size_t n, VtTile& t) {
    if (n < 28) return false;
    const uint8_t* p = data;
    const uint8_t* end = data + n;
    std::memcpy(&t.originX, p, 8); p += 8;
    std::memcpy(&t.originY, p, 8); p += 8;
    std::memcpy(&t.epsg, p, 4);    p += 4;
    uint32_t vc = 0, rc = 0;
    std::memcpy(&vc, p, 4); p += 4;
    std::memcpy(&rc, p, 4); p += 4;
    // vc/rc 来自文件字节: 每顶点至少 2 字节(两个 varint), 先校验再分配, 避免损坏缓存触发超大 resize
    if ((size_t)vc > (size_t)(end - p) / 2) return false;
    t.verts.resize((size_t)vc * 2);
    int32_t px = 0, py = 0;
    for (uint32_t i = 0; i < vc; ++i) {
        px += unzigzag32(getVarint(p, end));
        py += unzigzag32(getVarint(p, end));
        t.verts[(size_t)i * 2] = (int16_t)px;
        t.verts[(size_t)i * 2 + 1] = (int16_t)py;
    }
    size_t need = (size_t)(p - data) + (size_t)rc * 16;
    if (n < need) return false;
    t.rings.resize(rc);
    for (uint32_t i = 0; i < rc; ++i) { std::memcpy(&t.rings[i], p, 16); p += 16; }
    // 覆盖度块(可选; 旧缓存到此结束)
    if ((size_t)(end - p) >= 4) {
        uint32_t cc = 0;
        std::memcpy(&cc, p, 4);
        p += 4;
        if ((size_t)(end - p) < (size_t)cc * 2) return false;   // 损坏: 至少每条 2 字节
        t.cover.resize(cc);
        int32_t lastIdx = 0;
        for (uint32_t i = 0; i < cc; ++i) {
            lastIdx += unzigzag32(getVarint(p, end));
            t.cover[i] = ((uint32_t)lastIdx << 8) | (uint32_t)(*p++);
        }
    }
    return true;
}

}  // namespace peekg::vt
