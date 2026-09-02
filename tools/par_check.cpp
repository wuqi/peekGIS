#include <gdal.h>
#include <ogr_api.h>
#include <ogrsf_frmts.h>
#include <cstdio>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <fstream>

#include "data/geom_util.h"

static bool exists(const char* p) {
    std::ifstream f(p);
    return (bool)f;
}

static void createShp(const char* path, long long n) {
    GDALDriverH drv = GDALGetDriverByName("ESRI Shapefile");
    GDALDatasetH ds = GDALCreate(drv, path, 0, 0, 0, GDT_Unknown, nullptr);
    OGRLayerH lyr = GDALDatasetCreateLayer(ds, "lines", nullptr, wkbLineString, nullptr);
    for (long long i = 0; i < n; i++) {
        OGRFeatureH f = OGR_F_Create(OGR_L_GetLayerDefn(lyr));
        OGRGeometryH g = OGR_G_CreateGeometry(wkbLineString);
        double x = (double)(i % 1000);
        OGR_G_AddPoint(g, x, (double)(i / 1000), 0);
        OGR_G_AddPoint(g, x + 0.5, (double)(i / 1000) + 0.5, 0);
        OGR_G_AddPoint(g, x + 1.0, (double)(i / 1000), 0);
        OGR_F_SetGeometryDirectly(f, g);
        OGR_L_CreateFeature(lyr, f);
        OGR_F_Destroy(f);
    }
    GDALClose(ds);
}

int main(int argc, char** argv) {
    GDALAllRegister();
    CPLSetConfigOption("SHAPE_RESTORE_SHX", "YES");
    const char* path = (argc > 1) ? argv[1] : "C:/tmp/par_check.shp";
    long long n = (argc > 2) ? _atoi64(argv[2]) : 50000;
    if (!exists(path)) createShp(path, n);

    // 顺序读
    GDALDatasetH ds = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    OGRLayerH lyr = GDALDatasetGetLayer(ds, 0);
    long long count = OGR_L_GetFeatureCount(lyr, TRUE);
    std::vector<float> seq;
    OGR_L_ResetReading(lyr);
    OGRFeatureH f;
    while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        addGeometry(OGR_F_GetGeometryRef(f), seq);
        OGR_F_Destroy(f);
    }
    GDALClose(ds);
    long long seqN = (long long)seq.size();

    // 并行按 FID 分块读(复刻 AsyncLoader 逻辑)
    int K = 4;
    long long blockSize = (count + K - 1) / K;
    std::vector<std::vector<float>> blocks(K);
    std::vector<std::thread> ths;
    std::atomic<int> remaining{0};
    for (int k = 0; k < K; k++) {
        long long s = k * blockSize, e = (k + 1) * blockSize;
        if (s >= e) continue;
        remaining++;
        ths.emplace_back([&](long long s, long long e, int k) {
            GDALDatasetH d = GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
            OGRLayerH l = GDALDatasetGetLayer(d, 0);
            for (long long fid = s; fid < e; fid++) {
                OGRFeatureH ff = OGR_L_GetFeature(l, (GIntBig)fid);
                if (ff) { addGeometry(OGR_F_GetGeometryRef(ff), blocks[k]); OGR_F_Destroy(ff); }
            }
            GDALClose(d);
            remaining--;
        }, s, e, k);
    }
    for (auto& t : ths) t.join();
    long long parN = 0;
    for (auto& b : blocks) parN += (long long)b.size();

    printf("count=%lld  seqVerts=%lld  parVerts=%lld  K=%d  match=%s\n",
           count, seqN, parN, K, (seqN == parN && seqN > 0) ? "YES" : "NO");
    return (seqN == parN && seqN > 0) ? 0 : 1;
}
