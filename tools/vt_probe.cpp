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

    // PEEK_VT_MAP="lon0,lat0,lon1,lat1[,cols]" -> 在经纬窗口里打 ASCII 覆盖图(诊断"只画了一半")
    if (const char* mk = std::getenv("PEEK_VT_MAP")) {
        double mx0 = 0, my0 = 0, mx1 = 0, my1 = 0;
        int mcols = 96;
        if (std::sscanf(mk, "%lf,%lf,%lf,%lf,%d", &mx0, &my0, &mx1, &my1, &mcols) >= 4) {
            if (mcols < 8) mcols = 8;
            if (mcols > 400) mcols = 400;
            if (mx1 < mx0) { double s = mx0; mx0 = mx1; mx1 = s; }
            if (my1 < my0) { double s = my0; my0 = my1; my1 = s; }
            const double latRef = (my0 + my1) * 0.5;
            const double kx = 111.32 * std::cos(latRef * 3.14159265358979 / 180.0);
            int mrows = (int)((double)mcols * ((my1 - my0) * 110.57) / ((mx1 - mx0) * kx) * 0.5);
            if (mrows < 4) mrows = 4;
            if (mrows > 200) mrows = 200;
            printf("MAP window=(%.6f,%.6f)-(%.6f,%.6f) grid=%dx%d latRef=%.3f\n",
                   mx0, my0, mx1, my1, mcols, mrows, latRef);
            long long inFill = 0, inTot = 0;
            for (int r = mrows - 1; r >= 0; --r) {          // 北在上
                const double wy = my0 + (my1 - my0) * ((double)r + 0.5) / mrows;
                for (int c = 0; c < mcols; ++c) {
                    const double wx = mx0 + (mx1 - mx0) * ((double)c + 0.5) / mcols;
                    const int gx = (int)std::floor((wx - t.originX) / cell);
                    const int gy = (int)std::floor((wy - t.originY) / cell);
                    bool f = false;
                    if (gx >= 0 && gy >= 0 && gx < N && gy < N) {
                        f = cov[(size_t)gy * N + gx] != 0;
                        ++inTot; if (f) ++inFill;
                    }
                    putchar(f ? '#' : '.');
                }
                putchar('\n');
            }
            printf("MAP filled=%lld/%lld (%.1f%%)\n", inFill, inTot, inTot ? 100.0 * (double)inFill / inTot : 0.0);
        }
    }

    // PEEK_VT_HOLES=1 -> 列出全部内部洞像素坐标(聚簇定位屏幕上可见的洞)
    if (std::getenv("PEEK_VT_HOLES") != nullptr) {
        std::vector<unsigned char> out1(cov.size(), 0);
        std::vector<int> st1;
        for (int gx = 0; gx < N; ++gx) { st1.push_back(gx); st1.push_back((N - 1) * N + gx); }
        for (int gy = 0; gy < N; ++gy) { st1.push_back(gy * N); st1.push_back(gy * N + (N - 1)); }
        while (!st1.empty()) {
            int p = st1.back(); st1.pop_back();
            if (out1[p] || cov[p]) continue;
            out1[p] = 1;
            int gx = p % N, gy = p / N;
            if (gx > 0) st1.push_back(p - 1);
            if (gx < N - 1) st1.push_back(p + 1);
            if (gy > 0) st1.push_back(p - N);
            if (gy < N - 1) st1.push_back(p + N);
        }
        int nh2 = 0;
        for (int gy = 0; gy < N; ++gy)
            for (int gx = 0; gx < N; ++gx) {
                size_t p = (size_t)gy * N + gx;
                if (!cov[p] && !out1[p]) {
                    printf("hole g=(%d,%d) w=(%.6f,%.6f)\n", gx, gy,
                           t.originX + gx * cell, t.originY + gy * cell);
                    if (++nh2 >= 400) goto holes_done;
                }
            }
holes_done:
        printf("hole列全: %d\n", nh2);
    }

    // PEEK_VT_FIND="lon,lat[,rtol]" -> 打印 bbox 含该点的环 + 该处光栅覆盖(确认要素是否在且被填充)
    if (const char* fk = std::getenv("PEEK_VT_FIND")) {
        double fx = 0, fy = 0, rtol = 1e-9;
        if (std::sscanf(fk, "%lf,%lf,%lf", &fx, &fy, &rtol) >= 2) {
            int found = 0;
            for (const VtRing& r : t.rings) {
                if (r.type != RING_FACE || r.vertexCount < 1) continue;
                const int16_t* p = t.verts.data() + (size_t)r.firstVertex * 2;
                long mnx = 32767, mxx = -32768, mny = 32767, mxy = -32768;
                for (uint32_t k = 0; k < r.vertexCount; ++k) {
                    mnx = std::min<long>(mnx, p[2*k]); mxx = std::max<long>(mxx, p[2*k]);
                    mny = std::min<long>(mny, p[2*k+1]); mxy = std::max<long>(mxy, p[2*k+1]);
                }
                double wx0 = t.originX + mnx * cell, wx1 = t.originX + mxx * cell;
                double wy0 = t.originY + mny * cell, wy1 = t.originY + mxy * cell;
                if (fx >= wx0 - rtol && fx <= wx1 + rtol && fy >= wy0 - rtol && fy <= wy1 + rtol) {
                    printf("  hit ring#%u hole=%d polyGroup=%u verts=%u bbox=(%.6f,%.6f)-(%.6f,%.6f)\n",
                           (unsigned)(&r - t.rings.data()), r.hole, r.polyGroup, r.vertexCount,
                           wx0, wy0, wx1, wy1);
                    for (uint32_t k = 0; k < r.vertexCount; ++k)
                        printf("    v%d=(%.6f,%.6f) g=(%d,%d)\n", k,
                               t.originX + p[2*k] * cell, t.originY + p[2*k+1] * cell,
                               p[2*k], p[2*k+1]);
                    int gx0 = std::max(0, (int)mnx), gx1 = std::min(N - 1, (int)mxx);
                    int gy0 = std::max(0, (int)mny), gy1 = std::min(N - 1, (int)mxy);
                    long long c0 = 0, tot = 0;
                    for (int gy = gy0; gy <= gy1; ++gy)
                        for (int gx = gx0; gx <= gx1; ++gx) { c0 += cov[(size_t)gy * N + gx]; tot++; }
                    printf("    bbox栅格=(%d,%d)-(%d,%d) 覆盖=%lld/%lld\n", gx0, gy0, gx1, gy1, c0, tot);
                    if (++found >= 8) break;
                }
            }
            printf("find(%g,%g): %d ring(s)\n", fx, fy, found);

            // 该点所在格 + 四周 12 格内的洞像素(确认洞是否就在该要素上)
            int cgx = (int)std::floor((fx - t.originX) / cell);
            int cgy = (int)std::floor((fy - t.originY) / cell);
            std::vector<unsigned char> out2(cov.size(), 0);
            std::vector<int> st2;
            for (int gx = 0; gx < N; ++gx) { st2.push_back(gx); st2.push_back((N - 1) * N + gx); }
            for (int gy = 0; gy < N; ++gy) { st2.push_back(gy * N); st2.push_back(gy * N + (N - 1)); }
            while (!st2.empty()) {
                int p = st2.back(); st2.pop_back();
                if (out2[p] || cov[p]) continue;
                out2[p] = 1;
                int gx = p % N, gy = p / N;
                if (gx > 0) st2.push_back(p - 1);
                if (gx < N - 1) st2.push_back(p + 1);
                if (gy > 0) st2.push_back(p - N);
                if (gy < N - 1) st2.push_back(p + N);
            }
            int nh = 0;
            for (int gy = std::max(0, cgy - 12); gy <= std::min(N - 1, cgy + 12); ++gy)
                for (int gx = std::max(0, cgx - 12); gx <= std::min(N - 1, cgx + 12); ++gx) {
                    size_t p = (size_t)gy * N + gx;
                    if (!cov[p] && !out2[p]) {
                        printf("  near-hole g=(%d,%d) w=(%.6f,%.6f) dist_cell=(%d,%d)\n", gx, gy,
                               t.originX + gx * cell, t.originY + gy * cell, gx - cgx, gy - cgy);
                        if (++nh >= 40) break;
                    }
                }
            printf("  point所在格g=(%d,%d) cov=%d  附近洞=%d\n", cgx, cgy, cov[(size_t)cgy * N + cgx], nh);
        }
    }

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
