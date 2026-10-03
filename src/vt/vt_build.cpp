#include "vt/vt_build.h"
#include "vt/vt_cache.h"
#include "vt/vt_source.h"
#include "vt/vt_timing.h"
#include "vt/vt_topology.h"
#include "data/gdal_common.h"
#include "data/reproject.h"

#include <ogr_api.h>
#include <geos_c.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <limits>
#include <list>
#include <thread>
#include <unordered_map>
#include <vector>

namespace peekg::vt {

namespace {

using peekg::data::ensureGdal;
using peekg::data::gdalOpenVector;
using peekg::data::gdalSrsEpsg;
using peekg::data::reprojectPoint;

uint64_t tileKey(int level, int tx, int ty) {
    return ((uint64_t)(level & 0xff) << 48) |
           ((uint64_t)(tx & 0xffffff) << 24) |
           (uint64_t)(ty & 0xffffff);
}

int keyLevel(uint64_t k) { return (int)((k >> 48) & 0xff); }
int keyTx(uint64_t k) { return (int)((k >> 24) & 0xffffff); }
int keyTy(uint64_t k) { return (int)(k & 0xffffff); }

// 显示 CRS 数据范围。dstEpsg != srcEpsg 时对四角+中心重投影取包络并加 2% 余量。
void displayBbox(const LayerInfo& li, int dstEpsg,
                 double& minx, double& miny, double& maxx, double& maxy) {
    minx = li.minx; miny = li.miny; maxx = li.maxx; maxy = li.maxy;
    if (dstEpsg > 0 && li.srcEpsg > 0 && dstEpsg != li.srcEpsg && li.hasExtent) {
        double xs[5] = {li.minx, li.maxx, li.minx, li.maxx, (li.minx + li.maxx) / 2};
        double ys[5] = {li.miny, li.miny, li.maxy, li.maxy, (li.miny + li.maxy) / 2};
        minx = miny = 1e300; maxx = maxy = -1e300;
        for (int i = 0; i < 5; ++i) {
            double ox, oy;
            if (reprojectPoint(xs[i], ys[i], li.srcEpsg, dstEpsg, ox, oy)) {
                minx = std::min(minx, ox); maxx = std::max(maxx, ox);
                miny = std::min(miny, oy); maxy = std::max(maxy, oy);
            }
        }
        if (minx > maxx) { minx = li.minx; miny = li.miny; maxx = li.maxx; maxy = li.maxy; }
        double mx = (maxx - minx) * 0.02, my = (maxy - miny) * 0.02;
        minx -= mx; maxx += mx; miny -= my; maxy += my;
    }
}

// ---- GEOS 矩形裁剪(替代手写 S-H: 凹多边形不会产生桥接假边, 大面包含整片也不会丢) ----
void geosSilent(const char*, ...) {}

GEOSContextHandle_t geosInit() {
    GEOSContextHandle_t ctx = GEOS_init_r();
    if (ctx) {
        GEOSContext_setNoticeHandler_r(ctx, geosSilent);
        GEOSContext_setErrorHandler_r(ctx, geosSilent);
    }
    return ctx;
}

// 闭合环 -> GEOS 多边形(调用方负责 GEOSGeom_destroy_r)
GEOSGeometry* geosPolygon(GEOSContextHandle_t ctx, const std::vector<double>& xy) {
    int n = (int)(xy.size() / 2);
    if (n < 3) return nullptr;
    bool closed = (xy[0] == xy[2 * (n - 1)] && xy[1] == xy[2 * (n - 1) + 1]);
    int m = closed ? n : n + 1;
    if (m < 4) return nullptr;
    GEOSCoordSequence* seq = GEOSCoordSeq_create_r(ctx, (unsigned)m, 2);
    if (!seq) return nullptr;
    for (int i = 0; i < n; ++i) {
        GEOSCoordSeq_setX_r(ctx, seq, (unsigned)i, xy[2 * i]);
        GEOSCoordSeq_setY_r(ctx, seq, (unsigned)i, xy[2 * i + 1]);
    }
    if (!closed) {
        GEOSCoordSeq_setX_r(ctx, seq, (unsigned)n, xy[0]);
        GEOSCoordSeq_setY_r(ctx, seq, (unsigned)n, xy[1]);
    }
    GEOSGeometry* ring = GEOSGeom_createLinearRing_r(ctx, seq);
    if (!ring) { GEOSCoordSeq_destroy_r(ctx, seq); return nullptr; }
    GEOSGeometry* poly = GEOSGeom_createPolygon_r(ctx, ring, nullptr, 0);
    if (!poly) { GEOSGeom_destroy_r(ctx, ring); return nullptr; }
    return poly;
}

// 裁剪: 输出结果各多边形的外环(每个一个 xy 数组; 孔由调用方按 hole 环另行处理)
void geosClipRings(GEOSContextHandle_t ctx, const GEOSGeometry* poly,
                   double xmin, double ymin, double xmax, double ymax,
                   std::vector<std::vector<double>>& outRings) {
    outRings.clear();
    GEOSGeometry* res = GEOSClipByRect_r(ctx, const_cast<GEOSGeometry*>(poly), xmin, ymin, xmax, ymax);
    if (!res) return;
    std::function<void(const GEOSGeometry*)> visit = [&](const GEOSGeometry* g) {
        if (!g) return;
        int gt = GEOSGeomTypeId_r(ctx, g);
        if (gt == GEOS_POLYGON) {
            const GEOSGeometry* ext = GEOSGetExteriorRing_r(ctx, g);
            if (!ext) return;
            const GEOSCoordSequence* cs = GEOSGeom_getCoordSeq_r(ctx, ext);
            if (!cs) return;
            unsigned int m = 0;
            GEOSCoordSeq_getSize_r(ctx, cs, &m);
            std::vector<double> r;
            r.reserve((size_t)m * 2);
            for (unsigned int i = 0; i < m; ++i) {
                double x = 0, y = 0;
                GEOSCoordSeq_getX_r(ctx, cs, i, &x);
                GEOSCoordSeq_getY_r(ctx, cs, i, &y);
                r.push_back(x); r.push_back(y);
            }
            if (r.size() >= 6) outRings.push_back(std::move(r));
        } else if (gt == GEOS_MULTIPOLYGON || gt == GEOS_GEOMETRYCOLLECTION) {
            int ng = GEOSGetNumGeometries_r(ctx, g);
            for (int i = 0; i < ng; ++i) visit(GEOSGetGeometryN_r(ctx, g, i));
        }
    };
    visit(res);
    GEOSGeom_destroy_r(ctx, res);
}

struct GeosCtxGuard {
    GEOSContextHandle_t ctx;
    GeosCtxGuard() : ctx(geosInit()) {}
    ~GeosCtxGuard() { if (ctx) GEOS_finish_r(ctx); }
    GeosCtxGuard(const GeosCtxGuard&) = delete;
    GeosCtxGuard& operator=(const GeosCtxGuard&) = delete;
};

bool clipSegment(double x1, double y1, double x2, double y2,
                 double xmin, double ymin, double xmax, double ymax,
                 double& ox1, double& oy1, double& ox2, double& oy2) {
    double dx = x2 - x1, dy = y2 - y1;
    double t0 = 0.0, t1 = 1.0;
    auto clip = [&](double p, double q) -> bool {
        if (p == 0) return q >= 0;
        double r = q / p;
        if (p < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
        else { if (r < t0) return false; if (r < t1) t1 = r; }
        return true;
    };
    if (!clip(-dx, x1 - xmin)) return false;
    if (!clip( dx, xmax - x1)) return false;
    if (!clip(-dy, y1 - ymin)) return false;
    if (!clip( dy, ymax - y1)) return false;
    ox1 = x1 + t0 * dx; oy1 = y1 + t0 * dy;
    ox2 = x1 + t1 * dx; oy2 = y1 + t1 * dy;
    return true;
}

// 注意: 不做几何抽稀。相邻图斑共享边若各自抽稀会分叉 -> 量化后漏风(碎面)。
// 降顶点只靠"量化 + 环内连续去重"; 体积靠 delta+zigzag+zstd 压。

// 量化 + 环内连续去重 + 退化丢弃, 追加到瓦片
// 量化到格 + 环内连续去重 + 去掉显式闭合点
static void quantizeRing(const std::vector<double>& xyDisp, double originX, double originY,
                         double cell, std::vector<int16_t>& q) {
    int n = (int)(xyDisp.size() / 2);
    q.reserve((size_t)n * 2);
    for (int i = 0; i < n; ++i) {
        long gx = std::lround((xyDisp[2*i] - originX) / cell);
        long gy = std::lround((xyDisp[2*i+1] - originY) / cell);
        if (gx < -32768) gx = -32768; if (gx > 32767) gx = 32767;
        if (gy < -32768) gy = -32768; if (gy > 32767) gy = 32767;
        if (!q.empty() && q[q.size()-2] == (int16_t)gx && q[q.size()-1] == (int16_t)gy) continue;
        q.push_back((int16_t)gx);
        q.push_back((int16_t)gy);
    }
    // 去掉显式闭合点(首==尾), 否则共线压缩会把首点误删
    if (q.size() >= 4 && q[0] == q[q.size()-2] && q[1] == q[q.size()-1])
        q.resize(q.size() - 2);
}

// 共线点压缩(精确整数, 不改形状): 中间点落在前后点连线上则去掉。
// 共享边内部点两边上下文相同 -> 压缩结果一致, 不漏风。
static void compressCollinear(std::vector<int16_t>& q, uint8_t type) {
    if (type == RING_POINT || q.size() < 6) return;
    std::vector<int16_t> r;
    r.reserve(q.size());
    int m = (int)(q.size() / 2);
    for (int i = 0; i < m; ++i) {
        int pi = (i - 1 + m) % m, ni = (i + 1) % m;
        long x0 = q[pi*2], y0 = q[pi*2+1];
        long x1 = q[i*2],  y1 = q[i*2+1];
        long x2 = q[ni*2], y2 = q[ni*2+1];
        long cross = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
        if (cross == 0) continue;   // 共线: 去掉中间点
        r.push_back((int16_t)x1); r.push_back((int16_t)y1);
    }
    if ((int)(r.size() / 2) >= (type == RING_FACE ? 3 : 2)) q.swap(r);
}

// 退化处理: 点数/面积过小
// 诊断计数: 按"层 + 原因"统计。对"铺满型"数据(街区/地块)来说, 丢一个面就等于在
// 底图上打一个洞, 所以要能看清丢了多少。
// 计数是「每次构建」的量, buildVtCache 开头会清零(同进程可多次构建)。
static std::atomic<long long> g_dropArea[32];
static std::atomic<long long> g_dropVerts[32];
static std::atomic<int> g_curLevel;

// 层号越界保护: 直读层的 level 可以超过 maxLevel, 而数组只有 32 格。
// 越界时归到最后一格 —— 宁可日志不准, 也不能越界写坏内存。
static inline int dropSlot(int L) { return L < 0 ? 0 : (L > 31 ? 31 : L); }
static void resetDropCounters() {
    for (int i = 0; i < 32; ++i) { g_dropArea[i] = 0; g_dropVerts[i] = 0; }
}

// 环的格坐标有向面积
static double ringAreaCells(const std::vector<int16_t>& q) {
    int m = (int)(q.size() / 2);
    double a = 0;
    for (int i = 0; i < m; ++i) {
        int j = (i + 1) % m;
        a += (double)q[2*i] * q[2*j+1] - (double)q[2*j] * q[2*i+1];
    }
    return a;
}

// 用质心摆放退化面(方案 B): 把塌缩的面拉到原始环的质心位置, 生成 1×1 方块,
static void keepDegenerateFaceCentroid(std::vector<int16_t>& q, const std::vector<double>& xyDisp,
                                      double originX, double originY, double cell) {
    size_t m = xyDisp.size() / 2;
    double cx = 0, cy = 0;
    if (m == 0) { cx = xyDisp.empty() ? 0 : xyDisp[0]; cy = xyDisp.size() < 2 ? 0 : xyDisp[1]; }
    else { for (size_t i = 0; i < m; ++i) { cx += xyDisp[2 * i]; cy += xyDisp[2 * i + 1]; } cx /= (double)m; cy /= (double)m; }
    int gx = (int)std::llround((cx - originX) / cell);
    int gy = (int)std::llround((cy - originY) / cell);
    q.assign({(int16_t)gx, (int16_t)gy, (int16_t)(gx + 1), (int16_t)gy, (int16_t)(gx + 1), (int16_t)(gy + 1), (int16_t)gx, (int16_t)(gy + 1)});
}

// keepDegenerate=true(最深层): 退化面不丢, 撑成 >=1 格(见上);
// false(粗层/线): 照旧丢弃。
static bool ringDegenerate(uint8_t type, std::vector<int16_t>& q, const std::vector<double>& xyDisp,
                           double originX, double originY, double cell,
                           bool keepDegenerate) {
    if (type == RING_POINT) return q.size() < 2;
    if (type == RING_LINE) return q.size() < 4;
    const int slot = dropSlot(g_curLevel);
    if (q.size() < 6) {
        if (keepDegenerate && type == RING_FACE) { g_dropArea[slot]++; keepDegenerateFaceCentroid(q, xyDisp, originX, originY, cell); return false; }
        g_dropVerts[slot]++; return true;
    }
    if (std::fabs(ringAreaCells(q)) < 1.0) {
        if (keepDegenerate) { g_dropArea[slot]++; keepDegenerateFaceCentroid(q, xyDisp, originX, originY, cell); return false; }
        g_dropArea[slot]++; return true;
    }
    return false;
}

// keepDegenerateFace: 最深层(= Lmax)的退化面撑成 >=1 格(见 ringDegenerate 注释);
void appendRing(VtTile& t, uint8_t type, uint8_t hole, uint32_t polyGroup,
                const std::vector<double>& xyDisp, double originX, double originY, double cell,
                bool keepDegenerate = false) {
    if (xyDisp.size() < 2) return;
    std::vector<int16_t> q;
    quantizeRing(xyDisp, originX, originY, cell, q);
    compressCollinear(q, type);
    if (ringDegenerate(type, q, xyDisp, originX, originY, cell, keepDegenerate)) return;
    VtRing r;
    r.type = type; r.hole = hole; r.polyGroup = polyGroup;
    r.firstVertex = t.vertexCount();
    r.vertexCount = (uint32_t)(q.size() / 2);
    t.verts.insert(t.verts.end(), q.begin(), q.end());
    t.rings.push_back(r);
}

// 直读专用: 世界坐标 float 直接入 fverts —— 不量化/不压共线/不改退化面(直读=源几何)。
// 只做两件无损事: 连续重复点去重 + 去显式闭合点(首==尾, earcut/描边闭合逻辑同缓存路径)。
static void appendRingWorld(VtTile& t, uint8_t type, uint8_t hole, uint32_t polyGroup,
                            const std::vector<double>& xyDisp) {
    if (xyDisp.size() < 2) return;
    int n = (int)(xyDisp.size() / 2);
    uint32_t start = (uint32_t)(t.fverts.size() / 2);
    t.fverts.reserve(t.fverts.size() + (size_t)n * 2);
    for (int i = 0; i < n; ++i) {
        float x = (float)xyDisp[2 * i], y = (float)xyDisp[2 * i + 1];
        size_t sz = t.fverts.size();
        if (sz >= 2 && t.fverts[sz - 2] == x && t.fverts[sz - 1] == y &&
            (uint32_t)(sz / 2) != start) continue;
        t.fverts.push_back(x); t.fverts.push_back(y);
    }
    // 去显式闭合点(本环首点==末点)
    size_t sz = t.fverts.size();
    uint32_t s2 = start * 2;
    if (sz >= (size_t)s2 + 4 &&
        t.fverts[s2] == t.fverts[sz - 2] && t.fverts[s2 + 1] == t.fverts[sz - 1])
        t.fverts.resize(sz - 2);
    uint32_t vc = (uint32_t)(t.fverts.size() / 2) - start;
    size_t minV = (type == RING_FACE) ? 3 : (type == RING_LINE ? 2 : 1);
    if (vc < minV) { t.fverts.resize((size_t)start * 2); return; }   // 真退化(点数不够), 丢
    VtRing r;
    r.type = type; r.hole = hole; r.polyGroup = polyGroup;
    r.firstVertex = start;
    r.vertexCount = vc;
    t.rings.push_back(r);
}

// ---- 小面合并(纯整数格空间, 不用 GEOS): ----
// 面积 < minAreaCells(格²) 的面按聚合格(每 groupCells 格)分组, 组内统计无向边:
// 计数==2 的边 = 内部公共边 -> 丢弃; 计数==1 的 = 外边界 -> 追踪成新环。
// 拓扑安全(不产生自交/不丢洞), 与"大面"之间的边不动 -> 无缝无叠。
struct FaceMergeCfg {
    double minAreaCells = 16.0;   // 小面阈值: 面积 < 16 格² (≈4x4)
    int groupCells = 4;           // 聚合格边长(格)
    double dropCells = 1.0;       // 合并后面积 < 该值(格²)的碎片直接丢
};

static inline uint64_t fmPtKey(int x, int y) {
    return (uint64_t)(uint16_t)x | ((uint64_t)(uint16_t)y << 16);
}
static inline uint64_t fmEdgeKey(int x0, int y0, int x1, int y1) {
    uint64_t a = fmPtKey(x0, y0), b = fmPtKey(x1, y1);
    return a < b ? ((a << 32) | b) : ((b << 32) | a);
}

struct SmallFace { uint32_t ri; int gx, gy; };

// 收集小面(面积 < minAreaCells 的面), 记录其环下标与聚合格
static void collectSmallFaces(const VtTile& t, const FaceMergeCfg& cfg, std::vector<SmallFace>& small) {
    for (uint32_t i = 0; i < t.rings.size(); ++i) {
        const VtRing& r = t.rings[i];
        if (r.type != RING_FACE || r.vertexCount < 3) continue;
        const int16_t* p = t.verts.data() + (size_t)r.firstVertex * 2;
        int n = (int)r.vertexCount;
        double a = 0;
        long mnx = 32767, mxx = -32768, mny = 32767, mxy = -32768;
        for (int k = 0; k < n; ++k) {
            int j = (k + 1) % n;
            a += (double)p[2 * k] * p[2 * j + 1] - (double)p[2 * j] * p[2 * k + 1];
            mnx = std::min<long>(mnx, p[2 * k]); mxx = std::max<long>(mxx, p[2 * k]);
            mny = std::min<long>(mny, p[2 * k + 1]); mxy = std::max<long>(mxy, p[2 * k + 1]);
        }
        if (std::fabs(a) * 0.5 >= cfg.minAreaCells) continue;   // 大面保留
        int gx = (int)std::floor(((double)(mnx + mxx) * 0.5) / cfg.groupCells);
        int gy = (int)std::floor(((double)(mny + mxy) * 0.5) / cfg.groupCells);
        small.push_back({i, gx, gy});
    }
}

// 由"只出现一次的边"追踪出组内并集的外边界环(计数==2 的是内部公共边, 丢弃)
static void traceGroupRings(const std::unordered_map<uint64_t, int>& ecnt,
                            std::vector<std::vector<int16_t>>& got) {
    std::unordered_map<uint64_t, std::vector<uint64_t>> adj;
    std::unordered_map<uint64_t, int> used;
    for (auto& e : ecnt) {
        if (e.second != 1) continue;
        used[e.first] = 0;
        uint64_t a = e.first >> 32, b = e.first & 0xffffffffull;
        adj[a].push_back(b);
        adj[b].push_back(a);
    }
    if (adj.empty()) return;
    for (auto& e : ecnt) {
        if (e.second != 1 || used[e.first]) continue;
        used[e.first] = 1;
        uint64_t a = e.first >> 32, b = e.first & 0xffffffffull;
        std::vector<int16_t> ring;
        ring.push_back((int16_t)(uint16_t)(a & 0xffff));
        ring.push_back((int16_t)(uint16_t)((a >> 16) & 0xffff));
        uint64_t cur = b, prev = a;
        for (int guard = 0; guard < 100000; ++guard) {
            if (cur == a) break;
            ring.push_back((int16_t)(uint16_t)(cur & 0xffff));
            ring.push_back((int16_t)(uint16_t)((cur >> 16) & 0xffff));
            uint64_t nxt = UINT64_MAX;
            for (uint64_t nb : adj[cur]) {
                if (nb == prev) continue;
                uint64_t ek = fmEdgeKey((int)(uint16_t)(cur & 0xffff), (int)(uint16_t)((cur >> 16) & 0xffff),
                                        (int)(uint16_t)(nb & 0xffff), (int)(uint16_t)((nb >> 16) & 0xffff));
                if (!used[ek]) { used[ek] = 1; nxt = nb; break; }
            }
            if (nxt == UINT64_MAX) break;
            prev = cur; cur = nxt;
        }
        if (ring.size() >= 6) got.push_back(std::move(ring));
    }
}

// 重建瓦片: 保留未丢弃的环 + 追加合并环(过小的碎片直接丢)
static void rebuildMergedTile(VtTile& t, const std::vector<uint8_t>& drop,
                              std::vector<std::vector<int16_t>>& merged, const FaceMergeCfg& cfg) {
    std::vector<int16_t> nv;
    std::vector<VtRing> nr;
    for (uint32_t i = 0; i < t.rings.size(); ++i) {
        if (drop[i]) continue;
        VtRing r = t.rings[i];
        r.firstVertex = (uint32_t)(nv.size() / 2);
        nv.insert(nv.end(), t.verts.begin() + (size_t)t.rings[i].firstVertex * 2,
                  t.verts.begin() + (size_t)(t.rings[i].firstVertex + t.rings[i].vertexCount) * 2);
        nr.push_back(r);
    }
    for (auto& g : merged) {
        int n = (int)(g.size() / 2);
        if (n < 3) continue;
        double a = 0;
        for (int i = 0; i < n; ++i) { int j = (i + 1) % n; a += (double)g[2 * i] * g[2 * j + 1] - (double)g[2 * j] * g[2 * i + 1]; }
        if (std::fabs(a) * 0.5 < cfg.dropCells) continue;
        VtRing r;
        r.type = RING_FACE; r.hole = 0; r.polyGroup = 0;
        r.firstVertex = (uint32_t)(nv.size() / 2);
        r.vertexCount = (uint32_t)n;
        nv.insert(nv.end(), g.begin(), g.end());
        nr.push_back(r);
    }
    t.verts.swap(nv);
    t.rings.swap(nr);
}

static bool mergeSmallFaces(VtTile& t, const FaceMergeCfg& cfg) {
    if (t.rings.size() < 2) return false;
    std::vector<SmallFace> small;
    collectSmallFaces(t, cfg, small);
    if (small.size() < 2) return false;

    std::unordered_map<uint64_t, std::vector<uint32_t>> groups;
    for (uint32_t si = 0; si < small.size(); ++si)
        groups[((uint64_t)(uint32_t)small[si].gx << 32) | (uint32_t)small[si].gy].push_back(si);

    std::vector<uint8_t> drop(t.rings.size(), 0);
    std::vector<std::vector<int16_t>> merged;
    for (auto& kv : groups) {
        if (kv.second.size() < 2) continue;
        std::unordered_map<uint64_t, int> ecnt;
        for (uint32_t si : kv.second) {
            const VtRing& r = t.rings[small[si].ri];
            const int16_t* p = t.verts.data() + (size_t)r.firstVertex * 2;
            int n = (int)r.vertexCount;
            for (int k = 0; k < n; ++k) {
                int j = (k + 1) % n;
                ecnt[fmEdgeKey(p[2 * k], p[2 * k + 1], p[2 * j], p[2 * j + 1])]++;
            }
        }
        std::vector<std::vector<int16_t>> got;
        traceGroupRings(ecnt, got);
        if (got.empty()) continue;
        for (uint32_t si : kv.second) drop[small[si].ri] = 1;   // 按"环下标"标记, 不能用 firstVertex
        for (auto& g : got) merged.push_back(std::move(g));
    }
    if (merged.empty()) return false;

    rebuildMergedTile(t, drop, merged, cfg);
    return true;
}

struct Lru {
    std::unordered_map<uint64_t, VtTile> tiles;
    std::list<uint64_t> order;
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> it;
    long long verts = 0;
    long long cap = 0;

    VtTile& get(uint64_t k) {
        auto f = tiles.find(k);
        if (f != tiles.end()) {
            order.splice(order.begin(), order, it[k]);
            return f->second;
        }
        tiles.emplace(k, VtTile{});
        order.push_front(k);
        it[k] = order.begin();
        return tiles[k];
    }
    void remove(uint64_t k) {
        tiles.erase(k);
        auto f = it.find(k);
        if (f != it.end()) { order.erase(f->second); it.erase(f); }
    }
};

std::string parentDir(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

}  // namespace

namespace {

// 相邻点间距采样: 蓄水池(取前 cap 个), 用于量出源数据自身精度
struct GapSampler {
    std::vector<double> v;
    long long cap = 3000000;
    void add(double d) {
        if (std::isfinite(d) && d > 0 && (long long)v.size() < cap) v.push_back(d);
    }
    // 分位数(0<p<1)。用 nth_element, O(n)。
    double quantile(double p) {
        if (v.empty()) return 0.0;
        size_t k = (size_t)(p * (double)(v.size() - 1));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
    }
};

// 一次遍历同时产出: 每级格化点数计数(顶点估算) + 相邻点间距(误差策略) +
// 要素 bbox 分位数(稳健跨度)。计数口径与构建阶段A 一致(量化到 cell 网格 +
// 连续去重), 不是 Douglas-Peucker。注意 cell[] 一律用 512 基准; 最深层真实
// 是 1024 细格, 选层时再按比例修正。
constexpr int kMaxProbeLevels = 24;

constexpr size_t kMaxProbeBBox = 200000;   // 要素 bbox 采样上限

struct Probe {
    double ox = 0, oy = 0;
    const double* cell = nullptr;
    int C = 0;
    long long cnt[kMaxProbeLevels + 1] = {0};
    GapSampler* gap = nullptr;
    long long raw = 0;                 // 原始点数累计(选层公式的 N, 不受量化影响)
    // 要素 bbox 收集(取分位数得稳健跨度, 剔除离群要素)
    std::vector<double> bx0, by0, bx1, by1;
    bool bboxFull() const { return bx0.size() >= kMaxProbeBBox; }
};

void probeBBox(Probe& p, OGRGeometryH g) {
    if (!g || p.bboxFull()) return;
    OGREnvelope e;
    OGR_G_GetEnvelope(g, &e);   // GDAL 3.12 起返回 void(旧版返回 OGRerr)
    if (!(e.MaxX >= e.MinX && e.MaxY >= e.MinY)) return;   // 空/无效几何
    p.bx0.push_back(e.MinX); p.by0.push_back(e.MinY);
    p.bx1.push_back(e.MaxX); p.by1.push_back(e.MaxY);
}

// 稳健跨度: 要素 bbox 各边裁掉 2% 后的跨度。
// 为什么不用图层 extent: 跨日界线/含离群要素时 extent 会被撑得极大
// (实测 ZCTA extent 322.5°, 但裁掉 2% 后只有 52.2°), 选层会被系统性带偏。
double robustSpan(Probe& p, double fallback) {
    const size_t n = p.bx0.size();
    if (n < 8) return fallback;
    auto q = [](std::vector<double>& v, double f) {
        size_t k = (size_t)(f * (double)(v.size() - 1));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
    };
    double x0 = q(p.bx0, 0.02), x1 = q(p.bx1, 0.98);
    double y0 = q(p.by0, 0.02), y1 = q(p.by1, 0.98);
    double s = std::max(x1 - x0, y1 - y0);
    return (s > 0) ? s : fallback;
}

void probeRing(Probe& p, OGRGeometryH ring) {
    int n = OGR_G_GetPointCount(ring);
    if (n <= 0) return;
    long long px[kMaxProbeLevels + 1] = {0}, py[kMaxProbeLevels + 1] = {0};
    bool has[kMaxProbeLevels + 1] = {false};
    double lx = 0, ly = 0;
    bool hasPrev = false;
    for (int i = 0; i < n; ++i) {
        double x = OGR_G_GetX(ring, i), y = OGR_G_GetY(ring, i);
        if (hasPrev && p.gap) p.gap->add(std::hypot(x - lx, y - ly));
        lx = x; ly = y; hasPrev = true;
        p.raw++;                       // 原始点数(未量化), 选层公式的 N
        for (int L = 0; L <= p.C; ++L) {
            long long gx = std::lround((x - p.ox) / p.cell[L]);
            long long gy = std::lround((y - p.oy) / p.cell[L]);
            if (has[L] && gx == px[L] && gy == py[L]) continue;
            px[L] = gx; py[L] = gy; has[L] = true;
            p.cnt[L] += 1;
        }
    }
}

// 每级计数快照/恢复: 环被"退化阈值"过滤时要把该级计数回滚
struct CntSnap {
    long long v[kMaxProbeLevels + 1];
};
inline void snapTake(CntSnap& s, const Probe& p) {
    for (int L = 0; L <= p.C; ++L) s.v[L] = p.cnt[L];
}

long long probeGeom(Probe& p, OGRGeometryH g) {
    if (!g) return 0;
    OGRwkbGeometryType t = wkbFlatten(OGR_G_GetGeometryType(g));
    switch (t) {
        case wkbPolygon: {
            long long tot = 0;
            int nr = OGR_G_GetGeometryCount(g);
            for (int r = 0; r < nr; ++r) {
                CntSnap b; snapTake(b, p);
                probeRing(p, OGR_G_GetGeometryRef(g, r));
                for (int L = 0; L <= p.C; ++L) {
                    long long k = p.cnt[L] - b.v[L];
                    if (k < 3) p.cnt[L] = b.v[L];        // 退化环(<3 点)不计入, 同旧实现
                    else tot += k;
                }
            }
            return tot;
        }
        case wkbLineString:
        case wkbLinearRing: {
            CntSnap b; snapTake(b, p);
            probeRing(p, g);
            long long tot = 0;
            for (int L = 0; L <= p.C; ++L) {
                long long k = p.cnt[L] - b.v[L];
                if (k < 2) p.cnt[L] = b.v[L];            // 退化线(<2 点)不计入
                else tot += k;
            }
            return tot;
        }
        case wkbPoint:
            for (int L = 0; L <= p.C; ++L) p.cnt[L] += 1;
            return p.C + 1;
        case wkbMultiPoint:
        case wkbMultiPolygon:
        case wkbMultiLineString:
        case wkbGeometryCollection: {
            long long s = 0;
            int ng = OGR_G_GetGeometryCount(g);
            for (int i = 0; i < ng; ++i) s += probeGeom(p, OGR_G_GetGeometryRef(g, i));
            return s;
        }
        // 曲线几何: 容器下钻, 叶子线性化(否则全落 default 计 0 点 -> 选层判"源无几何" Lmax=0)
        case wkbCompoundCurve:
        case wkbMultiCurve:
        case wkbMultiSurface: {
            long long s = 0;
            int ng = OGR_G_GetGeometryCount(g);
            for (int i = 0; i < ng; ++i) s += probeGeom(p, OGR_G_GetGeometryRef(g, i));
            return s;
        }
        case wkbCircularString:
        case wkbCurvePolygon:
        case wkbCurve:
        case wkbSurface: {
            OGRGeometryH lin = OGR_G_GetLinearGeometry(g, 0.0, nullptr);
            long long s = 0;
            if (lin) {
                if (!OGR_G_HasCurveGeometry(lin, FALSE))   // 防递归: 线性化结果必须是线性类型
                    s = probeGeom(p, lin);
                OGR_G_DestroyGeometry(lin);
            }
            return s;
        }
        default:
            return 0;
    }
}

}  // namespace

VtLevelPick pickVtLevel(const std::string& srcPath, int layerIdx, int dstEpsg,
                        double errorFactor, int targetVerts, int cap,
                        long long maxTotalVerts, long long maxVertsPerTile, int levelStep) {
    VtLevelPick out;
    out.level = -1;
    ensureGdal();
    GDALDatasetH ds = gdalOpenVector(srcPath);
    if (!ds) return out;
    int nl = GDALDatasetGetLayerCount(ds);
    if (layerIdx < 0 || layerIdx >= nl) { GDALClose(ds); return out; }
    OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);

    OGRSpatialReferenceH srcSrs = OGR_L_GetSpatialRef(lyr);
    int srcEpsg = gdalSrsEpsg(srcSrs);
    int dst = dstEpsg > 0 ? dstEpsg : srcEpsg;
    OGRCoordinateTransformationH ct = nullptr;
    if (dst > 0 && srcEpsg > 0 && dst != srcEpsg && srcSrs) {
        OGRSpatialReferenceH d = OSRNewSpatialReference(nullptr);
        if (OSRImportFromEPSG(d, dst) == OGRERR_NONE)
            ct = OCTNewCoordinateTransformation(srcSrs, d);
        OSRDestroySpatialReference(d);
    }

    OGREnvelope env;
    double minx = 0, miny = 0, maxx = 0, maxy = 0;
    bool hasExt = OGR_L_GetExtent(lyr, &env, TRUE) == OGRERR_NONE;
    if (hasExt) {
        LayerInfo li;
        li.minx = env.MinX; li.miny = env.MinY; li.maxx = env.MaxX; li.maxy = env.MaxY;
        li.srcEpsg = srcEpsg; li.hasExtent = true;
        displayBbox(li, dst, minx, miny, maxx, maxy);
    }
    double S = hasExt ? std::max(maxx - minx, maxy - miny) : 1.0;
    if (S <= 0) S = 1.0;

    long long F = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
    if (F <= 0) {
        spdlog::warn("[vt] 选层: 图层要素数为 0, 退回 L0: {}", srcPath);
        if (ct) OCTDestroyCoordinateTransformation(ct);
        GDALClose(ds); out.level = 0; return out;
    }

    double spanX = maxx - minx, spanY = maxy - miny;
    double originX = minx - (S - spanX) / 2;
    double originY = miny - (S - spanY) / 2;

    // 采样格网按 1024 基准(= 最深层实际用的细格)。这样「把 L 当最深层」时的保留率
    // 就是 cnt[L] 的直接读数, 不需要任何倍率外推。
    // 早前用 512 基准计数再乘 2 外推细格结果, 在接近饱和时是错的(实测 ZCTA L12
    // 外推得 42.8% 而真实值 95.2%)—— 保留点并不随格距减半而线性翻倍。
    int C = std::min(std::max(0, cap), kMaxProbeLevels - 1);
    std::vector<double> cell(C + 1);
    for (int L = 0; L <= C; ++L) cell[L] = S / (FINE_TILE_SIZE * std::pow(2.0, L));

    // 采样: 按 FID **分层跨表**取样, 而不是顺序取前 sampleK 个。
    //
    // 为什么不能顺序取前 N 个(这是本 bug 的根因):
    //   很多权威矢量(TIGER/人口普查街区、ZCTA)按地理顺序排列要素 —— 前 N 个正好
    //   落在最密集的城区。实测 tabblock20(52万要素): 顺序前 5 万个的源点数外推得
    //   6939 万, 而分层跨表 + 全量交叉验证的真值只有 3437 万 —— 高估 2.02 倍。
    //   顶点预算被高估 2 倍 -> 体积安全阀误触发 -> L6 被压到 L4(实测把格距从 18m
    //   拉粗到 70m), 小街区塌成碎点, 放大后成片锯齿/洞。
    //
    // 为什么要保留"预算"而不是全表扫描: 千万级表全表扫描要几十秒, 会让"开始建
    //   缓存"迟迟不发生。所以按 FID 等距跨表取样 —— 每层网格都能均匀覆盖, 且只读
    //   sampleK 条(随机访问对 shp/gpkg 都是 O(1))。
    const long long sampleK = 50000;
    GapSampler gap;
    Probe pr;
    pr.ox = originX; pr.oy = originY; pr.cell = cell.data(); pr.C = C; pr.gap = &gap;
    long long n = 0;
    OGR_L_ResetReading(lyr);
    OGRFeatureH f;
    // F 已由 OGR_L_GetFeatureCount 给出; 按步长跨表取样。step==1 时等价于全表。
    if (F <= sampleK) {
        while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
            OGRGeometryH g = OGR_F_GetGeometryRef(f);
            if (g) {
                OGRGeometryH gg = g;
                OGRGeometryH owned = nullptr;
                if (ct) { owned = OGR_G_Clone(g); OGR_G_Transform(owned, ct); gg = owned; }
                probeGeom(pr, gg);
                probeBBox(pr, gg);
                if (owned) OGR_G_DestroyGeometry(owned);
                ++n;
            }
            OGR_F_Destroy(f);
        }
    } else {
        const long long step = std::max<long long>(1, F / sampleK);
        const auto tSamp0 = std::chrono::steady_clock::now();
        const double budgetSec = 8.0;   // 随机取样在少数驱动(如 FileGDB)上很慢, 兜个底
        bool overBudget = false;
        for (long long fid = 0; fid < F && !overBudget; fid += step) {
            f = OGR_L_GetFeature(lyr, fid);
            if (!f) continue;
            OGRGeometryH g = OGR_F_GetGeometryRef(f);
            if (g) {
                OGRGeometryH gg = g;
                OGRGeometryH owned = nullptr;
                if (ct) { owned = OGR_G_Clone(g); OGR_G_Transform(owned, ct); gg = owned; }
                probeGeom(pr, gg);
                probeBBox(pr, gg);
                if (owned) OGR_G_DestroyGeometry(owned);
                ++n;
            }
            OGR_F_Destroy(f);
            if ((n & 0xFF) == 0) {
                double el = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - tSamp0).count();
                if (el > budgetSec) overBudget = true;
            }
        }
        if (overBudget)
            spdlog::warn("[vt] 选层采样超 {:.0f}s 预算, 已用 {} 个样本(可能偏少, 层数估算精度下降)", 8.0, n);
    }
    if (ct) OCTDestroyCoordinateTransformation(ct);
    GDALClose(ds);
    if (n == 0) { spdlog::warn("[vt] 选层采样未读到任何要素, 退回 L0(缓存将只有一层, 放大后无内容): {}", srcPath); out.level = 0; return out; }

