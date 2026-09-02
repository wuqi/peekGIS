// fix_shx <shp路径>  -- 单线程重建缺失/损坏的 .shx
// 用途: 只读矢量文件的 .shx 索引缺失或损坏时, 用 GDAL 强制扫描全部记录并重建索引。
// 关键: 单线程执行(无并发写), 确保不会多线程抢写同一 .shx 导致损坏。
#include <gdal.h>
#include <cpl_conv.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fix_shx <shp_path>\n");
        return 2;
    }
    GDALAllRegister();
    CPLSetConfigOption("GDAL_DATA", "share/gdal");
    CPLSetConfigOption("PROJ_LIB", "share/proj");
    const char* path = argv[1];

    // SHAPE_RESTORE_SHX=YES: 扫描整个 .shp, 把正确的索引写回 .shx。
    // 这是唯一允许的"写"路径, 且在本工具中单线程执行。
    CPLSetConfigOption("SHAPE_RESTORE_SHX", "YES");
    GDALDatasetH ds = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) {
        fprintf(stderr, "GDALOpenEx FAILED: %s\n", CPLGetLastErrorMsg());
        return 1;
    }
    OGRLayerH lyr = GDALDatasetGetLayer(ds, 0);
    long long n = OGR_L_GetFeatureCount(lyr, TRUE);
    fprintf(stderr, "features=%lld\n", n);
    GDALClose(ds);
    CPLSetConfigOption("SHAPE_RESTORE_SHX", "NO");
    return 0;
}
