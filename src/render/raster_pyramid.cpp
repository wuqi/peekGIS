#include "render/raster_pyramid.h"
#include "data/gdal_common.h"

#include <ogr_api.h>
#include <zstd.h>
#include <plutovg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
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

// ---- 一层一个文件(.pak): 头部(64B) + 固定槽表(每槽 16B: off:u64,size:u32,valid:u8,pad:3) + 追加的 zstd 块 ----
// 目的: 消除"几万个小文件"的簇浪费(每文件 4KB 簇), 随机读 O(1), 追加写。
static const size_t kPakHeader = 64;
struct LevelPack {
    std::FILE* fp = nullptr;
    std::mutex mtx;
    std::vector<uint64_t> off;
    std::vector<uint32_t> sz;
    std::vector<uint8_t> valid;
    uint64_t appendOff = 64;
    uint32_t side = 0;
};

static bool packOpen(LevelPack& p, const std::string& path, int lv, int tileRes) {
    p.side = 1u << lv;
    uint32_t slotCount = p.side * p.side;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    bool exists = std::filesystem::exists(path, ec);
    p.fp = std::fopen(path.c_str(), exists ? "rb+" : "wb+");
    if (!p.fp) return false;
    p.off.assign(slotCount, 0);
    p.sz.assign(slotCount, 0);
    p.valid.assign(slotCount, 0);
    char hdr[kPakHeader];
    if (exists) {
        if (std::fread(hdr, 1, kPakHeader, p.fp) != kPakHeader || std::memcmp(hdr, "PEEKPAK1", 8) != 0) {
            std::fclose(p.fp); p.fp = nullptr; return false;
        }
        uint32_t ver = 0, hl = 0, hres = 0, hside = 0, hsc = 0;
        std::memcpy(&ver, hdr + 8, 4); std::memcpy(&hl, hdr + 12, 4);
        std::memcpy(&hres, hdr + 16, 4); std::memcpy(&hside, hdr + 20, 4);
        std::memcpy(&hsc, hdr + 24, 4); std::memcpy(&p.appendOff, hdr + 32, 8);
        if (ver != 1 || hl != (uint32_t)lv || hres != (uint32_t)tileRes || hside != p.side || hsc != slotCount) {
            std::fclose(p.fp); p.fp = nullptr; return false;
        }
        std::vector<uint8_t> st((size_t)slotCount * 16);
        std::fseek(p.fp, (long)kPakHeader, SEEK_SET);
        if (std::fread(st.data(), 1, st.size(), p.fp) != st.size()) {
            std::fclose(p.fp); p.fp = nullptr; return false;
        }
        for (uint32_t i = 0; i < slotCount; ++i) {
            std::memcpy(&p.off[i], st.data() + (size_t)i * 16, 8);
            std::memcpy(&p.sz[i], st.data() + (size_t)i * 16 + 8, 4);
            p.valid[i] = st[(size_t)i * 16 + 12];
        }
        if (p.appendOff < kPakHeader + (uint64_t)slotCount * 16)
            p.appendOff = kPakHeader + (uint64_t)slotCount * 16;
    } else {
        std::memset(hdr, 0, sizeof(hdr));
        std::memcpy(hdr, "PEEKPAK1", 8);
        uint32_t ver = 1, res = (uint32_t)tileRes, lvv = (uint32_t)lv;
        std::memcpy(hdr + 8, &ver, 4); std::memcpy(hdr + 12, &lvv, 4);
        std::memcpy(hdr + 16, &res, 4); std::memcpy(hdr + 20, &p.side, 4);
        std::memcpy(hdr + 24, &slotCount, 4);
        p.appendOff = kPakHeader + (uint64_t)slotCount * 16;
        std::memcpy(hdr + 32, &p.appendOff, 8);
        std::vector<uint8_t> st((size_t)slotCount * 16, 0);
        if (std::fwrite(hdr, 1, kPakHeader, p.fp) != kPakHeader || std::fwrite(st.data(), 1, st.size(), p.fp) != st.size()) {
            std::fclose(p.fp); p.fp = nullptr; return false;
        }
    }
    return true;
}