    // 每层总点数估计(采样在 1024 基准上做的, 与「L 为最深层」时的真实格数一致)。
    // 第二个参数 deepest: L < deepest 时该层实际用 512 格(格距粗 2 倍), 点数约为
    // 这里的一半 —— 只影响体积预算(安全阀), 不参与选层判据。
    // 早前写成 tileSizeAt(L, L) 恒等于 FINE_TILE_SIZE, 那个 0.5 分支是死代码,
    // 导致 totalAt 把每个粗层都按 1024 细格算, 粗层点数虚高一倍。
    auto pointsAt = [&](int L, int deepest) -> double {
        double c = pr.cnt[L] / (double)n * (double)F;
        return c * (tileSizeAt(L, deepest) == FINE_TILE_SIZE ? 1.0 : 0.5);
    };
    // 隔层保留时, 一次构建实际存储的顶点总量(L%step==0 的层 + 最深层)
    auto totalAt = [&](int deepest, int step) {
        double s = 0;
        for (int L = 0; L <= deepest; ++L)
            if (levelKept(L, deepest, step)) s += pointsAt(L, deepest);
        return s;
    };
    // 源顶点总数(样本平均 × 整表要素数)
    const double srcVerts = (n > 0 && pr.raw > 0)
        ? ((double)pr.raw / (double)n) * (double)F : (double)F;
    // 保留率: 把 L 当作最深层(1024 细格)时, 能保留多少比例的源顶点。
    // cnt[] 已在 1024 基准上采好, 所以这是直接读数, 无外推误差。
    auto keepAt = [&](int L) -> double {
        return srcVerts > 0 ? (pr.cnt[L] / (double)n * (double)F) / srcVerts : 0.0;
    };

