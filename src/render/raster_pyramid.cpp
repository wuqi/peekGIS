#include "render/raster_pyramid.h"
#include "data/gdal_common.h"

#include <ogr_api.h>
#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <unordered_map>
#include <vector>

namespace peekg::render {

namespace {

using peekg::data::ensureGdal;
using peekg::data::gdalOpenVector;

// ---- 磁盘: R8 覆盖度 zstd 块 (与既有烘焙缓存格式一致: [n:u64][zstd]) ----
bool saveBin(const std::string& path, const uint8_t* px, size_t n) {
    std::vector<char> comp(ZSTD_compressBound(n));
    size_t cz = ZSTD_compress(comp.data(), comp.size(), px, n, 1);   // level 1: 轻量, 覆盖度稀疏本来就好压
    if (ZSTD_isError(cz)) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream of(path, std::ios::binary);
    if (!of) return false;
    uint64_t nn = (uint64_t)n;
    of.write((const char*)&nn, 8);
    of.write(comp.data(), (std::streamsize)cz);
    return (bool)of;
}
bool loadBin(const std::string& path, uint8_t* px, size_t n) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    uint64_t nn = 0;
    in.read((char*)&nn, 8);
    if (!in || nn != n) return false;
    std::vector<char> comp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t ds = ZSTD_decompress(px, n, comp.data(), comp.size());
    return !ZSTD_isError(ds) && ds == n;
}

// 扫描线填充(偶奇)一个环(世界坐标)到片缓冲; 片覆盖 [ox,ox+res*cell] x [oy,oy+res*cell]
void fillRing(uint8_t* buf, int res, double ox, double oy, double cell,
              const double* xy, int n) {
    if (n < 3) return;
    double minY = 1e300, maxY = -1e300;
    for (int i = 0; i < n; ++i) {
        double y = (xy[2 * i + 1] - oy) / cell;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    int y0 = std::max(0, (int)std::floor(minY));
    int y1 = std::min(res - 1, (int)std::ceil(maxY));
    std::vector<double> xs;
    for (int py = y0; py <= y1; ++py) {
        double yc = py + 0.5;
        xs.clear();
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            double x1 = (xy[2 * i] - ox) / cell, y1v = (xy[2 * i + 1] - oy) / cell;
            double x2 = (xy[2 * j] - ox) / cell, y2v = (xy[2 * j + 1] - oy) / cell;
            if ((y1v <= yc && y2v > yc) || (y2v <= yc && y1v > yc)) {
                double t = (yc - y1v) / (y2v - y1v);
                xs.push_back(x1 + t * (x2 - x1));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            int px0 = std::max(0, (int)std::ceil(xs[k] - 0.5));
            int px1 = std::min(res - 1, (int)std::floor(xs[k + 1] - 0.5));
            for (int px = px0; px <= px1; ++px) buf[(size_t)py * res + px] = 255;
        }
    }
}

// DDA 画线(世界坐标折线)到片缓冲
void drawPolyline(uint8_t* buf, int res, double ox, double oy, double cell,
                  const double* xy, int n) {
    for (int i = 0; i + 1 < n; ++i) {
        double x1 = (xy[2 * i] - ox) / cell, y1 = (xy[2 * i + 1] - oy) / cell;
        double x2 = (xy[2 * i + 2] - ox) / cell, y2 = (xy[2 * i + 3] - oy) / cell;
        double dx = x2 - x1, dy = y2 - y1;
        int steps = (int)std::ceil(std::max(std::fabs(dx), std::fabs(dy)));
        if (steps <= 0) { if (x1 >= 0 && x1 < res && y1 >= 0 && y1 < res) buf[(size_t)y1 * res + (int)x1] = 255; continue; }
        if (steps > 100000) steps = 100000;
        for (int s = 0; s <= steps; ++s) {
            double t = (double)s / steps;
            int px = (int)std::floor(x1 + t * dx), py = (int)std::floor(y1 + t * dy);
            if (px >= 0 && px < res && py >= 0 && py < res) buf[(size_t)py * res + px] = 255;
        }
    }
}

// ---- 每片一个 R8 缓冲; 有界 LRU(超限存盘, 再触达时读回继续 OR) ----
struct TileBuf {
    std::vector<uint8_t> px;
    std::list<uint64_t>::iterator it;
};

uint64_t tkey(int lv, int tx, int ty) {
    return ((uint64_t)(lv & 0xff) << 48) | ((uint64_t)(tx & 0xffffff) << 24) | (uint64_t)(ty & 0xffffff);
}

}  // namespace

bool buildRasterPyramid(const std::string& srcPath, int layerIdx, int dstEpsg,
                        int maxLevel, int tileRes, const std::string& cacheDir,
                        const std::function<void(int)>& onProgress) {
    ensureGdal();
    GDALDatasetH ds = gdalOpenVector(srcPath);
    if (!ds) return false;
    int nl = GDALDatasetGetLayerCount(ds);
    if (layerIdx < 0 || layerIdx >= nl) { GDALClose(ds); return false; }
    OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);

