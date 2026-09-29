// 命令行版补建空间索引(与图层右键菜单共用 src/data/spatial_index.h)。
// 用法: mkqix <src> [--rm]
//   <src> 可以是 .shp / .gpkg / PG 连接串; --rm 删除该驱动的空间索引(仅 shp/gpkg)。
// 判据: 关掉重开后 OLCFastSpatialFilter == 1(索引真正落盘生效)。
#include "data/spatial_index.h"
#include <cstdio>
#include <string>
#include <chrono>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) { std::printf("用法: mkqix <shp|gpkg|PG:连接串> [--rm]\n"); return 1; }
    peekg::data::ensureGdal();
    std::string path = argv[1];
    const bool rm = (argc >= 3 && std::string(argv[2]) == "--rm");

    auto t0 = std::chrono::steady_clock::now();
    bool before = peekg::data::vectorHasSpatialIndex(path, 0);
    std::printf("建索引前 OLCFastSpatialFilter=%d\n", (int)before);

    if (rm) {
        GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                                     nullptr, nullptr, nullptr);
        if (!ds) { std::printf("打开失败(可能已在别处打开): %s\n", path.c_str()); return 1; }
        OGRLayerH lyr = GDALDatasetGetLayer(ds, 0);
        const char* lname = lyr ? OGR_L_GetName(lyr) : "";
        GDALDriverH dr = GDALGetDatasetDriver(ds);
        const char* drvName = dr ? GDALGetDriverShortName(dr) : "";
        std::string drv = drvName ? drvName : "";
        std::string sql;
        if (drv == "ESRI Shapefile") sql = std::string("DROP SPATIAL INDEX ON ") + lname;
        else if (drv == "GPKG") sql = "SELECT DisableSpatialIndex('" + std::string(lname) + "')";
        else { std::printf("驱动 %s 不支持删除索引\n", drvName); GDALClose(ds); return 2; }
        OGRLayerH res = GDALDatasetExecuteSQL(ds, sql.c_str(), nullptr, nullptr);
        if (res) GDALDatasetReleaseResultSet(ds, res);
        GDALClose(ds);
        std::printf("已执行 %s\n", sql.c_str());
        return 0;
    }

    auto r = peekg::data::buildVectorSpatialIndex(path, 0);
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    bool after = peekg::data::vectorHasSpatialIndex(path, 0);
    std::printf("结果=%s  重开后 OLCFastSpatialFilter=%d  耗时 %.1f s\n",
                r == peekg::data::QixResult::Ok ? "Ok"
                    : (r == peekg::data::QixResult::NoGeometry ? "NoGeometry" : "Failed"),
                (int)after, dt);
    return after ? 0 : 3;
}