    // ---- 选层: 让最深层格距 ≈ 数据的「典型结构尺度」 ----
    //
    // 判据: targetCell = p50 / errorFactor, Lmax = 使 L 层格距落到 targetCell 的层。
    //
    // 为什么是 p50(中位段长)而不是 p10/p25:
    //   · 选层的目的是**观感**, 不是几何精度。再往细存的东西, 屏幕上根本看不出来 ——
    //     ZCTA 保留率 L10=50.5% L12=95.2%, 但 L10 与 L12 肉眼无差别, 成本却差 4 倍。
    //   · p10 是「最细的那 10% 段」。用它当精度参照, 等于为了极端细节把**全部**数据
    //     建得很深, 而其余 90% 早就在浅层被量化到接近原样 —— 边际收益递减, 体积
    //     和构建时间指数上升。
    //   · p50 是数据的典型结构尺度。格距压到它附近, 形状轮廓就出来了; 再细是重复
    //     存储源点, 不产生新信息。
    //
    // 校准: 实测「观感合格」的三个点都落在这个判据上
    //   48号 p50=23m -> L6(格距 20m)  ✓ 人工确认合格
    //   55号 p50=~12m -> L6(格距 10m) ✓ 人工确认合格
    //   ZCTA p50=~31m -> L10(格距 31m) ✓ 人工确认合格
    //
    // errorFactor 是相对这个尺度的粗细倍率: >1 更浅省空间, <1 更深。
    out.nativeStep = gap.quantile(0.50);
    const bool canError = (errorFactor > 0.0 && out.nativeStep > 0.0);
    if (canError) {
        out.errorMode = true;
        out.spanUsed = robustSpan(pr, S);
        const double targetCell = out.nativeStep / errorFactor;
        // ceil 而不是 round: 约束是「格距 ≤ 目标尺度」(缓存至少要跟数据一样细)。
        //   cell(L) = S/(2^L × 1024) ≤ targetCell  ⇒  L ≥ log2(S/(1024 × targetCell))
        // 满足它的最小整数是 ceil。round 会往粗的一边偏, 直接违反约束 ——
        // 55 号的 p50 恰好落在 L5/L6 分界(差 3%), round 给 L5(格距比目标粗),
        // ceil 给 L6(格距 23m→11m, 合格)。
        // 往细偏最多浪费一档体积(用户明确: 大小无所谓, 观感优先), 往粗偏则出锯齿。
        int L = (int)std::ceil(std::log2(S / (FINE_TILE_SIZE * targetCell)));
        L = std::max(0, std::min(L, C));
        out.level = L;
        out.vertsPerTile = pointsAt(L, L) / std::pow(4.0, L);
        out.totalVerts = totalAt(L, levelStep);
        out.keepRatio = keepAt(L);
        out.srcVerts = srcVerts;

        // 体积安全阀: 双条件, 谁先触发按谁 —— 取更浅的那档。
        //
        // 条件1「单片顶点数上限」管渲染性能: 渲染一帧要处理整片(建桶 + 画)。
        // 条件2「保留层总顶点预算」管落盘体积: 总量与数据规模成正比, 是可比的兜底量。
        //
        // 为何不能只留条件1(历史 bug): 单个阈值对要素密度差 20~50 倍的数据集无法兼顾
        // —— 定小(如 8192)时 48 号(66.9万要素挤在 13°)被压到 L0 产出"全是洞"的缓存;
        // 定大(如 65536)时对稀疏的 ZCTA 几乎不起作用。两个尺子一起用。
        //
        // 校准: 格化计数反推比实际存储少(实测 48号 1.59x / ZCTA 1.34x), 乘 1.5 保守估计,
        // 否则安全阀会误判"没超预算"而放行过深的层。
        constexpr double kVertsCalib = 1.5;
        const double tileCap = maxVertsPerTile > 0 ? (double)maxVertsPerTile : 1e300;
        const double totCap  = maxTotalVerts  > 0 ? (double)maxTotalVerts  : 1e300;
        double tot = out.totalVerts * kVertsCalib;
        double vpt = out.vertsPerTile * kVertsCalib;
        while (L > 0 && (vpt > tileCap || tot > totCap)) {
            --L;
            tot = totalAt(L, levelStep) * kVertsCalib;
            vpt = pointsAt(L, L) / std::pow(4.0, L) * kVertsCalib;
            out.clampedByVerts = true;
        }
        out.level = L;
        out.cellAt = (S / std::pow(2.0, L)) / tileSizeAt(L, L);
        out.vertsPerTile = vpt;
        out.totalVerts = tot;
        out.keepRatio = keepAt(L);
        return out;
    }

