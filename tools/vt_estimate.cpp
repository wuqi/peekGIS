// vt_estimate -- 矢量金字塔快速估算(只读诊断工具, 不改主程序/不写缓存)
//
// 目的: 在构建 v2 矢量瓦片缓存前, 用"随机 FID 采样"快速估出:
//   1) 源要素数 F、原始顶点总数 N (用于 v1.0/v2 路由);
//   2) 各候选最细层 L 降采样后的总点数 P(L)、每瓦片点数 P(L)/4^L;
//   3) 选出让 P/瓦片最接近 targetVertsPerTile 的最细层 L*;
//   4) 金字塔磁盘占用、v1.0 每帧三角数, 给出路由建议。
//
// 采样是无偏的(按 FID 均匀随机), 避免"按存储顺序取前 N 个"的低估
// (实测广东前 2 万要素 avg=32.8 点, 全量 93.8 点)。
//
// 用法:
//   vt_estimate <dataset> [--layer N] [--samples K] [--target V] [--cap L]
//                         [--seed S] [--full]
//   --full  : 全量精确扫描(慢, 用于校验采样估计)

#include "data/gdal_common.h"
#include <ogr_api.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <random>
#include <cmath>
#include <chrono>
#include <algorithm>

using namespace peekg::data;

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// 递归统计几何点数: 容器(多边形/多面/集合)有子几何则求和, 否则取点串长度。
// 与 GDAL SQL 的 ST_NPoints 语义一致(含所有环/所有部分/含闭合点)。
int countPoints(OGRGeometryH g) {
    if (!g) return 0;
    int n = OGR_G_GetGeometryCount(g);
    if (n > 0) {
        int s = 0;
        for (int i = 0; i < n; ++i) s += countPoints(OGR_G_GetGeometryRef(g, i));
        return s;
    }
    return OGR_G_GetPointCount(g);
}

std::string comma(long long v) {
    std::string s = std::to_string(v < 0 ? -v : v);
    std::string o;
    int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i) {
        o.push_back(s[i]);
        if (++c % 3 == 0 && i > 0) o.push_back(',');
    }
    if (v < 0) o.push_back('-');
    std::reverse(o.begin(), o.end());
    return o;
}

std::string human(double bytes) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (bytes >= 1024.0 && i < 4) { bytes /= 1024.0; ++i; }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %s", bytes, u[i]);
    return buf;
}

