#include "vt/vt_build.h"
#include "vt/vt_cache.h"
#include "vt/vt_source.h"
#include "data/gdal_common.h"
#include "data/reproject.h"

#include <ogr_api.h>
#include <geos_c.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
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
    GEOSGeometry* res = GEOSClipByRect_r(ctx, (GEOSGeometry*)poly, xmin, ymin, xmax, ymax);
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
void appendRing(VtTile& t, uint8_t type, uint8_t hole, uint32_t polyGroup,
                const std::vector<double>& xyDisp, double originX, double originY, double cell) {
    int n = (int)(xyDisp.size() / 2);
    if (n < 1) return;
    std::vector<int16_t> q;
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
    // 共线点压缩(精确整数, 不改形状): 中间点落在前后点连线上则去掉。
    // 共享边内部点两边上下文相同 -> 压缩结果一致, 不漏风。
    if (type != RING_POINT && q.size() >= 6) {
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
    if (type == RING_POINT) {
        if (q.size() < 2) return;
    } else if (type == RING_LINE) {
        if (q.size() < 4) return;
    } else {
        if (q.size() < 6) return;
        int m = (int)(q.size() / 2);
        double area = 0;
        for (int i = 0; i < m; ++i) {
            int j = (i + 1) % m;
            area += (double)q[2*i] * q[2*j+1] - (double)q[2*j] * q[2*i+1];
        }
        if (std::fabs(area) < 1.0) return;
    }
    VtRing r;
    r.type = type; r.hole = hole; r.polyGroup = polyGroup;
    r.firstVertex = t.vertexCount();
    r.vertexCount = (uint32_t)(q.size() / 2);
    t.verts.insert(t.verts.end(), q.begin(), q.end());
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

static bool mergeSmallFaces(VtTile& t, const FaceMergeCfg& cfg) {
    if (t.rings.size() < 2) return false;
    struct S { uint32_t ri; int gx, gy; };
    std::vector<S> small;
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
        std::unordered_map<uint64_t, std::vector<uint64_t>> adj;
        std::unordered_map<uint64_t, int> used;
        for (auto& e : ecnt) {
            if (e.second != 1) continue;
            used[e.first] = 0;
            uint64_t a = e.first >> 32, b = e.first & 0xffffffffull;
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
        if (adj.empty()) continue;
        std::vector<std::vector<int16_t>> got;
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
        if (got.empty()) continue;
        for (uint32_t si : kv.second) drop[small[si].ri] = 1;   // 按"环下标"标记, 不能用 firstVertex
        for (auto& g : got) merged.push_back(std::move(g));
    }
    if (merged.empty()) return false;

    // 重建: 保留未丢弃的环 + 追加合并环
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

int estimateMaxLevel(const std::string& srcPath, int layerIdx, int dstEpsg,
                     int targetVerts, int cap) {
    ensureGdal();
    GDALDatasetH ds = gdalOpenVector(srcPath);
    if (!ds) return -1;
    int nl = GDALDatasetGetLayerCount(ds);
    if (layerIdx < 0 || layerIdx >= nl) { GDALClose(ds); return -1; }
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
    if (F <= 0) { if (ct) OCTDestroyCoordinateTransformation(ct); GDALClose(ds); return 0; }

    double spanX = maxx - minx, spanY = maxy - miny;
    double originX = minx - (S - spanX) / 2;
    double originY = miny - (S - spanY) / 2;

    int C = std::max(0, cap);
    std::vector<double> cell(C + 1);
    for (int L = 0; L <= C; ++L) cell[L] = S / (512.0 * std::pow(2.0, L));

    // 与阶段A一致的"格化"计数: 量化到 cell 网格 + 连续去重(不是 Douglas-Peucker)。
    auto snapRing = [](OGRGeometryH ring, double ox, double oy, double c) -> long long {
        int n = OGR_G_GetPointCount(ring);
        long long cnt = 0;
        long px = 0, py = 0;
        bool has = false;
        for (int i = 0; i < n; ++i) {
            long gx = std::lround((OGR_G_GetX(ring, i) - ox) / c);
            long gy = std::lround((OGR_G_GetY(ring, i) - oy) / c);
            if (has && gx == px && gy == py) continue;
            ++cnt; px = gx; py = gy; has = true;
        }
        return cnt;
    };
    std::function<long long(OGRGeometryH, double, double, double)> snapGeom;
    snapGeom = [&](OGRGeometryH g, double ox, double oy, double c) -> long long {
        if (!g) return 0;
        OGRwkbGeometryType t = wkbFlatten(OGR_G_GetGeometryType(g));
        switch (t) {
            case wkbPolygon: {
                long long s = 0;
                int nr = OGR_G_GetGeometryCount(g);
                for (int r = 0; r < nr; ++r) {
                    long long k = snapRing(OGR_G_GetGeometryRef(g, r), ox, oy, c);
                    if (k >= 3) s += k;
                }
                return s;
            }
            case wkbLineString:
            case wkbLinearRing: {
                long long k = snapRing(g, ox, oy, c);
                return k >= 2 ? k : 0;
            }
            case wkbPoint:
                return 1;
            case wkbMultiPoint:
            case wkbMultiPolygon:
            case wkbMultiLineString:
            case wkbGeometryCollection: {
                long long s = 0;
                int ng = OGR_G_GetGeometryCount(g);
                for (int i = 0; i < ng; ++i) s += snapGeom(OGR_G_GetGeometryRef(g, i), ox, oy, c);
                return s;
            }
            default:
                return 0;
        }
    };

    // 只取前 sampleK 个要素估计(遍历全表在千万级上要几十秒, 会让"建文件"迟迟不发生)
    const long long sampleK = 50000;
    std::vector<double> sumS(C + 1, 0.0);
    long long n = 0;
    OGR_L_ResetReading(lyr);
    OGRFeatureH f;
    while (n < sampleK && (f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        OGRGeometryH g = OGR_F_GetGeometryRef(f);
        if (g) {
            OGRGeometryH gg = g;
            OGRGeometryH owned = nullptr;
            if (ct) { owned = OGR_G_Clone(g); OGR_G_Transform(owned, ct); gg = owned; }
            for (int L = 0; L <= C; ++L)
                sumS[L] += (double)snapGeom(gg, originX, originY, cell[L]);
            if (owned) OGR_G_DestroyGeometry(owned);
            ++n;
        }
        OGR_F_Destroy(f);
    }
    if (ct) OCTDestroyCoordinateTransformation(ct);
    GDALClose(ds);
    if (n == 0) return 0;

    int best = 0;
    double bestErr = 1e300;
    for (int L = 0; L <= C; ++L) {
        double P = (sumS[L] / (double)n) * (double)F;   // 该层总点数估计
        double r = P / std::pow(4.0, L);
        double err = std::fabs(r - (double)targetVerts);
        if (err < bestErr) { bestErr = err; best = L; }
    }
    return best;
}

bool buildVtCache(const std::string& srcPath, int layerIdx, const std::string& cachePath,
                  const VtBuildConfig& cfg, VtBuildStats& stats,
                  const std::function<void(int, int, int)>& onTile,
                  const std::function<void(int)>& onProgress) {
    auto t0 = std::chrono::steady_clock::now();
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

    int Lmax = cfg.levels >= 0 ? cfg.levels
                               : estimateMaxLevel(srcPath, layerIdx, dstEpsg,
                                                  cfg.targetVerts, cfg.maxLevelCap);
    if (Lmax < 0) Lmax = 0;
    if (Lmax > cfg.maxLevelCap) Lmax = cfg.maxLevelCap;

    VtFileHeader h{};
    h.maxLevel = (uint32_t)Lmax;
    h.tileSize = TILE_SIZE;
    h.pad = TILE_PAD;
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
    GEOSContextHandle_t geosCtx = geosGuard.ctx;
    if (!geosCtx) return false;

    // ---- 逐层直接从源裁切(无层间合并): 人工裁切边只落在 10 格扩边里, scissor 裁掉 ----
    Lru lru;
    lru.cap = (long long)cfg.lruVerts;

    // 淘汰时"读盘-合并-写回": 瓦片被淘汰后又被后续要素触达时, 不能覆盖丢数据。
    FaceMergeCfg fmcfg;   // 小面合并参数(格² / 聚合格)
    int maxLv = (int)cache.header().maxLevel;   // 最深层不做合并(保住最细的碎面细节)
    auto flushTile = [&](uint64_t k) {
        auto f = lru.tiles.find(k);
        if (f == lru.tiles.end()) return;
        long long nv = (long long)f->second.vertexCount();
        if (!f->second.empty()) {
            int lv = keyLevel(k), tx = keyTx(k), ty = keyTy(k);
            VtTile out = std::move(f->second);
            VtTile existing;
            bool had = cache.readTile(lv, tx, ty, existing);
            if (getenv("PEEK_VT_TRACE")) fprintf(stderr, "[vt-trace] flush L%d (%d,%d) nv=%lld 已有=%d lruVerts=%lld/%lld dataBytes=%llu\n", lv, tx, ty, nv, had?1:0, lru.verts, lru.cap, (unsigned long long)cache.dataBytes());
            if (had) {
                uint32_t base = existing.vertexCount();
                for (const VtRing& r : out.rings) {
                    VtRing nr = r;
                    nr.firstVertex += base;
                    existing.rings.push_back(nr);
                }
                existing.verts.insert(existing.verts.end(), out.verts.begin(), out.verts.end());
                existing.originX = out.originX;
                existing.originY = out.originY;
                existing.epsg = out.epsg;
                if (lv < maxLv) mergeSmallFaces(existing, fmcfg);
                cache.writeTile(lv, tx, ty, existing);
            } else {
                if (lv < maxLv) mergeSmallFaces(out, fmcfg);
                cache.writeTile(lv, tx, ty, out);
            }
            ++stats.tilesWritten;
            stats.storedVerts += nv;
            if (onTile) onTile(lv, tx, ty);
        }
        lru.verts -= nv;
        lru.remove(k);
    };
    auto evict = [&](uint64_t keep) {
        if (getenv("PEEK_VT_TRACE") && lru.verts > lru.cap) fprintf(stderr, "[vt-trace] evict 触发: lruVerts=%lld > cap=%lld 驻留片=%zu\n", lru.verts, lru.cap, lru.order.size());
        while (lru.verts > lru.cap && lru.order.size() > 1) {
            uint64_t bk = lru.order.back();
            if (bk == keep) break;
            flushTile(bk);
        }
    };

    long long nf = streamVtRings(srcPath, layerIdx, dstEpsg, [&](const SourceRing& sr) {
        int rn0 = (int)(sr.xy.size() / 2);
        if (rn0 < 1) return;
        int rnOrig = rn0;
        // 不抽稀: 共享边两边坐标完全相同 -> 量化到同一批整数格 -> 无缝
        const std::vector<double>& rxy = sr.xy;
        int rn = (int)(rxy.size() / 2);
        if (rn < 1) return;

        double rminx = 1e300, rminy = 1e300, rmaxx = -1e300, rmaxy = -1e300;
        for (int i = 0; i < rn; ++i) {
            double x = rxy[2*i], y = rxy[2*i+1];
            rminx = std::min(rminx, x); rmaxx = std::max(rmaxx, x);
            rminy = std::min(rminy, y); rmaxy = std::max(rmaxy, y);
        }

        GEOSGeometry* facePoly = (sr.type == RING_FACE) ? geosPolygon(geosCtx, rxy) : nullptr;
        for (int L = Lmax; L >= 0; --L) {
            int n = 1 << L;
            double tileW = S / (double)n;
            double cell = tileW / (double)TILE_SIZE;

            if (sr.type == RING_POINT) {
                double x = rxy[0], y = rxy[1];
                int tx = (int)std::floor((x - originX) / tileW);
                int ty = (int)std::floor((y - originY) / tileW);
                if (tx < 0 || tx >= n || ty < 0 || ty >= n) continue;
                uint64_t k = tileKey(L, tx, ty);
                VtTile& t = lru.get(k);
                double ox = originX + tx * tileW, oy = originY + ty * tileW;
                t.originX = ox; t.originY = oy; t.epsg = dstEpsg;
                long long before = (long long)t.vertexCount();
                appendRing(t, RING_POINT, 0, 0, rxy, ox, oy, cell);
                lru.verts += (long long)t.vertexCount() - before;
                evict(k);
                continue;
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
                    VtTile& t = lru.get(k);
                    double ox = originX + tx * tileW, oy = originY + ty * tileW;
                    t.originX = ox; t.originY = oy; t.epsg = dstEpsg;
                    double wx0 = ox - TILE_PAD * cell, wy0 = oy - TILE_PAD * cell;
                    double wx1 = ox + TILE_SIZE * cell + TILE_PAD * cell;
                    double wy1 = oy + TILE_SIZE * cell + TILE_PAD * cell;
                    long long before = (long long)t.vertexCount();
                    if (sr.type == RING_FACE) {
                        if (facePoly) {
                            std::vector<std::vector<double>> parts;
                            geosClipRings(geosCtx, facePoly, wx0, wy0, wx1, wy1, parts);
                            for (auto& p : parts)
                                appendRing(t, RING_FACE, sr.hole, sr.polyGroup, p, ox, oy, cell);
                        }
                    } else {
                        for (int i = 0; i + 1 < rn; ++i) {
                            double a, b, c, d;
                            if (clipSegment(rxy[2*i], rxy[2*i+1], rxy[2*i+2], rxy[2*i+3],
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
        if (facePoly) GEOSGeom_destroy_r(geosCtx, facePoly);

        ++stats.rings;
        stats.srcVerts += rnOrig;
        if (onProgress && (stats.rings % 2000) == 0) {
            int pct = li.featureCount > 0 ? (int)(sr.featureIdx * 50 / li.featureCount) : 0;
            onProgress(pct);
        }
    });
    stats.features = nf > 0 ? nf : 0;

    while (!lru.order.empty()) flushTile(lru.order.back());
    for (int L = 0; L <= Lmax; ++L) cache.setFullyBuilt(L);

    // ---- 压缩重写: 丢弃 LRU 淘汰重写产生的孤儿块(否则文件被写放大到数倍) ----
    {
        std::string tmp = cachePath + ".compact";
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        VtCache dst;
        VtFileHeader h2 = h;
        bool ok = dst.create(tmp, h2);
        if (ok) {
            for (int L = 0; L <= Lmax; ++L) {
                int n = 1 << L;
                for (int ty = 0; ty < n && ok; ++ty)
                    for (int tx = 0; tx < n; ++tx) {
                        VtTile t;
                        if (cache.readTile(L, tx, ty, t)) dst.writeTile(L, tx, ty, t);
                    }
                dst.setFullyBuilt(L);
                if (onProgress) onProgress(90 + (L + 1) * 10 / (Lmax + 1));
            }
            dst.finalize();
        }
        dst.close();
        cache.close();
        if (ok) {
            std::filesystem::remove(cachePath, ec);
            std::filesystem::rename(tmp, cachePath, ec);
        }
    }

    if (onProgress) onProgress(100);
    cache.finalize();
    stats.dataBytes = cache.dataBytes();
    stats.maxLevel = Lmax;
    auto t1 = std::chrono::steady_clock::now();
    stats.seconds = std::chrono::duration<double>(t1 - t0).count();
    return true;
}

}  // namespace peekg::vt