    // 旧策略: |P(L)/4^L − targetVerts| 最小
    int best = 0;
    double bestErr = 1e300;
    for (int L = 0; L <= C; ++L) {
        double r = pointsAt(L, L) / std::pow(4.0, L);
        double err = std::fabs(r - (double)targetVerts);
        if (err < bestErr) { bestErr = err; best = L; }
    }
    out.level = best;
    out.cellAt = (S / std::pow(2.0, best)) / tileSizeAt(best, best);
    out.vertsPerTile = pointsAt(best, best) / std::pow(4.0, best);
    out.totalVerts = totalAt(best, levelStep);
    return out;
}

int estimateMaxLevel(const std::string& srcPath, int layerIdx, int dstEpsg,
                     int targetVerts, int cap) {
    return pickVtLevel(srcPath, layerIdx, dstEpsg, 0.0, targetVerts, cap,
                       (long long)1 << 62, (long long)1 << 62, 1).level;
}

namespace {

// 构建状态: 把原先散在 buildVtCache 里的状态与 lambda 收拢为方法, 降低单函数长度与嵌套。
// ---- VW(Visvalingam-Whyatt)抽稀: 线层用(世界坐标)。线是独立要素, 不分叉, 可直接抽。 ----
static double vwTriAreaW(const std::vector<double>& xy, int a, int b, int c) {
    double ax = xy[2*a], ay = xy[2*a+1], bx = xy[2*b], by = xy[2*b+1], cx = xy[2*c], cy = xy[2*c+1];
    return std::fabs((bx-ax)*(cy-ay) - (by-ay)*(cx-ax)) * 0.5;
}
static void vwThresholdsWorld(const std::vector<double>& xy, std::vector<double>& kk) {
    const int n = (int)(xy.size() / 2);
    const double INF = std::numeric_limits<double>::infinity();
    kk.assign((size_t)(n > 0 ? n : 0), INF);
    if (n < 3) return;
    std::vector<int> prv((size_t)n), nxt((size_t)n);
    std::vector<char> alive((size_t)n, 1);
    using Item = std::pair<double, int>;
    std::vector<Item> heap; heap.reserve((size_t)n);
    for (int i = 0; i < n; ++i) { prv[(size_t)i]=i-1; nxt[(size_t)i]=i+1;
        double v = (i==0||i==n-1) ? INF : vwTriAreaW(xy,i-1,i,i+1); kk[(size_t)i]=v; heap.push_back({v,i}); }
    std::make_heap(heap.begin(), heap.end(), std::greater<Item>());
    double maxVal = -INF;
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), std::greater<Item>());
        Item it = heap.back(); heap.pop_back(); int c = it.second;
        if (!alive[(size_t)c] || it.first != kk[(size_t)c]) continue;
        if (it.first == INF) break;
        if (it.first < maxVal) kk[(size_t)c] = maxVal; else maxVal = it.first;
        int b = prv[(size_t)c], d = nxt[(size_t)c];
        alive[(size_t)c]=0; nxt[(size_t)b]=d; prv[(size_t)d]=b;
        if (b > 0) { double nv = vwTriAreaW(xy,prv[(size_t)b],b,d); kk[(size_t)b]=nv; heap.push_back({nv,b}); std::push_heap(heap.begin(),heap.end(),std::greater<Item>()); }
        if (d < n-1) { double nv = vwTriAreaW(xy,b,d,nxt[(size_t)d]); kk[(size_t)d]=nv; heap.push_back({nv,d}); std::push_heap(heap.begin(),heap.end(),std::greater<Item>()); }
    }
    for (int i = 1; i < n-1; ++i) if (kk[(size_t)i] < INF) kk[(size_t)i] = std::sqrt(kk[(size_t)i]) * 0.65;
}
static void filterByIntervalWorld(const std::vector<double>& xy, const std::vector<double>& kk,
                                  double interval, std::vector<double>& out) {
    out.clear(); int n = (int)(xy.size()/2);
    if ((int)kk.size() < n) return;
    for (int i = 0; i < n; ++i) if (kk[(size_t)i] >= interval) { out.push_back(xy[2*i]); out.push_back(xy[2*i+1]); }
}
static void ringBboxW(const std::vector<double>& xy, double& minx,double& miny,double& maxx,double& maxy) {
    minx = miny = 1e300; maxx = maxy = -1e300; int n = (int)(xy.size()/2);
    for (int i = 0; i < n; ++i) { double x=xy[2*i], y=xy[2*i+1]; minx=std::min(minx,x);maxx=std::max(maxx,x);miny=std::min(miny,y);maxy=std::max(maxy,y); }
}

