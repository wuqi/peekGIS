// vt_probe -- 读某层某瓦片, 把面填充光栅化到 512×512, 报覆盖率(诊断切片是否有洞)。
// 用法: vt_probe <cache.vtk> [level] [tx] [ty]
//   省略 level -> 用 maxLevel; 省略 tx/ty -> 取该层中心瓦片。
#include "vt/vt_cache.h"
#include "vt/vt_geom.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace peekg::vt;

static bool inTri(double px, double py, double ax, double ay, double bx, double by, double cx, double cy) {
    double d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    double d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    double d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) { printf("用法: vt_probe <cache.vtk> [level] [tx] [ty]\n"); return 1; }
    std::string path = argv[1];
    VtCache c;
    if (!c.open(path)) { printf("打开失败: %s\n", path.c_str()); return 1; }
    const VtFileHeader& h = c.header();
    int level = (argc > 2) ? std::atoi(argv[2]) : (int)h.maxLevel;
    int n = 1 << level;
    int tx = (argc > 3) ? std::atoi(argv[3]) : n / 2;
    int ty = (argc > 4) ? std::atoi(argv[4]) : n / 2;
    printf("cache=%s maxLevel=%u tileW0=%.6f\n", path.c_str(), h.maxLevel, h.tileW0);
    printf("probe L=%d tile=(%d,%d) of %dx%d\n", level, tx, ty, n, n);

    VtTile t;
    if (!c.readTile(level, tx, ty, t)) { printf("该瓦片不存在(空)\n"); return 2; }
    double tileW = h.tileW0 / (double)n;
    double cell = tileW / (double)TILE_SIZE;

    int nface = 0, nline = 0, npt = 0;
    for (const VtRing& r : t.rings) {
        if (r.type == RING_FACE) nface++;
        else if (r.type == RING_LINE) nline++;
        else npt++;
    }
    printf("rings: face=%d line=%d point=%d  verts=%u\n", nface, nline, npt, t.vertexCount());

    std::vector<float> lines, points, fill;
    buildTileGeometry(t, cell, true, lines, points, fill);
    int tris = (int)(fill.size() / 6);
    printf("fill triangles=%d  line verts=%d  point verts=%d\n", tris, (int)(lines.size() / 2), (int)(points.size() / 2));

    const int N = TILE_SIZE;
    std::vector<unsigned char> cov((size_t)N * N, 0);
    long long covered = 0;
    for (int i = 0; i < tris; ++i) {
        double ax = (fill[6*i]   - t.originX) / cell, ay = (fill[6*i+1] - t.originY) / cell;
        double bx = (fill[6*i+2] - t.originX) / cell, by = (fill[6*i+3] - t.originY) / cell;
        double cx = (fill[6*i+4] - t.originX) / cell, cy = (fill[6*i+5] - t.originY) / cell;
        int gx0 = std::max(0, (int)std::floor(std::min({ax, bx, cx})));
        int gx1 = std::min(N - 1, (int)std::ceil(std::max({ax, bx, cx})));
        int gy0 = std::max(0, (int)std::floor(std::min({ay, by, cy})));
        int gy1 = std::min(N - 1, (int)std::ceil(std::max({ay, by, cy})));
        for (int gy = gy0; gy <= gy1; ++gy)
            for (int gx = gx0; gx <= gx1; ++gx) {
                if (cov[(size_t)gy * N + gx]) continue;
                if (inTri(gx + 0.5, gy + 0.5, ax, ay, bx, by, cx, cy)) {
                    cov[(size_t)gy * N + gx] = 1;
                    ++covered;
                }
            }
    }
    // 统计每行覆盖(看是否有横向条纹/洞)
    long long fullRows = 0;
    long long minRow = N, maxRow = 0;
    for (int gy = 0; gy < N; ++gy) {
        long long c = 0;
        for (int gx = 0; gx < N; ++gx) c += cov[(size_t)gy * N + gx];
        minRow = std::min(minRow, c);
        maxRow = std::max(maxRow, c);
        if (c >= N * 95 / 100) ++fullRows;
    }
    printf("覆盖率 = %.1f%% (%lld/%d)  满行(>=95%%)=%lld/%d  单行最少=%lld 最多=%lld\n",
           100.0 * (double)covered / (double)(N * N), covered, N * N, fullRows, N, minRow, maxRow);
    return 0;
}
