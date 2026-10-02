// vt_probe -- 读某层某瓦片, 把面填充光栅化, 报覆盖率 + 内部洞(诊断切片是否有洞)。
// 用法: vt_probe <cache.vtk> [level] [tx] [ty] [cellPx]
//   省略 level -> 用 maxLevel; 省略 tx/ty -> 取该层中心瓦片。
//   cellPx = 每格占多少屏幕像素(= cell/scale)。给定时按渲染器口径过滤亚像素小面
//   (minFillCells = 1/cellPx^2, 见 vt_render.cpp), 复现 app 真实画面; 省略=不过滤。
// 环境变量 PEEK_VT_SWEEP=1 -> 额外整层扫描, 报各瓦片覆盖率分布。
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

// 把 fill 三角形光栅化到 N×N 覆盖掩膜
static void raster(const VtTile& t, const std::vector<float>& fill, double cell, int N,
                   std::vector<unsigned char>& cov) {
    cov.assign((size_t)N * N, 0);
    int tris = (int)(fill.size() / 6);
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
                if (inTri(gx + 0.5, gy + 0.5, ax, ay, bx, by, cx, cy)) cov[(size_t)gy * N + gx] = 1;
            }
    }
}

// 从片边界洪泛, 统计"被填充区包住、但没被填充"的像素 = 真洞(与"数据本来就没有"区分开)
static long long interiorHoles(const std::vector<unsigned char>& cov, int N) {
    std::vector<unsigned char> out(cov.size(), 0);
    std::vector<int> st;
    st.reserve((size_t)N * 4);
    for (int gx = 0; gx < N; ++gx) {
        st.push_back(gx); st.push_back((N - 1) * N + gx);
    }
    for (int gy = 0; gy < N; ++gy) {
        st.push_back(gy * N); st.push_back(gy * N + (N - 1));
    }
    while (!st.empty()) {
        int p = st.back(); st.pop_back();
        if (out[p]) continue;
        if (cov[p]) continue;                 // 填充像素不可走
        out[p] = 1;
        int gx = p % N, gy = p / N;
        if (gx > 0)         st.push_back(p - 1);
        if (gx < N - 1)     st.push_back(p + 1);
        if (gy > 0)         st.push_back(p - N);
        if (gy < N - 1)     st.push_back(p + N);
    }
    long long holes = 0;
    for (size_t i = 0; i < cov.size(); ++i)
        if (!cov[i] && !out[i]) ++holes;
    return holes;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) { printf("用法: vt_probe <cache.vtk> [level] [tx] [ty] [cellPx]\n"); return 1; }
    std::string path = argv[1];
    VtCache c;
    if (!c.open(path)) { printf("打开失败: %s\n", path.c_str()); return 1; }
    const VtFileHeader& h = c.header();
    int level = (argc > 2) ? std::atoi(argv[2]) : (int)h.maxLevel;
    int n = 1 << level;
    int tx = (argc > 3) ? std::atoi(argv[3]) : n / 2;
    int ty = (argc > 4) ? std::atoi(argv[4]) : n / 2;
    double cellPx = (argc > 5) ? std::atof(argv[5]) : 0.0;
    double minFill = (cellPx > 0) ? 1.0 / (cellPx * cellPx) : 0.0;
    printf("cache=%s maxLevel=%u tileW0=%.6f\n", path.c_str(), h.maxLevel, h.tileW0);
    printf("probe L=%d tile=(%d,%d) of %dx%d  grid=%d  cellPx=%.3f  minFillCells=%.4f\n",
           level, tx, ty, n, n, tileSizeAt(level, (int)h.maxLevel), cellPx, minFill);

    VtTile t;
    if (!c.readTile(level, tx, ty, t)) { printf("该瓦片不存在(空)\n"); return 2; }
    double tileW = h.tileW0 / (double)n;
    const int N = tileSizeAt(level, (int)h.maxLevel);   // 最深层是 1024 格, 不能硬编码 512
    double cell = tileW / (double)N;

    int nface = 0, nline = 0, npt = 0, nhole = 0, nouter = 0;
    for (const VtRing& r : t.rings) {
        if (r.type == RING_FACE) { nface++; if (r.hole) nhole++; else nouter++; }
        else if (r.type == RING_LINE) nline++;
        else npt++;
    }
    printf("rings: face=%d (outer=%d hole=%d) line=%d point=%d  verts=%u\n",
           nface, nouter, nhole, nline, npt, t.vertexCount());

    std::vector<float> lines, points, fill;
    buildTileGeometry(t, cell, true, lines, points, fill, minFill);
    int tris = (int)(fill.size() / 6);
    printf("fill triangles=%d  line verts=%d  point verts=%d\n", tris, (int)(lines.size() / 2), (int)(points.size() / 2));

    std::vector<unsigned char> cov;
    raster(t, fill, cell, N, cov);
    long long covered = 0;
    for (size_t i = 0; i < cov.size(); ++i) covered += cov[i];
    long long holes = interiorHoles(cov, N);
    long long fullRows = 0, minRow = N, maxRow = 0;
    for (int gy = 0; gy < N; ++gy) {
        long long r = 0;
        for (int gx = 0; gx < N; ++gx) r += cov[(size_t)gy * N + gx];
        minRow = std::min(minRow, r); maxRow = std::max(maxRow, r);
        if (r >= N * 95 / 100) ++fullRows;
    }
    printf("覆盖率 = %.2f%% (%lld/%d)  内部洞像素 = %lld (%.3f%%)  满行(>=95%%)=%lld/%d  单行 %lld~%lld\n",
           100.0 * (double)covered / (double)(N * N), covered, N * N,
           holes, 100.0 * (double)holes / (double)(N * N),
           fullRows, N, minRow, maxRow);

    if (std::getenv("PEEK_VT_SWEEP") == nullptr) return 0;
    int tot = 0, empty = 0, lt50 = 0, lt90 = 0;
    double worst = 100.0, sum = 0, sumHole = 0;
    int wx = -1, wy = -1;
    long long worstHole = -1;
    for (int jy = 0; jy < n; ++jy) {
        for (int jx = 0; jx < n; ++jx) {
            VtTile u;
            if (!c.readTile(level, jx, jy, u)) continue;
            std::vector<float> l2, p2, f2;
            buildTileGeometry(u, cell, true, l2, p2, f2, minFill);
            if (f2.empty()) { empty++; continue; }
            std::vector<unsigned char> cv2;
            raster(u, f2, cell, N, cv2);
            long long cv = 0;
            for (size_t i = 0; i < cv2.size(); ++i) cv += cv2[i];
            double pc = 100.0 * (double)cv / (double)((size_t)N * N);
            long long ih = interiorHoles(cv2, N);
            sum += pc; sumHole += (double)ih; ++tot;
            if (pc < 50) ++lt50;
            if (pc < 90) ++lt90;
            if (pc < worst) { worst = pc; wx = jx; wy = jy; }
            if (ih > worstHole) worstHole = ih;
        }
    }
    printf("[整层 L%d] 有数据瓦片=%d  <50%%=%d  <90%%=%d  平均覆盖=%.1f%%  最差覆盖=%.1f%% @(%d,%d)  平均洞像素=%.0f  最多洞像素=%lld\n",
           level, tot, lt50, lt90, tot ? sum / tot : 0.0, worst, wx, wy,
           tot ? sumHole / tot : 0.0, worstHole);
    return 0;
}