struct BuildState {
    const VtBuildConfig& cfg;
    VtCache& cache;
    VtBuildStats& stats;
    const std::function<void(int, int, int)>& onTile;
    const std::function<void(int)>& onProgress;
    const std::function<void(int, int, int)>& onCover;
    const std::function<void(int, int, int)>& onMerge;

    double originX = 0, originY = 0, S = 1;
    int Lmax = 0, maxLv = 0;
    long long featureCount = 0;
    bool trace = false;
    bool hasBbox = false;          // PEEK_VT_BBOX: 只处理该范围(诊断用, 快速出几片)
    double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    Lru lru;
    FaceMergeCfg fmcfg;
    GEOSContextHandle_t geosCtx = nullptr;
    std::vector<double> kk_, simpXY_;   // 线层 VW 缓冲
    SourceRing simpRing_;

    BuildState(const VtBuildConfig& c, VtCache& ca, VtBuildStats& st,
               const std::function<void(int, int, int)>& onTile_,
               const std::function<void(int)>& onProgress_,
               const std::function<void(int, int, int)>& onCover_,
               const std::function<void(int, int, int)>& onMerge_)
        : cfg(c), cache(ca), stats(st), onTile(onTile_), onProgress(onProgress_), onCover(onCover_),
          onMerge(onMerge_) {}

    // 取瓦片(不存在则建), 首次触及上报覆盖框
    VtTile& tileAt(int L, int tx, int ty) {
        uint64_t k = tileKey(L, tx, ty);
        bool isNew = lru.tiles.find(k) == lru.tiles.end();
        VtTile& t = lru.get(k);
        if (isNew && onCover) onCover(L, tx, ty);
        return t;
    }

    // 落盘: 读回已有片合并(淘汰后再触达不丢), 非最深层做小面合并, 写回缓存
    void flushTile(uint64_t k) {
        VtScope _tf(vtTime().flush, vtTime().nFlush);
        auto f = lru.tiles.find(k);
        if (f == lru.tiles.end()) return;
        long long nv = (long long)f->second.vertexCount();
        if (!f->second.empty()) {
            int lv = keyLevel(k), tx = keyTx(k), ty = keyTy(k);
            VtTile out = std::move(f->second);
            VtTile existing;
            bool had = cache.readTile(lv, tx, ty, existing);
            if (trace)
                fprintf(stderr, "[vt-trace] flush L%d (%d,%d) nv=%lld 已有=%d lruVerts=%lld/%lld dataBytes=%llu\n",
                        lv, tx, ty, nv, had ? 1 : 0, lru.verts, lru.cap, (unsigned long long)cache.dataBytes());
            if (had) {
                uint32_t base = existing.vertexCount();
                existing.rings.reserve(existing.rings.size() + out.rings.size());
                existing.verts.reserve(existing.verts.size() + out.verts.size());
                for (const VtRing& r : out.rings) {
                    VtRing nr = r;
                    nr.firstVertex += base;
                    existing.rings.push_back(nr);
                }
                existing.verts.insert(existing.verts.end(), out.verts.begin(), out.verts.end());
                existing.originX = out.originX;
                existing.originY = out.originY;
                existing.epsg = out.epsg;
                cache.writeTile(lv, tx, ty, existing);
            } else {
                cache.writeTile(lv, tx, ty, out);
            }
            ++stats.tilesWritten;
            stats.storedVerts += nv;
            if (onTile) onTile(lv, tx, ty);
        }
        lru.verts -= nv;
        lru.remove(k);
    }

    // 超上限时淘汰最久未用的片(keep 除外)
    void evict(uint64_t keep) {
        if (trace && lru.verts > lru.cap)
            fprintf(stderr, "[vt-trace] evict 触发: lruVerts=%lld > cap=%lld 驻留片=%zu\n",
                    lru.verts, lru.cap, lru.order.size());
        while (lru.verts > lru.cap && lru.order.size() > 1) {
            uint64_t bk = lru.order.back();
            if (bk == keep) break;
            flushTile(bk);
        }
    }

    // 把一个源环路由到它覆盖的各层各瓦片
    void routeRing(const SourceRing& sr) {
        int rn0 = (int)(sr.xy.size() / 2);
        if (rn0 < 1) return;
        const std::vector<double>& rxy = sr.xy;
        int rn = (int)(rxy.size() / 2);
        if (rn < 1) return;

        double rminx = 1e300, rminy = 1e300, rmaxx = -1e300, rmaxy = -1e300;
        for (int i = 0; i < rn; ++i) {
            double x = rxy[2 * i], y = rxy[2 * i + 1];
            rminx = std::min(rminx, x); rmaxx = std::max(rmaxx, x);
            rminy = std::min(rminy, y); rmaxy = std::max(rmaxy, y);
        }
        if (hasBbox && (rmaxx < bx0 || rminx > bx1 || rmaxy < by0 || rminy > by1)) return;   // 诊断: 只处理指定范围

        GEOSGeometry* facePoly = (sr.type == RING_FACE) ? geosPolygon(geosCtx, rxy) : nullptr;
        // 线层: 在完整源线上算一次 VW 阈值, 各保留层按各自格距过滤(线独立要素, 不分叉)
        // 线: 在完整源线上算一次 VW 阈值(factor=1, 温和); 另丢掉"源 bbox 对角 < minFeatureCells 格"的短线。
        const bool doSimp = cfg.simplify && cfg.simplifyFactor > 0.0 && sr.type == RING_LINE && rn >= 3;
        if (doSimp) vwThresholdsWorld(rxy, kk_);
        const double srcDiag = std::hypot(rmaxx - rminx, rmaxy - rminy);
        const bool doFilter = sr.type == RING_LINE && cfg.minFeatureCells > 0.0;
        for (int L = Lmax; L >= 0; --L) {
            if (!levelKept(L, Lmax, cfg.levelStep)) continue;
            g_curLevel = L;
            const double cell = (S / (double)(1 << L)) / (double)tileSizeAt(L, Lmax);
            if (doFilter && srcDiag < cfg.minFeatureCells * cell) continue;   // 太短, 该层丢
            const SourceRing* use = &sr;
            if (doSimp) {
                filterByIntervalWorld(rxy, kk_, cfg.simplifyFactor * cell, simpXY_);
                if (simpXY_.size() < 4) continue;   // 该层抽到不足 2 点 -> 本层不画
                simpRing_.type = sr.type; simpRing_.hole = sr.hole;
                simpRing_.polyGroup = sr.polyGroup; simpRing_.featureIdx = sr.featureIdx;
                simpRing_.xy.swap(simpXY_);
                use = &simpRing_;
            }
            double bminx, bminy, bmaxx, bmaxy;
            ringBboxW(use->xy, bminx, bminy, bmaxx, bmaxy);
            routeRingLevel(L, *use, (int)(use->xy.size() / 2), bminx, bminy, bmaxx, bmaxy, facePoly);
        }
        if (facePoly) GEOSGeom_destroy_r(geosCtx, facePoly);

        ++stats.rings;
        stats.srcVerts += rn0;
        if (onProgress && (stats.rings % 2000) == 0) {
            int pct = featureCount > 0 ? (int)(sr.featureIdx * 50 / featureCount) : 0;
            onProgress(pct);
        }
    }

