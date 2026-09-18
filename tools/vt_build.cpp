// vt_build -- 构建 v2 矢量瓦片缓存(CLI)。用法:
//   vt_build <src> <out.vtk> [--layer N] [--epsg D] [--levels L]
//                            [--target V] [--cap C] [--lru M] [--quiet]
#include "vt/vt_build.h"
#include "vt/vt_cache.h"
#include "vt/vt_source.h"
#include "platform/exe_path.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace peekg::vt;

static void usage() {
    printf(
        "vt_build -- 构建 v2 矢量瓦片缓存\n"
        "用法: vt_build <src> [<out.vtk>] [--auto] [--cachedir D] [--layer N]\n"
        "                     [--epsg D] [--levels L] [--target V] [--cap C]\n"
        "                     [--lru M] [--no-simplify] [--sfactor F] [--quiet]\n"
        "  --auto      输出到 <cachedir>/vtk/<hash>_e<src>_d<dst>.vtk(peekGIS 会自动发现)\n"
        "  --cachedir D 缓存根目录(默认 cache)\n"
        "  --layer N   图层序号(默认 0)\n"
        "  --epsg D    显示 EPSG(默认 0=用源 EPSG)\n"
        "  --levels L  强制最深层(默认 -1=自动估算)\n"
        "  --target V  目标每瓦片顶点(默认 2048)\n"
        "  --cap C     最深层上限(默认 12)\n"
        "  --lru M     内存瓦片 LRU 上限(顶点数, 默认 100000000)\n"
        "  --no-simplify  关闭各层抽稀(体积更大)\n"
        "  --sfactor F 抽稀容差=格距*F(默认 1.0)\n");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) { usage(); return 1; }

    std::string src = argv[1];
    std::string out;
    std::string cacheDir = "cache";
    bool autoPath = false;
    VtBuildConfig cfg;
    bool quiet = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "缺少 %s 参数\n", a.c_str()); exit(1); }
            return argv[++i];
        };
        if (a == "--auto") autoPath = true;
        else if (a == "--cachedir") cacheDir = next();
        else if (a == "--layer") cfg.layerIdx = std::atoi(next().c_str());
        else if (a == "--epsg") cfg.dstEpsg = std::atoi(next().c_str());
        else if (a == "--levels") cfg.levels = std::atoi(next().c_str());
        else if (a == "--target") cfg.targetVerts = std::atoi(next().c_str());
        else if (a == "--cap") cfg.maxLevelCap = std::atoi(next().c_str());
        else if (a == "--lru") cfg.lruVerts = std::atof(next().c_str());
        else if (a == "--no-simplify") cfg.simplify = false;
        else if (a == "--sfactor") cfg.simplifyFactor = std::atof(next().c_str());
        else if (a == "--quiet") quiet = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] != '-') out = a;
    }

    if (out.empty()) {
        if (!autoPath) { usage(); return 1; }
        // 相对缓存目录按 exe 目录解析(与 peekGIS 运行期一致), 否则 app 发现不了产物。
        std::filesystem::path cd(cacheDir);
        if (!cd.is_absolute()) {
            std::string e = exeDir();
            if (!e.empty()) cacheDir = e + "/" + cacheDir;
        }
        LayerInfo li;
        if (!readVtLayerInfo(src, cfg.layerIdx, li)) {
            printf("读取图层信息失败, 无法自动定位缓存路径\n");
            return 1;
        }
        int dst = cfg.dstEpsg > 0 ? cfg.dstEpsg : li.srcEpsg;
        out = vtCachePath(cacheDir, src, li.srcEpsg, dst);
    }

    printf("构建: %s -> %s\n", src.c_str(), out.c_str());
    VtBuildStats st;
    auto onTile = [&](int L, int tx, int ty) {
        if (!quiet && (st.tilesWritten % 500) == 0)
            printf("  瓦片 %lld  (L%d %d,%d)\n", st.tilesWritten, L, tx, ty);
    };
    auto onProg = [&](int pct) {
        static int last = -1;
        if (!quiet && pct >= last + 10) { last = pct; printf("  建缓存 %d%%\n", pct); }
    };
    if (!buildVtCache(src, cfg.layerIdx, out, cfg, st, onTile, onProg)) {
        printf("构建失败\n");
        return 1;
    }

    printf("\n=== 构建完成 ===\n");
    printf("要素=%lld  源环=%lld  源顶点=%lld\n", st.features, st.rings, st.srcVerts);
    printf("写出瓦片=%lld  存储顶点=%lld  数据字节=%llu (%.2f MB)\n",
           st.tilesWritten, st.storedVerts, (unsigned long long)st.dataBytes,
           st.dataBytes / 1048576.0);
    printf("最深层=%d  用时=%.1fs\n", st.maxLevel, st.seconds);

    VtCache c;
    if (c.open(out)) {
        const VtFileHeader& h = c.header();
        printf("头: epsg src=%d dst=%d  origin=(%.6f,%.6f)  tileW0=%.6f  levels=%u built=0x%x\n",
               h.srcEpsg, h.dstEpsg, h.originX, h.originY, h.tileW0, h.maxLevel,
               h.fullyBuiltLevels);
        c.close();
    }
    return 0;
}