void usage() {
    printf(
        "vt_estimate -- 矢量金字塔快速估算(只读)\n"
        "用法: vt_estimate <dataset> [--layer N] [--samples K] [--target V]\n"
        "                              [--cap L] [--seed S] [--full]\n"
        "  --layer N   图层序号(默认 0)\n"
        "  --samples K 随机采样要素数(默认 40000)\n"
        "  --target V  目标每瓦片顶点数(默认 2048)\n"
        "  --cap L     最细层上限(默认 12)\n"
        "  --seed S    随机种子(默认 12345)\n"
        "  --full      全量精确扫描(慢, 用于校验)\n");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) { usage(); return 1; }

    std::string path;
    int layerIdx = 0;
    long long samples = 40000;
    double target = 2048.0;
    int cap = 12;
    unsigned seed = 12345;
    bool full = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "缺少 %s 参数\n", name); exit(1); }
            return argv[++i];
        };
        if (a == "--layer") layerIdx = std::atoi(next("--layer").c_str());
        else if (a == "--samples") samples = std::atoll(next("--samples").c_str());
        else if (a == "--target") target = std::atof(next("--target").c_str());
        else if (a == "--cap") cap = std::atoi(next("--cap").c_str());
        else if (a == "--seed") seed = (unsigned)std::strtoul(next("--seed").c_str(), nullptr, 10);
        else if (a == "--full") full = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else path = a;
    }
    if (path.empty()) { usage(); return 1; }
    if (cap < 1) cap = 1;

    ensureGdal();
    GDALDatasetH ds = gdalOpenVector(path);
    if (!ds) { printf("VECTOR OPEN FAIL: %s\n", path.c_str()); return 1; }
    int nl = GDALDatasetGetLayerCount(ds);
    if (layerIdx < 0 || layerIdx >= nl) {
        printf("layer %d 不存在(共 %d 层)\n", layerIdx, nl);
        GDALClose(ds);
        return 1;
    }
    OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);
    const char* lname = OGR_L_GetName(lyr);

    OGREnvelope env;
    bool hasExt = OGR_L_GetExtent(lyr, &env, TRUE) == OGRERR_NONE;
    long long F = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
    int epsg = gdalSrsEpsg(OGR_L_GetSpatialRef(lyr));

    printf("=== vt_estimate: %s ===\n", path.c_str());
    printf("layer[%d] %s  features=%s  epsg=%d\n", layerIdx, lname ? lname : "?", comma(F).c_str(), epsg);
    if (hasExt)
        printf("extent=(%.6f,%.6f)-(%.6f,%.6f)  span=(%.6f x %.6f)\n",
               env.MinX, env.MinY, env.MaxX, env.MaxY,
               env.MaxX - env.MinX, env.MaxY - env.MinY);
    else
        printf("extent=(不可用)\n");

    if (F <= 0) { printf("空图层\n"); GDALClose(ds); return 0; }

    double extentMax = hasExt ? std::max(env.MaxX - env.MinX, env.MaxY - env.MinY) : 1.0;
    if (extentMax <= 0) extentMax = 1.0;

    // 各层容差 t_L = extentMax / (512 * 2^L)  (对应 512 网格 / 每层 2x 降采样)
    std::vector<double> tol(cap + 1);
    for (int L = 0; L <= cap; ++L) tol[L] = extentMax / (512.0 * std::pow(2.0, L));

    std::vector<double> raw;                       // 每个样本的原始点数
    std::vector<std::vector<double>> simp(cap + 1);  // 每个样本在各层简化后的点数

    auto t0 = std::chrono::steady_clock::now();
    long long got = 0, tried = 0;
    long long maxRaw = 0;
    (void)seed;

    auto processFeature = [&](OGRFeatureH f) {
        OGRGeometryH g = OGR_F_GetGeometryRef(f);
        if (!g) return;
        int p = countPoints(g);
        raw.push_back((double)p);
        if (p > maxRaw) maxRaw = p;
        for (int L = 0; L <= cap; ++L) {
            OGRGeometryH sg = OGR_G_Simplify(g, tol[L]);
            simp[L].push_back((double)countPoints(sg));
            if (sg) OGR_G_DestroyGeometry(sg);
        }
        ++got;
    };

    if (full) {
        printf("全量扫描中...\n");
        OGR_L_ResetReading(lyr);
        OGRFeatureH f;
        while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
            processFeature(f);
            OGR_F_Destroy(f);
            if ((got % 200000) == 0 && got > 0) printf("  %s / %s\n", comma(got).c_str(), comma(F).c_str());
        }
        tried = F;
    } else {
        // 步进采样: 顺序读, 每 step 个取一个。FileGDB 的随机 GetFeature 实际是
        // 全表扫描(极慢), 因此只做顺序步进(有偏但快, 数量级足够)。
        long long step = std::max<long long>(1, F / std::max<long long>(1, samples));
        printf("步进采样 每 %lld 个取 1 (目标 %s 个)...\n", step, comma(samples).c_str());
        OGR_L_ResetReading(lyr);
        OGRFeatureH f;
        long long idx = 0;
        while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
            if (idx % step == 0) processFeature(f);
            OGR_F_Destroy(f);
            ++idx;
        }
        tried = idx;
    }

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    if (raw.empty()) { printf("未取到任何要素\n"); GDALClose(ds); return 1; }

    long long n = (long long)raw.size();
    double meanRaw = 0;
    for (double v : raw) meanRaw += v;
    meanRaw /= (double)n;

    printf("\n采样完成: %s 个要素, 用时 %.1fs\n", comma(n).c_str(), secs);
    printf("原始点数: 平均 %.2f, 样本最大 %s\n", meanRaw, comma(maxRaw).c_str());
    printf("原始总点数 N 估计 = %s\n", comma((long long)std::llround(meanRaw * (double)F)).c_str());

    // 各层总点数估计 = F * mean(simplified)
    printf("\n  L   tol(deg)        P(L) est        tiles     P/tile\n");
    std::vector<double> P(cap + 1, 0.0);
    for (int L = 0; L <= cap; ++L) {
        double m = 0;
        for (double v : simp[L]) m += v;
        m /= (double)simp[L].size();
        P[L] = m * (double)F;
        double tiles = std::pow(4.0, L);
        printf("  %-2d  %-12.3e  %14s  %11.0f  %10.1f\n",
               L, tol[L], comma((long long)std::llround(P[L])).c_str(), tiles, P[L] / tiles);
    }

    // 选最细层: 令 P/瓦片最接近 target
    int best = 0;
    double bestErr = 1e300;
    for (int L = 0; L <= cap; ++L) {
        double r = P[L] / std::pow(4.0, L);
        double err = std::fabs(r - target);
        if (err < bestErr) { bestErr = err; best = L; }
    }
    printf("\n目标每瓦片顶点 = %.0f  ->  最细层 L* = %d (P/tile=%.1f)\n",
           target, best, P[best] / std::pow(4.0, best));

    // 金字塔总体积(仅 L=0..L*)
    double sumPts = 0;
    for (int L = 0; L <= best; ++L) sumPts += P[L];
    double rawBytes = sumPts * 4.0;                 // int16 x,y = 4B/顶点
    printf("\n金字塔(L0..L%d)总点数估计 = %s\n", best, comma((long long)std::llround(sumPts)).c_str());
    printf("裸顶点字节 = %s  (4B/顶点)\n", human(rawBytes).c_str());
    printf("含环表/头 ~x1.3 = %s;  zstd 后约 %s ~ %s\n",
           human(rawBytes * 1.3).c_str(),
           human(rawBytes * 1.3 / 3.0).c_str(),
           human(rawBytes * 1.3 / 2.0).c_str());

    // v1.0 路由估计: v1.0 每帧画全部几何, 三角形 ~= 1.5 * 原始点数
    double v1tris = meanRaw * (double)F * 1.5;
    printf("\n[v1.0] 每帧三角数估计 = %s\n", comma((long long)std::llround(v1tris)).c_str());
    if (v1tris < 5e6)        printf("建议: v1.0 (平滑, 秒开)\n");
    else if (v1tris < 2e7)   printf("建议: v1.0 默认 / v2 可选\n");
    else                     printf("建议: v2 (v1.0 会明显掉帧)\n");

    GDALClose(ds);
    return 0;
}