    OGRSpatialReferenceH srcSrs = OGR_L_GetSpatialRef(lyr);
    int srcEpsg = peekg::data::gdalSrsEpsg(srcSrs);
    int dst = dstEpsg > 0 ? dstEpsg : srcEpsg;
    OGRCoordinateTransformationH ct = nullptr;
    if (dst > 0 && srcEpsg > 0 && dst != srcEpsg && srcSrs) {
        OGRSpatialReferenceH d = OSRNewSpatialReference(nullptr);
        if (OSRImportFromEPSG(d, dst) == OGRERR_NONE)
            ct = OCTNewCoordinateTransformation(srcSrs, d);
        OSRDestroySpatialReference(d);
    }

    OGREnvelope env;
    if (OGR_L_GetExtent(lyr, &env, TRUE) != OGRERR_NONE) { if (ct) OCTDestroyCoordinateTransformation(ct); GDALClose(ds); return false; }
    double minx = env.MinX, miny = env.MinY, maxx = env.MaxX, maxy = env.MaxY;
    if (ct) {
        const double xs[4] = {env.MinX, env.MaxX, env.MinX, env.MaxX};
        const double ys[4] = {env.MinY, env.MinY, env.MaxY, env.MaxY};
        minx = miny = 1e300; maxx = maxy = -1e300;
        for (int i = 0; i < 4; ++i) {
            double x = xs[i], y = ys[i];
            if (OCTTransform(ct, 1, &x, &y, nullptr)) {
                minx = std::min(minx, x); maxx = std::max(maxx, x);
                miny = std::min(miny, y); maxy = std::max(maxy, y);
            }
        }
        if (minx > maxx) { minx = env.MinX; miny = env.MinY; maxx = env.MaxX; maxy = env.MaxY; }
    }
    double spanX = maxx - minx, spanY = maxy - miny;
    double S = std::max(spanX, spanY);
    if (S <= 0) S = 1.0;
    double originX = minx - (S - spanX) / 2, originY = miny - (S - spanY) / 2;
    long long F = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
    if (F <= 0) F = 1;