    // 单个层: 点直接落格; 线/面按环 bbox 覆盖的瓦片逐个裁剪+追加
    void routeRingLevel(int L, const SourceRing& sr, int rn,
                        double rminx, double rminy, double rmaxx, double rmaxy,
                        GEOSGeometry* facePoly) {
        const std::vector<double>& rxy = sr.xy;
        int n = 1 << L;
        double tileW = S / (double)n;
        double cell = tileW / (double)tileSizeAt(L, Lmax);
        const bool keepDeg = (L == Lmax);   // 最深层不丢退化面(无更细的层兜底)

        if (sr.type == RING_POINT) {
            double x = rxy[0], y = rxy[1];
            int tx = (int)std::floor((x - originX) / tileW);
            int ty = (int)std::floor((y - originY) / tileW);
            if (tx < 0 || tx >= n || ty < 0 || ty >= n) return;
            VtTile& t = tileAt(L, tx, ty);
            double ox = originX + tx * tileW, oy = originY + ty * tileW;
            t.originX = ox; t.originY = oy; t.epsg = cache.header().dstEpsg;
            long long before = (long long)t.vertexCount();
            appendRing(t, RING_POINT, 0, 0, rxy, ox, oy, cell);
            lru.verts += (long long)t.vertexCount() - before;
            evict(tileKey(L, tx, ty));
            return;
        }

        int tx0 = (int)std::floor((rminx - originX) / tileW);
        int tx1 = (int)std::floor((rmaxx - originX) / tileW);
        int ty0 = (int)std::floor((rminy - originY) / tileW);
        int ty1 = (int)std::floor((rmaxy - originY) / tileW);
        tx0 = std::max(0, std::min(n - 1, tx0));
        tx1 = std::max(0, std::min(n - 1, tx1));
        ty0 = std::max(0, std::min(n - 1, ty0));
        ty1 = std::max(0, std::min(n - 1, ty1));

        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                uint64_t k = tileKey(L, tx, ty);
                VtTile& t = tileAt(L, tx, ty);
                double ox = originX + tx * tileW, oy = originY + ty * tileW;
                t.originX = ox; t.originY = oy; t.epsg = cache.header().dstEpsg;
                double wx0 = ox - tilePadAt(L, Lmax) * cell, wy0 = oy - tilePadAt(L, Lmax) * cell;
                double wx1 = ox + tileSizeAt(L, Lmax) * cell + tilePadAt(L, Lmax) * cell;
                double wy1 = oy + tileSizeAt(L, Lmax) * cell + tilePadAt(L, Lmax) * cell;
                long long before = (long long)t.vertexCount();
                if (sr.type == RING_FACE) {
                    // 快路径: 环的包围盒完全落在本片(含 pad)内 -> 裁剪是恒等变换, 不必
                    // 调 GEOS。实测绝大多数(环,层)对只覆盖 1 片(平均 1.05 次/对), 原来
                    // 每个都对整个环做一次 GEOSClipByRect, 是阶段A 最大的一笔无谓开销。
                    // appendRing 只做量化+共线压缩+退化判定, 与环的起点/绕向无关,
                    // 所以直接传原始 rxy 与走 GEOS 的结果一致。
                    static const bool clipFast = std::getenv("PEEK_VT_NO_CLIPFAST") == nullptr;
                    if (clipFast && rminx >= wx0 && rmaxx <= wx1 && rminy >= wy0 && rmaxy <= wy1) {
                        VtScope _ta(vtTime().append, vtTime().nAppend);
                        vtAdd(vtTime().nClipFast, 1);
                        appendRing(t, RING_FACE, sr.hole, sr.polyGroup, rxy, ox, oy, cell, keepDeg);
                    } else if (facePoly) {
                        std::vector<std::vector<double>> parts;
                        {
                            VtScope _tc(vtTime().clip, vtTime().nClip);
                            geosClipRings(geosCtx, facePoly, wx0, wy0, wx1, wy1, parts);
                        }
                        for (auto& p : parts) {
                            VtScope _ta(vtTime().append, vtTime().nAppend);
                            appendRing(t, RING_FACE, sr.hole, sr.polyGroup, p, ox, oy, cell, keepDeg);
                        }
                    }
                } else {
                    for (int i = 0; i + 1 < rn; ++i) {
                        double a, b, c, d;
                        if (clipSegment(rxy[2 * i], rxy[2 * i + 1], rxy[2 * i + 2], rxy[2 * i + 3],
                                        wx0, wy0, wx1, wy1, a, b, c, d)) {
                            std::vector<double> seg = {a, b, c, d};
                            appendRing(t, RING_LINE, 0, 0, seg, ox, oy, cell);
                        }
                    }
                }
                lru.verts += (long long)t.vertexCount() - before;
                evict(k);
            }
        }
    }
};

}  // namespace

