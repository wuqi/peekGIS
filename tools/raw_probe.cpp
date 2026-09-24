// 端到端直读探针: 像渲染器那样, 用缓存 header 网格 + 视口中心在 Lraw 层对源 shp 做 RawRegionStream。
// 用法: raw_probe <cache.vtk> <src.shp> [level] [cx] [cy]
//   缺省 level=9; cx/cy 为视口中心(缓存 CRS 坐标)
#include "vt/vt_cache.h"
#include "vt/vt_build.h"
#include "vt/vt_level.h"
#include "vt/vt_types.h"
#include "data/gdal_common.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 3) {
        std::printf("用法: raw_probe <cache.vtk> <src.shp> [level] [cx] [cy]\n");
        return 1;
    }
    peekg::data::ensureGdal();
    std::string cache = argv[1], src = argv[2];
    int level = (argc > 3) ? std::atoi(argv[3]) : 9;

    peekg::vt::VtCache c;
    if (!c.open(cache)) { std::printf("缓存打开失败\n"); return 1; }
    const peekg::vt::VtFileHeader& h = c.header();
    double ox = h.originX, oy = h.originY, S = h.tileW0;
    int maxLevel = (int)h.maxLevel;
    double cx = ox + S * 0.5, cy = oy + S * 0.5;
    if (argc > 4) cx = std::atof(argv[4]);
    if (argc > 5) cy = std::atof(argv[5]);
    std::printf("cache=%s maxLevel=%d origin=(%.6f,%.6f) S=%.6f level=%d 中心=(%.4f,%.4f)\n",
                cache.c_str(), maxLevel, ox, oy, S, level, cx, cy);

    // 视口: 大约一屏 2048px, 缩放到该层邻域 —— scale = tileW/(512*2^L)? 渲染里 Lw 由 scale 决定。
    // 这里直接取该层的世界尺度: 整层 S, 一屏约看到 1/2^L 宽度的若干瓦片。
    double n = (double)(1LL << level);
    double tileW = S / n;
    // 视口范围: 中心 ±(2*tileW) => 约 4x4 瓦片(用户场景: 深层看到的要素就十几个)
    double rx0 = cx - 2.0 * tileW, rx1 = cx + 2.0 * tileW;
    double ry0 = cy - 2.0 * tileW, ry1 = cy + 2.0 * tileW;

    auto t0 = std::chrono::steady_clock::now();
    peekg::vt::RawRegionStream rs;
    bool ok = rs.open(src, 0, h.dstEpsg, level, maxLevel, ox, oy, S, rx0, ry0, rx1, ry1);
    auto t1 = std::chrono::steady_clock::now();
    if (!ok) { std::printf("RawRegionStream::open 失败\n"); return 2; }
    std::printf("open 成功 (%.1fms), 命中要素(featureCount) = %lld\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count(), rs.featureCount());

    for (int pass = 0; pass < 3 && !rs.isOpen(); ++pass)
        ;
    // 分块读到 EOF
    long long scanned = 0; double ms = 0; bool done = false;
    long long cumScanned = 0; double cumMs = 0;
    int chunks = 0;
    do {
        scanned = 0; ms = 0; done = false;
        auto tc = std::chrono::steady_clock::now();
        rs.chunk(20000, 20.0, scanned, ms, done);
        auto tc2 = std::chrono::steady_clock::now();
        cumScanned += scanned;
        cumMs += std::chrono::duration<double, std::milli>(tc2 - tc).count();
        ++chunks;
        std::printf(" 块%d: scanned=%lld 耗时=%.1fms done=%s\n", chunks, scanned, ms, done ? "Y" : "N");
    } while (!done && chunks < 50);
    std::printf("累计: %lld 要素 / %d 块 / %.1fms\n", cumScanned, chunks, cumMs);

    std::vector<std::pair<uint64_t, peekg::vt::VtTile>> tiles;
    rs.takeTiles(tiles);
    std::printf("取走瓦片 %zu 片:", tiles.size());
    int rings = 0; uint32_t verts = 0;
    for (auto& kv : tiles) {
        int tx = (int)((kv.first >> 24) & 0xffffff);
        int ty = (int)(kv.first & 0xffffff);
        std::printf(" (%d,%d)", tx, ty);
        for (auto& r : kv.second.rings) ++rings;
        verts += kv.second.vertexCount();
    }
    std::printf("\n环总数=%d 顶点=%u\n", rings, verts);
    rs.close();
    c.close();
    return 0;
}