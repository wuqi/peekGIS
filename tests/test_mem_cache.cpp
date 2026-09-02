#include "doctest.h"
#include "data/gdal_common.h"
#include "data/geom_cache.h"
#include "data/geom_util.h"
#include "config/app_config.h"
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {
double nowSec() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}
long long peakWorkingSetKB() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (long long)pmc.PeakWorkingSetSize / 1024;
    return -1;
}
void fillMeta(GDALDatasetH ds, int li, VectorData& vd) {
    OGRLayerH lyr = GDALDatasetGetLayer(ds, li);
    vd.name = OGR_L_GetName(lyr);
    long long n = OGR_L_GetFeatureCount(lyr, FALSE);
    vd.featureCount = (n >= 0) ? (long long)n : -1;
    OGRSpatialReferenceH srs = OGR_L_GetSpatialRef(lyr);
    if (srs) {
        const char* auth = OSRGetAuthorityName(srs, nullptr);
        const char* code = OSRGetAuthorityCode(srs, nullptr);
        if (code) vd.srcEpsg = std::atoi(code);
        vd.sourceCrs = (auth && code) ? std::string(auth) + ":" + code : "unknown";
    } else vd.sourceCrs = "unknown";
    vd.minx = vd.miny = 1e300;
    vd.maxx = vd.maxy = -1e300;
}
}

// 内存探针: 设置 PEEKGIS_TEST_MEMCACHE=<shp 路径> 时, 实测该文件流式写缓存的内存峰值。
// 验证分块流式(不整层驻留)后峰值应远低于旧 5.3GB, 目标 <2GB。
TEST_CASE("memcache streaming write probe") {
    const char* p = std::getenv("PEEKGIS_TEST_MEMCACHE");
    if (!p || !*p) return;

    std::string path = p;
    ensureGdal();
    AppConfig cfg;
    cfg.cache_dir = "C:/tmp/pgc_mem_probe";
    cfg.cache_max_mb = 64;
    std::error_code ec;
    std::filesystem::remove_all("C:/tmp/pgc_mem_probe", ec);

    GDALDatasetH ds = gdalOpenVector(path);
    REQUIRE(ds != nullptr);
    int nLayer = GDALDatasetGetLayerCount(ds);
    REQUIRE(nLayer >= 1);

    std::vector<VectorData> merged(nLayer);
    for (int mi = 0; mi < nLayer; mi++) fillMeta(ds, mi, merged[mi]);

    CacheWriter* cw = cacheWriterOpen(path, merged, cfg);

    const long long kChunkFeature = 20000;
    long long processed = 0;
    long long segs = 0, pts = 0;
    double t0 = nowSec();
    for (int mi = 0; mi < nLayer; mi++) {
        OGRLayerH lyr = GDALDatasetGetLayer(ds, mi);
        OGR_L_ResetReading(lyr);
        std::vector<float> local, localTris, localPts;
        long long within = 0;
        OGRFeatureH f;
        while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
            addFilledGeometry(OGR_F_GetGeometryRef(f), local, localTris, &localPts);
            OGR_F_Destroy(f);
            processed++; within++;
            if (within % kChunkFeature == 0) {
                segs += (long long)local.size() / 2;
                pts += (long long)localPts.size() / 2;
                if (cw) cacheWriterAppend(cw, mi, local, localPts, localTris);
                local.clear(); localTris.clear(); localPts.clear();
            }
        }
        segs += (long long)local.size() / 2;
        pts += (long long)localPts.size() / 2;
        if (cw && (!local.empty() || !localTris.empty() || !localPts.empty()))
            cacheWriterAppend(cw, mi, local, localPts, localTris);
    }
    double dt = nowSec() - t0;
    long long peak = peakWorkingSetKB();
    if (cw) cacheWriterClose(cw);
    GDALClose(ds);

    fprintf(stderr,
        "[memcache] layers=%d features=%lld segs=%lld pts=%lld dt=%.2fs PEAK=%.1f MB (%.2f GB)\n",
        nLayer, processed, segs, pts, dt, peak / 1024.0, peak / 1048576.0);
    MESSAGE("memcache probe done (see stderr)");
}