bool buildVtCache(const std::string& srcPath, int layerIdx, const std::string& cachePath,
                  const VtBuildConfig& cfg, VtBuildStats& stats,
                  const std::function<void(int, int, int)>& onTile,
                  const std::function<void(int)>& onProgress,
                  const std::function<void(int, int, int)>& onCover,
                  const std::function<void(int, int, int)>& onMerge) {
    auto t0 = std::chrono::steady_clock::now();
    resetDropCounters();   // 同进程可多次构建, 计数是「本次构建」的量
    LayerInfo li;
    if (!readVtLayerInfo(srcPath, layerIdx, li)) return false;

    int dstEpsg = cfg.dstEpsg > 0 ? cfg.dstEpsg : li.srcEpsg;
    double minx, miny, maxx, maxy;
    displayBbox(li, dstEpsg, minx, miny, maxx, maxy);
    double spanX = maxx - minx, spanY = maxy - miny;
    double S = std::max(spanX, spanY);
    if (S <= 0) S = 1.0;
    double originX = minx - (S - spanX) / 2;
    double originY = miny - (S - spanY) / 2;

    int Lmax = cfg.levels;
    VtLevelPick pick;
    if (Lmax < 0) {
        pick = pickVtLevel(srcPath, layerIdx, dstEpsg, cfg.errorFactor, cfg.targetVerts,
                           cfg.maxLevelCap, cfg.maxTotalVerts, cfg.maxVertsPerTile, cfg.levelStep);
        Lmax = pick.level;
        if (pick.errorMode) {
            spdlog::info("[vt] 选层(典型尺度): 源中位段长 {:.4f}°(≈{:.0f}m) -> Lmax={} (最深层格距 {:.4f}°≈{:.0f}m, 保留率 {:.0f}%, 每瓦片约 {:.0f} 顶点, 总量约 {:.1f}M)",
                         pick.nativeStep, pick.nativeStep * 111320.0,
                         Lmax, pick.cellAt, pick.cellAt * 111320.0,
                         pick.keepRatio * 100.0, pick.vertsPerTile, pick.totalVerts / 1e6);
            if (pick.clampedByVerts) {
                spdlog::warn("[vt] 超出体积上限(单片 {:.0f} 顶点 / 总量 {:.1f}M), 已压浅到 Lmax={}: 单片约 {:.0f} 顶点, 总量约 {:.1f}M",
                             (double)cfg.maxVertsPerTile, (double)cfg.maxTotalVerts / 1e6, Lmax,
                             pick.vertsPerTile, pick.totalVerts / 1e6);
            }
        } else {
            spdlog::info("[vt] 选层(顶点数驱动, 源无几何或 factor<=0): Lmax={} (格距 {:.6f}°, 每瓦片约 {:.0f} 顶点, 总量约 {:.1f}M)",
                         Lmax, pick.cellAt, pick.vertsPerTile, pick.totalVerts / 1e6);
        }
    } else {
        // 显式指定: 观感/性能由人决定, 不参与任何启发式。体积安全阀也不压它 ——
        // 要 L8 就是 L8, 顶多是 cap 拦一道(cap 默认 12, 平时不触发)。
        if (Lmax > cfg.maxLevelCap) {
            spdlog::warn("[vt] 指定 Lmax={} 超过上限 {}, 已压到 {}", Lmax, cfg.maxLevelCap, cfg.maxLevelCap);
            Lmax = cfg.maxLevelCap;
        }
        spdlog::info("[vt] 选层(显式指定): Lmax={} (格距 {:.6f}°, 不做体积预估)", Lmax, (S / std::pow(2.0, Lmax)) / tileSizeAt(Lmax, Lmax));
    }
    if (Lmax < 0) Lmax = 0;
    if (Lmax > cfg.maxLevelCap) Lmax = cfg.maxLevelCap;

    VtFileHeader h{};
    h.maxLevel = (uint32_t)Lmax;
    h.tileSize = TILE_SIZE;
    h.pad = TILE_PAD;
    h.fineTileSize = FINE_TILE_SIZE;
    h.srcEpsg = li.srcEpsg;
    h.dstEpsg = dstEpsg;
    h.minx = minx; h.miny = miny; h.maxx = maxx; h.maxy = maxy;
    h.originX = originX; h.originY = originY;
    h.tileW0 = S;
    h.srcHash = sourceHash(srcPath);
    {
        size_t s = srcPath.find_last_of("/\\");
        std::string bn = (s == std::string::npos) ? srcPath : srcPath.substr(s + 1);
        std::memset(h.srcName, 0, sizeof(h.srcName));
        std::strncpy(h.srcName, bn.c_str(), sizeof(h.srcName) - 1);
    }
    h.buildTime = (int64_t)std::time(nullptr);

    std::string dir = parentDir(cachePath);
    if (!dir.empty()) std::filesystem::create_directories(dir);

    VtCache cache;
    if (!cache.create(cachePath, h)) return false;

    GeosCtxGuard geosGuard;
    if (!geosGuard.ctx) return false;

    BuildState bs(cfg, cache, stats, onTile, onProgress, onCover, onMerge);
    bs.originX = originX; bs.originY = originY; bs.S = S;
    bs.Lmax = Lmax;
    bs.maxLv = (int)cache.header().maxLevel;   // 最深层不做合并(保住最细的碎面细节)
    bs.featureCount = li.featureCount;
    bs.geosCtx = geosGuard.ctx;
    bs.trace = std::getenv("PEEK_VT_TRACE") != nullptr;
    if (const char* bb = std::getenv("PEEK_VT_BBOX")) {
        double a, b, c, d;
        if (std::sscanf(bb, "%lf,%lf,%lf,%lf", &a, &b, &c, &d) == 4) {
            bs.hasBbox = true; bs.bx0 = a; bs.by0 = b; bs.bx1 = c; bs.by1 = d;
        }
    }
    bs.lru.cap = (long long)cfg.lruVerts;

    long long nf;
    {
        // 整段 = 读源+collectRing(重投影)+sink(建瓦片); sink 内累计 route, 相减即读源耗时
        auto _t0 = std::chrono::steady_clock::now();
        nf = streamVtRings(srcPath, layerIdx, dstEpsg, [&](const SourceRing& sr) {
            VtScope _tr(vtTime().route, vtTime().nRoute);
            bs.routeRing(sr);
        });
        vtAdd(vtTime().streamTotal, std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - _t0).count());
    }
    if (nf < 0) {   // streamVtRings 返回负值表示读源失败
        spdlog::error("[vt] 读取源失败, 中止构建: {}", srcPath);
        return false;
    }
    stats.features = nf;

    while (!bs.lru.order.empty()) bs.flushTile(bs.lru.order.back());
    spdlog::info("[vt-t] 阶段A(读源+建各保留层) {:.1f}s",
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

    // ---- 拓扑后处理: 每片内建 arc 拓扑 -> 小面并入邻面 -> 按 arc 抽稀(钉净区边界) ----
    // 最深层(Lmax, 1024 格)不处理: 容差极小、抽稀几乎无效果, 小面也几乎为零 —— 白干最耗时的部分。
    if (!std::getenv("PEEK_VT_NO_TOPO")) {
        auto tT0 = std::chrono::steady_clock::now();
        // 小面并入邻面: 拓扑局部拼接(mergeSmallFacesLocal)
        const double minFaceCells = cfg.simplify ? 4.0 : 0.0;   // 面积 < 4 格² 的小面并入邻面
        long long nProc = 0;
        // 阶段B 是纯 CPU(每片只读/只写自己那个 slot, 互不相干), 按行分给多线程。
        // 与阶段A 刻意不并行: 那里共用 LRU 和 geosCtx, 且并行读会丢可复现性。
        unsigned nThr = 1;
        if (const char* tp = std::getenv("PEEK_VT_TOPO_THREADS")) {
            int v = std::atoi(tp);
            if (v > 0) nThr = (unsigned)v;
        } else {
            nThr = std::max(1u, std::min(std::thread::hardware_concurrency(),
                                         (unsigned)vtTopoThreadCap()));
        }
        for (int L = 0; L < Lmax; ++L) {
            if (!levelKept(L, Lmax, cfg.levelStep)) continue;
            const int n = 1 << L;
            const int tsz = tileSizeAt(L, Lmax);
            const double cell = (S / (double)n) / (double)tsz;
            const double tol = (cfg.simplify && cfg.simplifyFactor > 0.0) ? cfg.simplifyFactor * 3.0 : 0.0;   // 面层 VW 容差(单位=格, 因为拓扑 VW 在整数格上算面积)
            // 本层要处理的片数少于一线程阈值时, 不值得开线程(省掉线程创建开销)
            const unsigned nthr = (nThr > 1 && (long long)n * n >= 8) ? nThr : 1u;
            std::atomic<long long> nProcL{0};
            // onMerge 回调(覆盖框上色)未必线程安全 -> 工作线程只记完成片, 主线程串行回调
            std::vector<std::pair<int, int>> doneTiles;
            std::mutex doneMtx;
            auto workRow = [&](int ty, std::atomic<long long>& proc,
                               std::vector<std::pair<int, int>>& done, std::mutex& m) {
                for (int tx = 0; tx < n; ++tx) {
                    VtTile t;
                    {
                        VtScopeNoCnt _tr(vtTime().topoRead);
                        if (!cache.readTile(L, tx, ty, t)) continue;
                    }
                    if (processTileTopology(t, tol, tsz, minFaceCells)) {
                        VtScopeNoCnt _tw(vtTime().topoWrite);
                        cache.writeTile(L, tx, ty, t);
                        proc.fetch_add(1, std::memory_order_relaxed);
                    }
                    // 记下"这一片已合并/糊化完"(不论是否被改写): 覆盖框据此把该片从
                    // "已建"色切到"已合并"色, 于是能看到合并逐片推进。
                    if (onMerge) { std::lock_guard<std::mutex> lk(m); done.emplace_back(tx, ty); }
                }
            };
            if (nthr <= 1) {
                std::vector<std::pair<int, int>> done;
                for (int ty = 0; ty < n; ++ty) workRow(ty, nProcL, done, doneMtx);
                if (onMerge) for (auto& q : done) onMerge(L, q.first, q.second);
            } else {
                std::vector<std::thread> ths;
                std::vector<std::vector<std::pair<int, int>>> tDone(nthr);
                std::vector<std::mutex> tMtx(nthr);
                for (unsigned k = 0; k < nthr; ++k) {
                    ths.emplace_back([&, k] {
                        for (int ty = (int)k; ty < n; ty += (int)nthr)
                            workRow(ty, nProcL, tDone[k], tMtx[k]);
                    });
                }
                for (auto& th : ths) th.join();
                if (onMerge)
                    for (unsigned k = 0; k < nthr; ++k)
                        for (auto& q : tDone[k]) onMerge(L, q.first, q.second);
            }
            nProc += nProcL.load();
            spdlog::info("[vt-t] 拓扑后处理 L{} 完成 ({:.1f}s, {} 线程)", L,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - tT0).count(),
                         (int)nthr);
            if (onProgress && Lmax > 0) onProgress(50 + (int)((long long)(L + 1) * 40 / Lmax));
        }
        spdlog::info("[vt-t] 拓扑后处理 {:.1f}s (改写 {} 片)",
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - tT0).count(), nProc);
    }

    for (int L = 0; L <= Lmax; ++L)
        if (levelKept(L, Lmax, cfg.levelStep)) cache.setFullyBuilt(L);

    // ---- 原地压实: 丢弃 LRU 淘汰重写留下的孤儿块并截断文件 ----
    // 用跨实例共享锁与渲染读端互斥(见 vt_cache 的 fileMtx_), 因此无需 temp+rename;
    // 后者在 Windows 上会因渲染端仍持有文件句柄而失败(ERROR_SHARING_VIOLATION)。
    if (onProgress) onProgress(90);
    if (!cache.finalize())
        spdlog::warn("[vt] 压实失败, 缓存仍可用但可能偏大: {}", cachePath);
    if (onProgress) onProgress(100);

    stats.dataBytes = cache.dataBytes();
    stats.maxLevel = Lmax;

    // 退化面诊断: 铺满型数据(街区/地块)丢一个面 = 底图一个洞。
    // Lmax 无更细的层兜底 -> 退化面已撑成 1 格保留(keepDegenerateFace), 这里只报"保留了多少";
    // 粗层照旧丢弃, 但同样要报出来, 否则"缩小时有洞"无从追查。
    for (int L = Lmax; L >= 0; --L) {
        long long da = g_dropArea[L], dv = g_dropVerts[L];
        if (L == Lmax) {
            if (da + dv > 0)
                spdlog::info("[vt] L{} (最深层) {} 个退化面已保留(撑成 1 格, 免得放大到底后成洞)", L, da + dv);
        } else {
            if (da + dv > 0)
                spdlog::info("[vt] L{} (粗层) 退化丢弃 {} 环(面积<1格² {} / 点数不足 {})",
                             L, da + dv, da, dv);
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    stats.seconds = std::chrono::duration<double>(t1 - t0).count();

    // ---- 耗时构成(PEEK_VT_TIMING=1) ----
    if (vtTime().on) {
        VtTimeAcc& A = vtTime();
        const double tStream = vtGet(A.streamTotal), tRoute = vtGet(A.route);
        const double tClip = vtGet(A.clip), tApp = vtGet(A.append), tFlush = vtGet(A.flush);
        const double readSrc = tStream - tRoute;   // 整段减去 sink = 纯读源+重投影
        spdlog::info("[vt-t] === 耗时构成(总 {:.1f}s) ===", stats.seconds);
        spdlog::info("[vt-t] 阶段A 读源+建层 {:.1f}s = 读源+重投影 {:.1f}s ({} 环) + 建瓦片 {:.1f}s",
                     tStream / 1000.0, readSrc / 1000.0, vtGet(A.nRoute), tRoute / 1000.0);
        spdlog::info("[vt-t]   建瓦片细分: GEOS裁剪 {:.1f}s ({} 次, 快路径 {} 次) + 量化写入 {:.1f}s ({} 次) + 落盘 {:.1f}s ({} 次) + 余量 {:.1f}s",
                     tClip / 1000.0, vtGet(A.nClip), vtGet(A.nClipFast),
                     tApp / 1000.0, vtGet(A.nAppend),
                     tFlush / 1000.0, vtGet(A.nFlush),
                     (tRoute - tClip - tApp - tFlush) / 1000.0);
        spdlog::info("[vt-t] 阶段B 拓扑后处理 {} 片: 读片 {:.1f}s + 建arc {:.1f}s + 并小面 {:.1f}s + 抽稀 {:.1f}s + 还原 {:.1f}s + 判孔 {:.1f}s + 写片 {:.1f}s",
                     vtGet(A.nTopo), vtGet(A.topoRead) / 1000.0, vtGet(A.topoBuild) / 1000.0,
                     vtGet(A.topoMerge) / 1000.0, vtGet(A.topoSimp) / 1000.0,
                     vtGet(A.topoRebuild) / 1000.0, vtGet(A.topoHoles) / 1000.0, vtGet(A.topoWrite) / 1000.0);
    }
    return true;
}

// ---- 原始数据直读流(超 Lmax 时用): 分块顺序扫源, 只路由可见区; 不抽稀, 保留原始细节 ----
// (vt_source.cpp 的匿名 collectRing 不可跨 TU 引用, 在此轻量复刻)
static void collectRingRaw(OGRGeometryH ring, OGRCoordinateTransformationH ct,
                           std::vector<double>& out) {
    int n = OGR_G_GetPointCount(ring);
    out.reserve(out.size() + (size_t)n * 2);
    for (int i = 0; i < n; ++i) {
        double x = OGR_G_GetX(ring, i);
        double y = OGR_G_GetY(ring, i);
        if (ct) OCTTransform(ct, 1, &x, &y, nullptr);
        out.push_back(x);
        out.push_back(y);
    }
}

RawRegionStream::RawRegionStream() = default;
RawRegionStream::~RawRegionStream() { close(); }

void RawRegionStream::close() {
    if (geosCtx_) { GEOS_finish_r((GEOSContextHandle_t)geosCtx_); geosCtx_ = nullptr; }
    if (ct_) { OCTDestroyCoordinateTransformation((OGRCoordinateTransformationH)ct_); ct_ = nullptr; }
    if (ds_) { GDALClose((GDALDatasetH)ds_); ds_ = nullptr; lyr_ = nullptr; }
    tiles_.clear();
}

bool RawRegionStream::open(const std::string& srcPath, int layerIdx, int dstEpsg,
                           int level, int maxLevel, double originX, double originY, double S,
                           double rx0, double ry0, double rx1, double ry1) {
    close();
    if (level <= maxLevel) return false;   // 直读只用于超出缓存最深层
    if (!(S > 0)) return false;
    ensureGdal();
    GDALDatasetH ds = gdalOpenVector(srcPath);
    if (!ds) return false;
    int nl = GDALDatasetGetLayerCount(ds);
    if (layerIdx < 0 || layerIdx >= nl) { GDALClose(ds); return false; }
    OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);

    OGRSpatialReferenceH srcSrs = OGR_L_GetSpatialRef(lyr);
    int srcEpsg = gdalSrsEpsg(srcSrs);
    // 直读只取几何、跳过属性列(不读 DBF)。大属性表时(如人口普查块)属性读是主要耗时:
    // 实测 66 万要素 1844 命中, 不忽略 305ms / 忽略后 57ms。必须在计数/遍历前设置才生效。
    {
        const char* ignored[] = {"*", nullptr};
        OGR_L_SetIgnoredFields(lyr, ignored);
    }
    int dst = dstEpsg > 0 ? dstEpsg : srcEpsg;
    OGRCoordinateTransformationH ct = nullptr;
    if (dst > 0 && srcEpsg > 0 && dst != srcEpsg && srcSrs) {
        OGRSpatialReferenceH d = OSRNewSpatialReference(nullptr);
        if (OSRImportFromEPSG(d, dst) == OGRERR_NONE)
            ct = OCTNewCoordinateTransformation(srcSrs, d);
        OSRDestroySpatialReference(d);
    }

    // 可见区域 -> 源 CRS 坐标。能投影就用 OGR 空间过滤: 走到物理/逻辑空间索引(如 .qix),
    // 只顺序读命中的要素 —— 这正是"放到深层读可见区子集很快"的前提, 否则整表扫描必然超预算。
    // scanned/featureCount/EWMA 全部按可见区子集度量(不再有"整表计数"语义)。
    double sbx0 = -1e300, sby0 = -1e300, sbx1 = 1e300, sby1 = 1e300;
    bool useFilter = false;
    if (srcEpsg > 0) {
        sbx0 = 1e300; sby0 = 1e300; sbx1 = -1e300; sby1 = -1e300;
        double fx[5] = {rx0, rx1, rx0, rx1, (rx0 + rx1) * 0.5};
        double fy[5] = {ry0, ry0, ry1, ry1, (ry0 + ry1) * 0.5};
        bool fok = true;
        for (int i = 0; i < 5; ++i) {
            double ox = fx[i], oy = fy[i];
            if (dst != srcEpsg) {
                if (!reprojectPoint(fx[i], fy[i], dst, srcEpsg, ox, oy)) { fok = false; break; }
            }
            sbx0 = std::min(sbx0, ox); sbx1 = std::max(sbx1, ox);
            sby0 = std::min(sby0, oy); sby1 = std::max(sby1, oy);
        }
        if (fok) {
            double mx = (sbx1 - sbx0) * 0.05, my = (sby1 - sby0) * 0.05;
            sbx0 -= mx; sbx1 += mx; sby0 -= my; sby1 += my;
            useFilter = true;
        } else {
            sbx0 = -1e300; sby0 = -1e300; sbx1 = 1e300; sby1 = 1e300;
        }
    }

    // 空间过滤能否快速执行(GDAL 标准能力检测, 须在 SetSpatialFilterRect 之前判断):
    // shapefile 无 .qix 时 OLCFastSpatialFilter=0(过滤退化为全表顺序扫描, 66万要素的过滤
    // 计数就要 ~12s), gpkg 无扩展索引同理。有索引的源才值得走超Lmax直读。
    // 实测: 48 号有 .qix -> 1, 55 号无 -> 0。能力位对 shp/gpkg 都正确。
    // 豁免"本来就很小的数据源"(如单测内存 GeoJSON): 整表也很小(毫秒级全扫), 直读无性能风险。
    // 注意: 过滤设置后 GetFeatureCount 无论 TRUE/FALSE 都返回命中子集, 所以整表计数要在过滤前做。
    long long totalFeatures = -1;
    if (useFilter && !OGR_L_TestCapability(lyr, OLCFastSpatialFilter)) {
        totalFeatures = (long long)OGR_L_GetFeatureCount(lyr, FALSE);   // 读元数据(如 .shx), 毫秒级
        if (totalFeatures > 20000) {
            spdlog::info("[vt] 源无空间索引(OLCFastSpatialFilter=0)且整表 {} 要素, 超Lmax直读不可用: {}", totalFeatures, srcPath);
            GDALClose(ds);
            return false;
        }
    }

    if (useFilter) OGR_L_SetSpatialFilterRect(lyr, sbx0, sby0, sbx1, sby1);

    // 过滤后再计数: 要素数 = 可见区命中子集(shapefile 有 .qix 时空过滤计数走空间索引, 秒回)。
    // 必须 bForce=TRUE: FALSE 可能返回未过滤的缓存总数(如 shapefile), 那样投影永远以为还有 39 万
    // 要素没读而判死回退, 直读等同于从未启用。
    long long total = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
    if (total < 0) total = (long long)OGR_L_GetFeatureCount(lyr, FALSE);

    ds_ = ds; lyr_ = lyr; ct_ = ct;
    geosCtx_ = geosInit();
    featureCount_ = total;
    level_ = level; maxLevel_ = maxLevel;
    originX_ = originX; originY_ = originY; S_ = S;
    epsg_ = dst;
    tileW_ = S / (double)(1LL << level);
    cell_ = tileW_ / (double)tileSizeAt(level, maxLevel);
    sbx0_ = sbx0; sby0_ = sby0; sbx1_ = sbx1; sby1_ = sby1;

    int n = 1 << level;
    vtx0_ = (int)std::floor((rx0 - originX) / tileW_);
    vtx1_ = (int)std::floor((rx1 - originX) / tileW_);
    vty0_ = (int)std::floor((ry0 - originY) / tileW_);
    vty1_ = (int)std::floor((ry1 - originY) / tileW_);
    vtx0_ = std::max(0, std::min(n - 1, vtx0_));
    vtx1_ = std::max(0, std::min(n - 1, vtx1_));
    vty0_ = std::max(0, std::min(n - 1, vty0_));
    vty1_ = std::max(0, std::min(n - 1, vty1_));
    if (vtx1_ < vtx0_ || vty1_ < vty0_) { close(); return false; }
    return true;
}

