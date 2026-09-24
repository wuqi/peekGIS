// 临时探针: 验证 OGR shapefile 驱动对 .qix 空间索引的过滤行为(读 cuenta/过滤读取速度)。
// 用法: qix_probe <shp> <minx> <miny> <maxx> <maxy>
#include "data/gdal_common.h"
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <string>

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 6) {
        std::printf("用法: qix_probe <shp> <minx> <miny> <maxx> <maxy>\n");
        return 1;
    }
    peekg::data::ensureGdal();
    std::string path = argv[1];
    double rx0 = std::atof(argv[2]), ry0 = std::atof(argv[3]);
    double rx1 = std::atof(argv[4]), ry1 = std::atof(argv[5]);

    auto t0 = std::chrono::steady_clock::now();
    GDALDatasetH ds = peekg::data::gdalOpenVector(path);
    auto t1 = std::chrono::steady_clock::now();
    if (!ds) { std::printf("打开失败\n"); return 1; }
    int nl = GDALDatasetGetLayerCount(ds);
    std::printf("打开耗时 %.1fms, 层数=%d\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count(), nl);
    OGRLayerH lyr = GDALDatasetGetLayer(ds, 0);
    if (!lyr) { std::printf("无图层\n"); return 2; }

    OGRSpatialReferenceH srs = OGR_L_GetSpatialRef(lyr);
    int srcEpsg = peekg::data::gdalSrsEpsg(srs);
    std::printf("源 EPSG=%d\n", srcEpsg);
    std::printf("OLCFastSpatialFilter = %d\n",
                (int)OGR_L_TestCapability(lyr, OLCFastSpatialFilter));
    std::printf("OLCFastSetNextByIndex = %d\n",
                (int)OGR_L_TestCapability(lyr, OLCFastSetNextByIndex));

    // vt 直读只要几何: 跳过全部属性读(DBF 101MB), 验证对耗时的影响
    if (OGR_L_GetLayerDefn(lyr)) {
        const char* ignored[] = {"*", nullptr};
        OGR_L_SetIgnoredFields(lyr, ignored);
    }
    t0 = std::chrono::steady_clock::now();
    long long total = (long long)OGR_L_GetFeatureCount(lyr, FALSE);
    t1 = std::chrono::steady_clock::now();
    std::printf("无过滤总数(FALSE) = %lld, %.1fms\n", total,
                std::chrono::duration<double, std::milli>(t1 - t0).count());

    t0 = std::chrono::steady_clock::now();
    OGR_L_SetSpatialFilterRect(lyr, rx0, ry0, rx1, ry1);
    long long filtered = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
    t1 = std::chrono::steady_clock::now();
    std::printf("过滤矩形(%.2f,%.2f)-(%.2f,%.2f) 命中 = %lld, %.1fms\n",
                rx0, ry0, rx1, ry1, filtered,
                std::chrono::duration<double, std::milli>(t1 - t0).count());

    t0 = std::chrono::steady_clock::now();
    long long read = 0;
    OGRFeatureH f;
    OGR_L_ResetReading(lyr);
    while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        ++read;
        OGR_F_Destroy(f);
    }
    t1 = std::chrono::steady_clock::now();
    std::printf("过滤遍历读取 = %lld 要素, %.1fms\n", read,
                std::chrono::duration<double, std::milli>(t1 - t0).count());

    GDALClose(ds);
    return 0;
}