static bool packLoad(LevelPack& p, int tx, int ty, uint8_t* out, size_t n) {
    if (!p.fp) return false;
    uint64_t idx = (uint64_t)ty * p.side + tx;
    if (idx >= p.off.size()) return false;
    std::lock_guard<std::mutex> lk(p.mtx);
    if (!p.valid[idx]) return false;
    std::vector<char> comp(p.sz[idx]);
    std::fseek(p.fp, (long)p.off[idx], SEEK_SET);
    if (std::fread(comp.data(), 1, comp.size(), p.fp) != comp.size()) return false;
    uint64_t nn = 0;
    std::memcpy(&nn, comp.data(), 8);
    if (nn != n) return false;
    size_t ds = ZSTD_decompress(out, n, comp.data() + 8, comp.size() - 8);
    return !ZSTD_isError(ds) && ds == n;
}

static bool packSave(LevelPack& p, int tx, int ty, const uint8_t* px, size_t n) {
    if (!p.fp) return false;
    uint64_t idx = (uint64_t)ty * p.side + tx;
    if (idx >= p.off.size()) return false;
    std::vector<char> comp(8 + ZSTD_compressBound(n));
    uint64_t nn = (uint64_t)n;
    std::memcpy(comp.data(), &nn, 8);
    size_t cz = ZSTD_compress(comp.data() + 8, comp.size() - 8, px, n, 1);
    if (ZSTD_isError(cz)) return false;
    size_t total = 8 + cz;
    std::lock_guard<std::mutex> lk(p.mtx);
    uint64_t off = p.appendOff;
    std::fseek(p.fp, (long)off, SEEK_SET);
    if (std::fwrite(comp.data(), 1, total, p.fp) != total) return false;
    p.appendOff = off + total;
    p.off[idx] = off; p.sz[idx] = (uint32_t)total; p.valid[idx] = 1;
    uint8_t slot[16];
    std::memcpy(slot, &off, 8);
    uint32_t s32 = (uint32_t)total;
    std::memcpy(slot + 8, &s32, 4);
    slot[12] = 1; std::memset(slot + 13, 0, 3);
    std::fseek(p.fp, (long)(kPakHeader + idx * 16), SEEK_SET);
    if (std::fwrite(slot, 1, 16, p.fp) != 16) return false;
    uint64_t ao = p.appendOff;
    std::fseek(p.fp, 32, SEEK_SET);
    std::fwrite(&ao, 1, 8, p.fp);
    return true;
}

