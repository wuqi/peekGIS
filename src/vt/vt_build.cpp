#include "vt/vt_build.h"
#include "vt/vt_cache.h"
#include "vt/vt_source.h"
#include "data/gdal_common.h"
#include "data/reproject.h"

#include <ogr_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <list>
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

void clipPolyRect(const std::vector<double>& in, double xmin, double ymin, double xmax, double ymax,
                  std::vector<double>& out) {
    out.clear();
    if (in.size() < 6) return;
    std::vector<double> cur = in;
    for (int edge = 0; edge < 4; ++edge) {
        std::vector<double> res;
        int m = (int)(cur.size() / 2);
        if (m < 1) break;
        auto inside = [&](double x, double y) -> bool {
            switch (edge) {
                case 0: return x >= xmin;
                case 1: return x <= xmax;
                case 2: return y >= ymin;
                default: return y <= ymax;
            }
        };
        auto inter = [&](double x1, double y1, double x2, double y2, double& ix, double& iy) {
            if (edge == 0) { ix = xmin; iy = y1 + (y2 - y1) * (xmin - x1) / (x2 - x1); }
            else if (edge == 1) { ix = xmax; iy = y1 + (y2 - y1) * (xmax - x1) / (x2 - x1); }
            else if (edge == 2) { iy = ymin; ix = x1 + (x2 - x1) * (ymin - y1) / (y2 - y1); }
            else { iy = ymax; ix = x1 + (x2 - x1) * (ymax - y1) / (y2 - y1); }
        };
        for (int i = 0; i < m; ++i) {
            double x1 = cur[2*i], y1 = cur[2*i+1];
            int j = (i + 1) % m;
            double x2 = cur[2*j], y2 = cur[2*j+1];
            bool in1 = inside(x1, y1), in2 = inside(x2, y2);
            if (in1) {
                res.push_back(x1); res.push_back(y1);
                if (!in2) { double ix, iy; inter(x1, y1, x2, y2, ix, iy); res.push_back(ix); res.push_back(iy); }
            } else if (in2) {
                double ix, iy; inter(x1, y1, x2, y2, ix, iy); res.push_back(ix); res.push_back(iy);
                res.push_back(x2); res.push_back(y2);
            }
        }
        cur.swap(res);
        if (cur.empty()) break;
    }
    out.swap(cur);
}

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