    // 缓存目录(与 app 的 bakeCachePath 命名一致)
    char hb[24];
    std::snprintf(hb, sizeof(hb), "%zx", std::hash<std::string>{}(srcPath));
    std::string tag = "dat";
    size_t dot = srcPath.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < srcPath.size()) {
        tag = srcPath.substr(dot + 1);
        for (auto& ch : tag) ch = (char)std::tolower((unsigned char)ch);
    }
    std::string dir = cacheDir + "/bake/" + std::string(hb).substr(0, 8) + "_" + tag;
    if (dst != 0) dir += "_epsg" + std::to_string(dst);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto tilePath = [&](int lv, int tx, int ty) {
        char b[64];
        std::snprintf(b, sizeof(b), "/L%d_%d_%d.bin", lv, tx, ty);
        return dir + b;
    };

    // ---- 1) 最深层: 扫源一遍, 逐要素光栅化进相交片(LRU 有界) ----
    int n = 1 << maxLevel;
    double tileW = S / n;
    double cell = tileW / tileRes;
    const size_t pxBytes = (size_t)tileRes * tileRes;
    std::unordered_map<uint64_t, TileBuf> lru;
    std::list<uint64_t> order;
    const size_t lruCap = 4096;   // ~256MB(256px 片): 加大减少反复 load/save
    auto flush = [&](uint64_t k) {
        auto f = lru.find(k);
        if (f == lru.end()) return;
        int lv = (int)((k >> 48) & 0xff), tx = (int)((k >> 24) & 0xffffff), ty = (int)(k & 0xffffff);
        saveBin(tilePath(lv, tx, ty), f->second.px.data(), pxBytes);
        lru.erase(f);
    };
    auto get = [&](uint64_t k, int lv, int tx, int ty) -> uint8_t* {
        auto f = lru.find(k);
        if (f != lru.end()) { order.splice(order.begin(), order, f->second.it); return f->second.px.data(); }
        while (lru.size() >= lruCap) { flush(order.back()); order.pop_back(); }
        TileBuf tb;
        tb.px.assign(pxBytes, 0);
        loadBin(tilePath(lv, tx, ty), tb.px.data(), pxBytes);   // 已有则读回(继续 OR)
        order.push_front(k);
        tb.it = order.begin();
        auto ins = lru.emplace(k, std::move(tb));
        return ins.first->second.px.data();
    };

    OGR_L_ResetReading(lyr);
    OGRFeatureH feat;
    long long fi = 0;
    std::vector<double> xy;
    std::function<void(OGRGeometryH)> emit;
    emit = [&](OGRGeometryH g) {
        if (!g) return;
        OGRwkbGeometryType t = wkbFlatten(OGR_G_GetGeometryType(g));
        if (t == wkbMultiPolygon || t == wkbMultiLineString || t == wkbMultiPoint ||
            t == wkbGeometryCollection) {
            int ng = OGR_G_GetGeometryCount(g);
            for (int i = 0; i < ng; ++i) emit(OGR_G_GetGeometryRef(g, i));
            return;
        }
        int ng = OGR_G_GetGeometryCount(g);
        bool isLine = (t == wkbLineString || t == wkbLinearRing);
        bool isPoint = (t == wkbPoint);
        int rings = (t == wkbPolygon && ng > 0) ? ng : 1;
        for (int r = 0; r < rings; ++r) {
            OGRGeometryH ring = (t == wkbPolygon && ng > 0) ? OGR_G_GetGeometryRef(g, r) : g;
            int np = OGR_G_GetPointCount(ring);
            if (np < (isPoint ? 1 : 2)) continue;
            xy.clear();
            xy.reserve((size_t)np * 2);
            for (int i = 0; i < np; ++i) {
                double x = OGR_G_GetX(ring, i), y = OGR_G_GetY(ring, i);
                if (ct) OCTTransform(ct, 1, &x, &y, nullptr);
                xy.push_back(x); xy.push_back(y);
            }
            // 片范围
            double rminx = 1e300, rminy = 1e300, rmaxx = -1e300, rmaxy = -1e300;
            for (size_t i = 0; i < xy.size(); i += 2) {
                rminx = std::min(rminx, xy[i]); rmaxx = std::max(rmaxx, xy[i]);
                rminy = std::min(rminy, xy[i + 1]); rmaxy = std::max(rmaxy, xy[i + 1]);
            }
            int tx0 = std::max(0, std::min(n - 1, (int)std::floor((rminx - originX) / tileW)));
            int tx1 = std::max(0, std::min(n - 1, (int)std::floor((rmaxx - originX) / tileW)));
            int ty0 = std::max(0, std::min(n - 1, (int)std::floor((rminy - originY) / tileW)));
            int ty1 = std::max(0, std::min(n - 1, (int)std::floor((rmaxy - originY) / tileW)));
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx) {
                    uint8_t* buf = get(tkey(maxLevel, tx, ty), maxLevel, tx, ty);
                    double ox = originX + tx * tileW, oy = originY + ty * tileW;
                    if (isPoint) {
                        int px = (int)std::floor((xy[0] - ox) / cell), py = (int)std::floor((xy[1] - oy) / cell);
                        if (px >= 0 && px < tileRes && py >= 0 && py < tileRes) buf[(size_t)py * tileRes + px] = 255;
                    } else if (isLine) {
                        drawPolyline(buf, tileRes, ox, oy, cell, xy.data(), (int)(xy.size() / 2));
                    } else {
                        fillRing(buf, tileRes, ox, oy, cell, xy.data(), (int)(xy.size() / 2));
                    }
                }
        }
    };

    while ((feat = OGR_L_GetNextFeature(lyr)) != nullptr) {
        OGRGeometryH g = OGR_F_GetGeometryRef(feat);
        if (g) emit(g);
        OGR_F_Destroy(feat);
        ++fi;
        if (onProgress && (fi % 20000) == 0) onProgress((int)(fi * 70 / F));
    }
    while (!order.empty()) { flush(order.back()); order.pop_back(); }
    if (ct) OCTDestroyCoordinateTransformation(ct);
    GDALClose(ds);
    if (onProgress) onProgress(70);

    // ---- 2) 逐层 2x2 降采样 ----
    std::vector<uint8_t> child(pxBytes), parent(pxBytes);
    for (int lv = maxLevel - 1; lv >= 0; --lv) {
        int m = 1 << lv;
        for (int ty = 0; ty < m; ++ty)
            for (int tx = 0; tx < m; ++tx) {
                std::fill(parent.begin(), parent.end(), 0);
                for (int cy = 0; cy < 2; ++cy)
                    for (int cx = 0; cx < 2; ++cx) {
                        if (!loadBin(tilePath(lv + 1, tx * 2 + cx, ty * 2 + cy), child.data(), pxBytes)) continue;
                        int off = (cy * tileRes / 2) * tileRes + cx * tileRes / 2;
                        for (int y = 0; y < tileRes / 2; ++y)
                            for (int x = 0; x < tileRes / 2; ++x) {
                                uint8_t v = child[(size_t)(y * 2) * tileRes + x * 2];
                                uint8_t& p = parent[(size_t)(off + y * tileRes + x)];
                                if (v > p) p = v;
                            }
                    }
                saveBin(tilePath(lv, tx, ty), parent.data(), pxBytes);
            }
        if (onProgress) onProgress(70 + (maxLevel - lv) * 30 / maxLevel);
    }
    if (onProgress) onProgress(100);
    return true;
}

}  // namespace peekg::render