// 扫描线填充(偶奇)一个环(世界坐标)到片缓冲; 片覆盖 [ox,ox+res*cell] x [oy,oy+res*cell]
void fillRing(uint8_t* buf, int res, double ox, double oy, double cell,
              const float* xy, int n) {
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

// 用 plutovg 奇偶填充环到 ARGB32 片缓冲
static void fillRingsPlutovg(uint8_t* argb, int res, double ox, double oy, double cell, float alpha,
                             const std::vector<std::vector<float>>& rings) {
    plutovg_surface_t* surf = plutovg_surface_create_for_data(argb, res, res, res * 4);
    if (!surf) return;
    plutovg_canvas_t* cv = plutovg_canvas_create(surf);
    if (!cv) { plutovg_surface_destroy(surf); return; }
    plutovg_canvas_set_fill_rule(cv, PLUTOVG_FILL_RULE_EVEN_ODD);
    plutovg_canvas_set_paint(cv, plutovg_paint_create_rgba(1, 1, 1, alpha));
    for (const auto& ring : rings) {
        int np = (int)(ring.size() / 2);
        if (np < 3) continue;
        plutovg_canvas_move_to(cv, (float)((ring[0] - ox) / cell), (float)((ring[1] - oy) / cell));
        for (int i = 1; i < np; ++i)
            plutovg_canvas_line_to(cv, (float)((ring[2 * i] - ox) / cell), (float)((ring[2 * i + 1] - oy) / cell));
        plutovg_canvas_close_path(cv);
    }
    plutovg_canvas_fill(cv);
    plutovg_canvas_destroy(cv);
    plutovg_surface_destroy(surf);
}

// DDA 画线(世界坐标折线)到片缓冲
void drawPolyline(uint8_t* buf, int res, double ox, double oy, double cell,
                  const float* xy, int n) {
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
                        int maxLevel, int tileRes, float fillAlpha, const std::string& cacheDir,
                        const std::function<void(int)>& onProgress,
                        const std::function<void(int, int, int)>& onTile) {
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
    // 一层一个文件: L<lv>.pak (替代每片一个 .bin, 消除小文件簇浪费)
    std::vector<LevelPack> packs((size_t)maxLevel + 1);
    for (int lv = 0; lv <= maxLevel; ++lv)
        packOpen(packs[lv], dir + "/L" + std::to_string(lv) + ".pak", lv, tileRes);

    // ---- 1) 最深层: 扫源一遍, 逐要素路由到 N 个 worker(各带自己的片 LRU), 并行光栅化 ----
    int n = 1 << maxLevel;
    double tileW = S / n;
    double cell = tileW / tileRes;
    const size_t pxBytes = (size_t)tileRes * tileRes;
    unsigned NW = std::thread::hardware_concurrency();
    if (NW == 0) NW = 4;
    if (NW > 16) NW = 16;

    struct QItem {
        uint64_t k = 0;
        uint8_t type = 0;   // 0=面 1=线 2=点
        std::shared_ptr<std::vector<std::vector<float>>> rings;
    };
    struct Worker {
        std::unordered_map<uint64_t, std::vector<uint8_t>> lru;
        std::list<uint64_t> order;
        std::mutex m;
        std::condition_variable cv;
        std::deque<QItem> q;
        bool done = false;
        long long items = 0;
    };
    std::vector<Worker> W(NW);
    const size_t lruCap = 1024;   // 每 worker ~64MB(256px 片)
    std::vector<std::thread> workers;
    workers.reserve(NW);
    for (unsigned w = 0; w < NW; ++w) {
        workers.emplace_back([&, w]() {
            Worker& wk = W[w];
            for (;;) {
                QItem item;
                {
                    std::unique_lock<std::mutex> lk(wk.m);
                    wk.cv.wait(lk, [&] { return wk.done || !wk.q.empty(); });
                    if (wk.q.empty()) { if (wk.done) break; continue; }
                    item = std::move(wk.q.front());
                    wk.q.pop_front();
                }
                uint64_t k = item.k;
                int lv = (int)((k >> 48) & 0xff), tx = (int)((k >> 24) & 0xffffff), ty = (int)(k & 0xffffff);
                auto f = wk.lru.find(k);
                if (f == wk.lru.end()) {
                    while (wk.lru.size() >= lruCap) {
                        uint64_t bk = wk.order.back(); wk.order.pop_back();
                        auto bf = wk.lru.find(bk);
                        if (bf == wk.lru.end()) continue;
                        int blv = (int)((bk >> 48) & 0xff), btx = (int)((bk >> 24) & 0xffffff), bty = (int)(bk & 0xffffff);
                        std::vector<uint8_t> r8(pxBytes);
                        for (size_t i = 0; i < pxBytes; ++i) r8[i] = bf->second[i * 4];
                        packSave(packs[blv], btx, bty, r8.data(), pxBytes);
                        wk.lru.erase(bf);
                        if (onTile && blv == maxLevel) onTile(blv, btx, bty);   // 报告已建好的最深层片
                    }
                    std::vector<uint8_t> buf(pxBytes * 4, 0);   // ARGB32(plutovg 面)
                    {
                        std::vector<uint8_t> r8(pxBytes, 0);
                        if (packLoad(packs[lv], tx, ty, r8.data(), pxBytes))
                            for (size_t i = 0; i < pxBytes; ++i) { buf[i * 4] = r8[i]; buf[i * 4 + 3] = r8[i]; }
                    }
                    wk.order.push_front(k);
                    f = wk.lru.emplace(k, std::move(buf)).first;
                    if (onTile && lv == maxLevel) onTile(lv, tx, ty);   // 新片即报(覆盖即时增长)
                } else {
                    wk.order.splice(wk.order.begin(), wk.order, std::find(wk.order.begin(), wk.order.end(), k));
                }
                uint8_t* buf = f->second.data();
                double ox = originX + tx * tileW, oy = originY + ty * tileW;
                if (item.type == 1) {
                    for (const auto& r : *item.rings)
                        drawPolyline(buf, tileRes, ox, oy, cell, r.data(), (int)(r.size() / 2));
                } else if (item.type == 2) {
                    for (const auto& r : *item.rings) {
                        if (r.size() < 2) continue;
                        int px = (int)std::floor((r[0] - ox) / cell), py = (int)std::floor((r[1] - oy) / cell);
                        if (px >= 0 && px < tileRes && py >= 0 && py < tileRes) {
                            buf[(size_t)py * tileRes * 4 + px * 4] = 255;
                            buf[(size_t)py * tileRes * 4 + px * 4 + 3] = 255;
                        }
                    }
                } else {
                    fillRingsPlutovg(buf, tileRes, ox, oy, cell, fillAlpha, *item.rings);
                }
                ++wk.items;
            }
            for (auto& kv : wk.lru) {
                int lv = (int)((kv.first >> 48) & 0xff), tx = (int)((kv.first >> 24) & 0xffffff), ty = (int)(kv.first & 0xffffff);
                std::vector<uint8_t> r8(pxBytes);
                for (size_t i = 0; i < pxBytes; ++i) r8[i] = kv.second[i * 4];
                packSave(packs[lv], tx, ty, r8.data(), pxBytes);
                if (onTile && lv == maxLevel) onTile(lv, tx, ty);
            }
        });
    }

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
        bool isPoint = (t == wkbPoint);
        uint8_t gtype = isPoint ? 2 : ((t == wkbLineString || t == wkbLinearRing) ? 1 : 0);
        int nrings = (t == wkbPolygon && ng > 0) ? ng : 1;
        // 同一要素的所有环(外环+孔)收进一个 shared 容器, 一起路由给同一片 -> worker 奇偶填充抠孔
        auto rp = std::make_shared<std::vector<std::vector<float>>>();
        double rminx = 1e300, rminy = 1e300, rmaxx = -1e300, rmaxy = -1e300;
        for (int r = 0; r < nrings; ++r) {
            OGRGeometryH ring = (t == wkbPolygon && ng > 0) ? OGR_G_GetGeometryRef(g, r) : g;
            int np = OGR_G_GetPointCount(ring);
            if (np < (isPoint ? 1 : 2)) continue;
            std::vector<float> rxy;
            rxy.reserve((size_t)np * 2);
            for (int i = 0; i < np; ++i) {
                double x = OGR_G_GetX(ring, i), y = OGR_G_GetY(ring, i);
                if (ct) OCTTransform(ct, 1, &x, &y, nullptr);
                rxy.push_back((float)x); rxy.push_back((float)y);
                rminx = std::min(rminx, x); rmaxx = std::max(rmaxx, x);
                rminy = std::min(rminy, y); rmaxy = std::max(rmaxy, y);
            }
            rp->push_back(std::move(rxy));
        }
        if (rp->empty()) return;
        int tx0 = std::max(0, std::min(n - 1, (int)std::floor((rminx - originX) / tileW)));
        int tx1 = std::max(0, std::min(n - 1, (int)std::floor((rmaxx - originX) / tileW)));
        int ty0 = std::max(0, std::min(n - 1, (int)std::floor((rminy - originY) / tileW)));
        int ty1 = std::max(0, std::min(n - 1, (int)std::floor((rmaxy - originY) / tileW)));
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                uint64_t k = tkey(maxLevel, tx, ty);
                unsigned w = (unsigned)(((tx * 73856093u) ^ (ty * 19349663u)) % NW);
                {
                    std::lock_guard<std::mutex> lk(W[w].m);
                    W[w].q.push_back({k, gtype, rp});   // 同一 shared_ptr 给多片(共享环, 抠孔正确)
                }
                W[w].cv.notify_one();
            }
    };

    while ((feat = OGR_L_GetNextFeature(lyr)) != nullptr) {
        OGRGeometryH g = OGR_F_GetGeometryRef(feat);
        if (g) emit(g);
        OGR_F_Destroy(feat);
        ++fi;
        if (onProgress && (fi % 20000) == 0) onProgress((int)(fi * 70 / F));
    }
    for (unsigned w = 0; w < NW; ++w) { { std::lock_guard<std::mutex> lk(W[w].m); W[w].done = true; } W[w].cv.notify_all(); }
    for (auto& t : workers) t.join();
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
                        if (!packLoad(packs[lv + 1], tx * 2 + cx, ty * 2 + cy, child.data(), pxBytes)) continue;
                        int off = (cy * tileRes / 2) * tileRes + cx * tileRes / 2;
                        for (int y = 0; y < tileRes / 2; ++y)
                            for (int x = 0; x < tileRes / 2; ++x) {
                                uint8_t v = child[(size_t)(y * 2) * tileRes + x * 2];
                                uint8_t& p = parent[(size_t)(off + y * tileRes + x)];
                                if (v > p) p = v;
                            }
                    }
                packSave(packs[lv], tx, ty, parent.data(), pxBytes);
            }
        if (onProgress) onProgress(70 + (maxLevel - lv) * 30 / maxLevel);
    }
    for (auto& p : packs) if (p.fp) std::fclose(p.fp);
    if (onProgress) onProgress(100);
    return true;
}

}  // namespace peekg::render