// 近共线点抽稀(类 Douglas-Peucker 的一次遍历版, 可迭代)。tol 为该层格距。
// 闭合环按环处理(首尾相连), 开放折线保留两端点。用于按层降采样, 显著降低粗层顶点。
void simplifyPolyline(std::vector<double>& xy, double tol, bool closed) {
    if (tol <= 0) return;
    int n = (int)(xy.size() / 2);
    if (n < 3) return;
    for (int iter = 0; iter < 8; ++iter) {
        int m = (int)(xy.size() / 2);
        if (m < 3) break;
        std::vector<double> out;
        out.reserve(xy.size());
        for (int i = 0; i < m; ++i) {
            if (!closed && (i == 0 || i == m - 1)) {
                out.push_back(xy[2*i]); out.push_back(xy[2*i+1]);
                continue;
            }
            int pi = (i - 1 + m) % m, ni = (i + 1) % m;
            double ax = xy[2*pi], ay = xy[2*pi+1];
            double bx = xy[2*ni], by = xy[2*ni+1];
            double px = xy[2*i], py = xy[2*i+1];
            double dx = bx - ax, dy = by - ay;
            double len2 = dx*dx + dy*dy;
            double d;
            if (len2 < 1e-18) {
                d = std::hypot(px - ax, py - ay);
            } else {
                double t = ((px - ax)*dx + (py - ay)*dy) / len2;
                if (t < 0) t = 0; if (t > 1) t = 1;
                double qx = ax + t*dx, qy = ay + t*dy;
                d = std::hypot(px - qx, py - qy);
            }
            if (d < tol) continue;   // 丢弃近共线点
            out.push_back(px); out.push_back(py);
        }
        if (closed && (int)(out.size() / 2) < 3) break;   // 别把环抽没了
        if (out.size() == xy.size()) break;
        xy.swap(out);
    }
}

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

    long long step = std::max<long long>(1, F / 2000);
    std::vector<double> sumS(C + 1, 0.0);
    long long n = 0;
    OGR_L_ResetReading(lyr);
    OGRFeatureH f;
    long long idx = 0;
    while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        if (idx % step == 0) {
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
        }
        OGR_F_Destroy(f);
        ++idx;
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
                  const std::function<void(long long, long long)>& onProgress) {
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
    h.buildTime = (int64_t)std::time(nullptr);

    std::string dir = parentDir(cachePath);
    if (!dir.empty()) std::filesystem::create_directories(dir);

    VtCache cache;
    if (!cache.create(cachePath, h)) return false;

    // ---- 阶段 A: 最深层 ----
    Lru lru;
    lru.cap = (long long)cfg.lruVerts;
    const int n1 = 1 << Lmax;
    const double tileW = S / (double)n1;
    const double cell = tileW / (double)TILE_SIZE;

    auto flushTile = [&](uint64_t k) {
        auto f = lru.tiles.find(k);
        if (f == lru.tiles.end()) return;
        if (!f->second.empty()) {
            cache.writeTile(keyLevel(k), keyTx(k), keyTy(k), f->second);
            ++stats.tilesWritten;
            stats.storedVerts += (long long)f->second.vertexCount();
            if (onTile) onTile(keyLevel(k), keyTx(k), keyTy(k));
        }
        lru.verts -= (long long)f->second.vertexCount();
        lru.remove(k);
    };
    auto evict = [&](uint64_t keep) {
        while (lru.verts > lru.cap && lru.order.size() > 1) {
            uint64_t bk = lru.order.back();
            if (bk == keep) break;
            flushTile(bk);
        }
    };

    long long nf = streamVtRings(srcPath, layerIdx, dstEpsg, [&](const SourceRing& sr) {
        int rn = (int)(sr.xy.size() / 2);
        if (rn < 1) { return; }
        int rnOrig = rn;
        std::vector<double> simplified;
        const std::vector<double>* rp = &sr.xy;
        if (cfg.simplify && sr.type != RING_POINT) {
            simplified = sr.xy;
            simplifyPolyline(simplified, cell * cfg.simplifyFactor, sr.type == RING_FACE);
            rp = &simplified;
        }
        const std::vector<double>& rxy = *rp;
        rn = (int)(rxy.size() / 2);
        if (rn < 1) { return; }
        double rminx = 1e300, rminy = 1e300, rmaxx = -1e300, rmaxy = -1e300;
        for (int i = 0; i < rn; ++i) {
            double x = rxy[2*i], y = rxy[2*i+1];
            rminx = std::min(rminx, x); rmaxx = std::max(rmaxx, x);
            rminy = std::min(rminy, y); rmaxy = std::max(rmaxy, y);
        }
        int tx0 = (int)std::floor((rminx - originX) / tileW);
        int tx1 = (int)std::floor((rmaxx - originX) / tileW);
        int ty0 = (int)std::floor((rminy - originY) / tileW);
        int ty1 = (int)std::floor((rmaxy - originY) / tileW);
        tx0 = std::max(0, std::min(n1 - 1, tx0));
        tx1 = std::max(0, std::min(n1 - 1, tx1));
        ty0 = std::max(0, std::min(n1 - 1, ty0));
        ty1 = std::max(0, std::min(n1 - 1, ty1));

        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                uint64_t k = tileKey(Lmax, tx, ty);
                VtTile& t = lru.get(k);
                double ox = originX + tx * tileW;
                double oy = originY + ty * tileW;
                double wx0 = ox - TILE_PAD * cell;
                double wy0 = oy - TILE_PAD * cell;
                double wx1 = ox + TILE_SIZE * cell + TILE_PAD * cell;
                double wy1 = oy + TILE_SIZE * cell + TILE_PAD * cell;
                long long before = (long long)t.vertexCount();
                if (sr.type == RING_FACE) {
                    std::vector<double> clipped;
                    clipPolyRect(rxy, wx0, wy0, wx1, wy1, clipped);
                    appendRing(t, RING_FACE, sr.hole, sr.polyGroup, clipped, ox, oy, cell);
                } else if (sr.type == RING_LINE) {
                    for (int i = 0; i + 1 < rn; ++i) {
                        double a, b, c, d;
                        if (clipSegment(rxy[2*i], rxy[2*i+1], rxy[2*i+2], rxy[2*i+3],
                                        wx0, wy0, wx1, wy1, a, b, c, d)) {
                            std::vector<double> seg = {a, b, c, d};
                            appendRing(t, RING_LINE, 0, 0, seg, ox, oy, cell);
                        }
                    }
                } else {
                    double x = sr.xy[0], y = sr.xy[1];
                    if (x >= wx0 && x <= wx1 && y >= wy0 && y <= wy1)
                        appendRing(t, RING_POINT, 0, 0, sr.xy, ox, oy, cell);
                }
                long long added = (long long)t.vertexCount() - before;
                lru.verts += added;
                evict(k);
            }
        }
        ++stats.rings;
        stats.srcVerts += rnOrig;
        if (onProgress && (stats.rings % 10000) == 0) onProgress(stats.rings, li.featureCount);
    });
    stats.features = nf > 0 ? nf : 0;

    while (!lru.order.empty()) flushTile(lru.order.back());
    cache.setFullyBuilt(Lmax);

    // ---- 阶段 B: 4x4 合并(父=子/2) ----
    for (int L = Lmax - 1; L >= 0; --L) {
        int n = 1 << L;
        double pW = S / (double)n;
        double pCell = pW / (double)TILE_SIZE;
        int cn = 1 << (L + 1);
        double cW = pW / 2.0;
        double cCell = pCell / 2.0;
        for (int py = 0; py < n; ++py) {
            for (int px = 0; px < n; ++px) {
                VtTile parent;
                parent.originX = originX + px * pW;
                parent.originY = originY + py * pW;
                parent.epsg = dstEpsg;
                double wx0 = parent.originX - TILE_PAD * pCell;
                double wy0 = parent.originY - TILE_PAD * pCell;
                double wx1 = parent.originX + TILE_SIZE * pCell + TILE_PAD * pCell;
                double wy1 = parent.originY + TILE_SIZE * pCell + TILE_PAD * pCell;
                int cx0 = std::max(0, std::min(cn - 1, (int)std::floor((wx0 - originX) / cW)));
                int cx1 = std::max(0, std::min(cn - 1, (int)std::floor((wx1 - originX) / cW)));
                int cy0 = std::max(0, std::min(cn - 1, (int)std::floor((wy0 - originY) / cW)));
                int cy1 = std::max(0, std::min(cn - 1, (int)std::floor((wy1 - originY) / cW)));
                for (int cy = cy0; cy <= cy1; ++cy) {
                    for (int cx = cx0; cx <= cx1; ++cx) {
                        VtTile child;
                        if (!cache.readTile(L + 1, cx, cy, child)) continue;
                        double cox = originX + cx * cW;
                        double coy = originY + cy * cW;
                        for (const VtRing& r : child.rings) {
                            std::vector<double> disp;
                            disp.reserve((size_t)r.vertexCount * 2);
                            for (uint32_t i = 0; i < r.vertexCount; ++i) {
                                int16_t gx = child.verts[(size_t)(r.firstVertex + i) * 2];
                                int16_t gy = child.verts[(size_t)(r.firstVertex + i) * 2 + 1];
                                disp.push_back(cox + gx * cCell);
                                disp.push_back(coy + gy * cCell);
                            }
                            if (cfg.simplify && r.type != RING_POINT)
                                simplifyPolyline(disp, pCell * cfg.simplifyFactor, r.type == RING_FACE);
                            if (r.type == RING_FACE) {
                                std::vector<double> clipped;
                                clipPolyRect(disp, wx0, wy0, wx1, wy1, clipped);
                                appendRing(parent, RING_FACE, r.hole, r.polyGroup, clipped,
                                           parent.originX, parent.originY, pCell);
                            } else if (r.type == RING_LINE) {
                                int m = (int)(disp.size() / 2);
                                for (int i = 0; i + 1 < m; ++i) {
                                    double a, b, c, d;
                                    if (clipSegment(disp[2*i], disp[2*i+1], disp[2*i+2], disp[2*i+3],
                                                    wx0, wy0, wx1, wy1, a, b, c, d)) {
                                        std::vector<double> seg = {a, b, c, d};
                                        appendRing(parent, RING_LINE, 0, 0, seg,
                                                   parent.originX, parent.originY, pCell);
                                    }
                                }
                            } else {
                                double x = disp[0], y = disp[1];
                                if (x >= wx0 && x <= wx1 && y >= wy0 && y <= wy1)
                                    appendRing(parent, RING_POINT, 0, 0, disp,
                                               parent.originX, parent.originY, pCell);
                            }
                        }
                    }
                }
                if (!parent.empty()) {
                    cache.writeTile(L, px, py, parent);
                    ++stats.tilesWritten;
                    stats.storedVerts += (long long)parent.vertexCount();
                    if (onTile) onTile(L, px, py);
                }
            }
        }
        cache.setFullyBuilt(L);
    }

    cache.finalize();
    stats.dataBytes = cache.dataBytes();
    stats.maxLevel = Lmax;
    auto t1 = std::chrono::steady_clock::now();
    stats.seconds = std::chrono::duration<double>(t1 - t0).count();
    return true;
}

}  // namespace peekg::vt