// 读一块: 逐要素遍历, 已路由即进 tiles_。scanned = 本块实际读盘要素数(全表顺序读, 手动判区域)。
void RawRegionStream::chunk(long long maxFeatures, double maxMs,
                            long long& scanned, double& ms, bool& done) {
    scanned = 0; ms = 0; done = false;
    if (!ds_) { done = true; return; }
    auto t0 = std::chrono::steady_clock::now();
    uint32_t polyCounter = 0;
    for (; scanned < maxFeatures; ++scanned) {
        OGRFeatureH f = OGR_L_GetNextFeature((OGRLayerH)lyr_);
        if (!f) { done = true; break; }
        OGRGeometryH g = OGR_F_GetGeometryRef(f);
        if (g) addFeatureGeom(g, polyCounter, scanned);
        OGR_F_Destroy(f);
        if (scanned % 512 == 0) {
            double el = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            if (el >= maxMs) break;
        }
    }
    ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

void RawRegionStream::addFeatureGeom(void* g, uint32_t& polyCounter, long long featureIdx) {
    OGRGeometryH gh = (OGRGeometryH)g;
    // 源 CRS 快速判定: 与可见区(源坐标)不相交的要素整条跳过(不重投影/不路由)
    if (sbx1_ > sbx0_) {
        OGREnvelope e;
        OGR_G_GetEnvelope(gh, &e);
        if (e.MaxX < sbx0_ || e.MinX > sbx1_ || e.MaxY < sby0_ || e.MinY > sby1_) return;
    }
    OGRwkbGeometryType t = wkbFlatten(OGR_G_GetGeometryType(gh));
    switch (t) {
        case wkbPolygon: {
            uint32_t pg = ++polyCounter;
            int nr = OGR_G_GetGeometryCount(gh);
            for (int r = 0; r < nr; ++r) {
                SourceRing sr;
                sr.type = RING_FACE;
                sr.hole = (r == 0) ? 0 : 1;
                sr.polyGroup = pg;
                sr.featureIdx = featureIdx;
                collectRingRaw(OGR_G_GetGeometryRef(gh, r), (OGRCoordinateTransformationH)ct_, sr.xy);
                if (sr.xy.size() >= 6) routeRing(sr, cell_);
            }
            break;
        }
        case wkbLineString:
        case wkbLinearRing: {
            SourceRing sr;
            sr.type = RING_LINE;
            sr.featureIdx = featureIdx;
            collectRingRaw(gh, (OGRCoordinateTransformationH)ct_, sr.xy);
            if (sr.xy.size() >= 4) routeRing(sr, cell_);
            break;
        }
        case wkbPoint: {
            double x = OGR_G_GetX(gh, 0), y = OGR_G_GetY(gh, 0);
            if (ct_) OCTTransform((OGRCoordinateTransformationH)ct_, 1, &x, &y, nullptr);
            SourceRing sr;
            sr.type = RING_POINT;
            sr.featureIdx = featureIdx;
            sr.xy = {x, y};
            routeRing(sr, cell_);
            break;
        }
        case wkbMultiPoint:
        case wkbMultiPolygon:
        case wkbMultiLineString:
        case wkbGeometryCollection: {
            int ng = OGR_G_GetGeometryCount(gh);
            for (int i = 0; i < ng; ++i)
                addFeatureGeom(OGR_G_GetGeometryRef(gh, i), polyCounter, featureIdx);
            break;
        }
        // 曲线几何: 容器下钻, 叶子线性化(否则全落 default 整条丢弃 -> 直读也是空的)
        case wkbCompoundCurve:
        case wkbMultiCurve:
        case wkbMultiSurface: {
            int ng = OGR_G_GetGeometryCount(gh);
            for (int i = 0; i < ng; ++i)
                addFeatureGeom(OGR_G_GetGeometryRef(gh, i), polyCounter, featureIdx);
            break;
        }
        case wkbCircularString:
        case wkbCurvePolygon:
        case wkbCurve:
        case wkbSurface: {
            OGRGeometryH lin = OGR_G_GetLinearGeometry(gh, 0.0, nullptr);
            if (lin) {
                if (!OGR_G_HasCurveGeometry(lin, FALSE))   // 防递归: 线性化结果必须是线性类型
                    addFeatureGeom(lin, polyCounter, featureIdx);
                OGR_G_DestroyGeometry(lin);
            }
            break;
        }
        default:
            break;
    }
}

// 单个直读层: 点直接落格; 线/面按环 bbox 与可见区相交的瓦片逐个裁剪+追加(与构建端同格, 保证片对齐)
void RawRegionStream::routeRing(const SourceRing& sr, double cell) {
    const std::vector<double>& rxy = sr.xy;
    int rn = (int)(rxy.size() / 2);
    if (rn < 1) return;
    double rminx = 1e300, rminy = 1e300, rmaxx = -1e300, rmaxy = -1e300;
    for (int i = 0; i < rn; ++i) {
        double x = rxy[2 * i], y = rxy[2 * i + 1];
        rminx = std::min(rminx, x); rmaxx = std::max(rmaxx, x);
        rminy = std::min(rminy, y); rmaxy = std::max(rmaxy, y);
    }
    if (rmaxx < originX_ + vtx0_ * tileW_ - tileW_ || rminx > originX_ + (vtx1_ + 1) * tileW_ + tileW_ ||
        rmaxy < originY_ + vty0_ * tileW_ - tileW_ || rminy > originY_ + (vty1_ + 1) * tileW_ + tileW_)
        return;                              // 与可见区扩展片无交(加 1 片余量防误差)

    GEOSGeometry* facePoly = (sr.type == RING_FACE)
        ? geosPolygon((GEOSContextHandle_t)geosCtx_, rxy) : nullptr;
    int n = 1 << level_;
    int tx0 = (int)std::floor((rminx - originX_) / tileW_);
    int tx1 = (int)std::floor((rmaxx - originX_) / tileW_);
    int ty0 = (int)std::floor((rminy - originY_) / tileW_);
    int ty1 = (int)std::floor((rmaxy - originY_) / tileW_);
    tx0 = std::max(vtx0_, std::min(n - 1, tx0));
    tx1 = std::max(vtx0_, std::min(vtx1_, tx1));
    ty0 = std::max(vty0_, std::min(n - 1, ty0));
    ty1 = std::max(vty0_, std::min(vty1_, ty1));

    if (sr.type == RING_POINT) {
        int tx = (int)std::floor((rxy[0] - originX_) / tileW_);
        int ty = (int)std::floor((rxy[1] - originY_) / tileW_);
        if (tx < vtx0_ || tx > vtx1_ || ty < vty0_ || ty > vty1_) { if (facePoly) GEOSGeom_destroy_r((GEOSContextHandle_t)geosCtx_, facePoly); return; }
        VtTile& t = tiles_[tileKey(level_, tx, ty)];
        t.originX = originX_ + tx * tileW_; t.originY = originY_ + ty * tileW_;
        t.epsg = epsg_;
        appendRingWorld(t, RING_POINT, 0, 0, rxy);
        if (facePoly) GEOSGeom_destroy_r((GEOSContextHandle_t)geosCtx_, facePoly);
        return;
    }

    if (tx1 < tx0 || ty1 < ty0) { if (facePoly) GEOSGeom_destroy_r((GEOSContextHandle_t)geosCtx_, facePoly); return; }
    double pad = tilePadAt(level_, maxLevel_);
    double tsz = tileSizeAt(level_, maxLevel_);
    g_curLevel = level_;   // 直读层也可能退化, 诊断要按它自己的层号归位
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            double ox = originX_ + tx * tileW_, oy = originY_ + ty * tileW_;
            double wx0 = ox - pad * cell, wy0 = oy - pad * cell;
            double wx1 = ox + tsz * cell + pad * cell, wy1 = oy + tsz * cell + pad * cell;
            VtTile& t = tiles_[tileKey(level_, tx, ty)];
            t.originX = ox; t.originY = oy; t.epsg = epsg_;
            if (sr.type == RING_FACE) {
                if (facePoly) {
                    std::vector<std::vector<double>> parts;
                    geosClipRings((GEOSContextHandle_t)geosCtx_, facePoly, wx0, wy0, wx1, wy1, parts);
                    for (auto& p : parts)
                        appendRingWorld(t, RING_FACE, sr.hole, sr.polyGroup, p);
                }
            } else {
                for (int i = 0; i + 1 < rn; ++i) {
                    double a, b, c, d;
                    if (clipSegment(rxy[2 * i], rxy[2 * i + 1], rxy[2 * i + 2], rxy[2 * i + 3],
                                    wx0, wy0, wx1, wy1, a, b, c, d)) {
                        std::vector<double> seg = {a, b, c, d};
                        appendRingWorld(t, RING_LINE, 0, 0, seg);
                    }
                }
            }
        }
    }
    if (facePoly) GEOSGeom_destroy_r((GEOSContextHandle_t)geosCtx_, facePoly);
}

void RawRegionStream::takeTiles(std::vector<std::pair<uint64_t, VtTile>>& out) {
    out.reserve(out.size() + tiles_.size());
    for (auto& kv : tiles_) out.push_back(std::move(kv));
    tiles_.clear();
}

}  // namespace peekg::vt
