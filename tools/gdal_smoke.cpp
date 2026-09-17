#include "data/geom_cache.h"
#include "config/app_config.h"
#include <cstdio>
#include <algorithm>

using namespace peekg::data;

#ifdef _WIN32
#include <windows.h>
static std::string toUtf8Arg(int i, char** /*argv*/) {
    int n = 0;
    wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w || i >= n) { if (w) LocalFree(w); return ""; }
    int c = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(c > 0 ? c - 1 : 0, '\0');
    if (c > 0) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], c, nullptr, nullptr);
    LocalFree(w);
    return s;
}
#else
static std::string toUtf8Arg(int i, char** argv) { return argv[i]; }
#endif

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s <vector>\n", argv[0]); return 1; }
    std::string path = toUtf8Arg(1, argv);
    AppConfig cfg;
    cfg.cache_dir = "C:/tmp/pgc_test_cache";
    cfg.cache_max_mb = 512;

    std::vector<VectorData> vds;
    if (!GeomCache::loadVectorCachedAll(path, vds, cfg)) { printf("load FAILED: %s\n", path.c_str()); return 2; }
    printf("[result] layers=%d\n", (int)vds.size());
    long long total = 0;
    for (size_t i = 0; i < vds.size(); i++) {
        auto& v = vds[i];
        total += (long long)v.vertices.size();
        printf("  [%d] %s  crs=%s epsg=%d  feats=%lld  verts=%lld  extent=[%.3f,%.3f,%.3f,%.3f]\n",
               (int)i, v.name.c_str(), v.sourceCrs.c_str(), v.srcEpsg,
               v.featureCount, (long long)v.vertices.size(),
               v.minx, v.miny, v.maxx, v.maxy);
    }
    printf("[total] verts=%lld\n", total);
    return 0;
}
