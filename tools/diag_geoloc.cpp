#include "data/gdal_common.h"
#include "data/geoloc.h"
#include "data/raster_reader.h"
#include <gdal_alg.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <cmath>

using namespace peekg::data;

#ifdef _WIN32
#include <windows.h>
#endif

static int doTransformProbe(const char* path) {
    ensureGdal();
    GDALDatasetH ds = GDALOpenEx(path, GDAL_OF_RASTER | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) { printf("open fail: %s\n", path); return 1; }
    const char* drv = GDALGetDatasetDriver(ds) ? GDALGetDriverShortName(GDALGetDatasetDriver(ds)) : "?";
    printf("driver=%s size=%dx%d bands=%d\n", drv,
           GDALGetRasterXSize(ds), GDALGetRasterYSize(ds), GDALGetRasterCount(ds));
    char** gl = GDALGetMetadata(ds, "GEOLOCATION");
    printf("GEOLOCATION metadata:\n");
    if (gl) for (char** p = gl; *p; p++) printf("  %s\n", *p);
    else printf("  (none)\n");
    void* tr = (gl && *gl) ? GDALCreateGeoLocTransformer(ds, gl, FALSE) : nullptr;
    printf("transformer=%s\n", tr ? "created" : "NULL");
    int sw = GDALGetRasterXSize(ds), sh = GDALGetRasterYSize(ds);
    GDALClose(ds);
    if (!tr) return 1;
    // 正算: 源像素/行 -> lon/lat
    const int np = 9;
    int pts[np][2] = { {0,0}, {sw-1,0}, {0,sh-1}, {sw-1,sh-1}, {sw/2,sh/2},
                       {0,sh/2}, {sw-1,sh/2}, {sw/2,0}, {sw/2,sh-1} };
    double px[np], py[np], pz[np];
    int okk[np];
    for (int i = 0; i < np; i++) { px[i] = pts[i][0]; py[i] = pts[i][1]; pz[i] = 0; }
    GDALGeoLocTransform(tr, FALSE, np, px, py, pz, okk);
    printf("forward (px,line)->(lon,lat):\n");
    for (int i = 0; i < np; i++)
        printf("  (%4d,%4d) -> (%9.4f,%9.4f) ok=%d\n", pts[i][0], pts[i][1], px[i], py[i], okk[i]);
    // 反算: 目标(lon,lat) -> 源像素/行
    printf("inverse (lon,lat)->(px,line):\n");
    double ilons[] = { -179.5, -120, -60, -30, 0, 30, 60, 120, 179.5 };
    double ilats[] = { 10, 30, 50, 70, 84 };
    for (double la : ilats) {
        for (double lo : ilons) {
            double ix = lo, iy = la, iz = 0; int k = 0;
            GDALGeoLocTransform(tr, TRUE, 1, &ix, &iy, &iz, &k);
            printf("  (%8.2f,%5.2f) -> px=%9.3f line=%9.3f ok=%d%s\n",
                   lo, la, ix, iy, k,
                   (k && ix >= 0 && ix < sw && iy >= 0 && iy < sh) ? "" : "  <-- OUT");
        }
    }
    GDALDestroyGeoLocTransformer(tr);
    return 0;
}

static void dumpHeader(const std::string& path) {
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) { printf("OPEN FAIL: %s\n", path.c_str()); return; }
    printf("size=%dx%d bands=%d\n", GDALGetRasterXSize(ds), GDALGetRasterYSize(ds),
           GDALGetRasterCount(ds));
    double gt[6] = {0,0,0,0,0,0};
    bool hasGeo = (GDALGetGeoTransform(ds, gt) == CE_None);
    if (hasGeo)
        printf("  gt=%f %f %f / %f %f %f\n", gt[0], gt[1], gt[2], gt[3], gt[4], gt[5]);
    const char* wkt = GDALGetProjectionRef(ds);
    if (wkt && wkt[0]) printf("projection: [%.100s...]\n", wkt);
    printf("== driver: %s ==\n",
           GDALGetDatasetDriver(ds) ? GDALGetDriverShortName(GDALGetDatasetDriver(ds)) : "?");
    GDALClose(ds);
}

static int doWarp(const std::string& p, const std::string& var, int time) {
    ensureGdal();
    std::string cache = geolocCachePath(p, var, time);
    printf("warp  %s|%s|%d\n  cache: %s\n", p.c_str(), var.c_str(), time, cache.c_str());
    printf("  buildGeolocationWarp: %s\n", buildGeolocationWarp(p, var, time, cache) ? "OK" : "FAIL");
    if (!netcdfHasGeolocation(p)) return 1;
    printf("\n=== warped cache header ===\n");
    dumpHeader(cache);
    GDALDatasetH ds = GDALOpenEx(cache.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) { printf("OPEN CACHE FAIL\n"); return 1; }
    GDALRasterBandH b = GDALGetRasterBand(ds, 1);
    int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds);
    std::vector<float> buf((size_t)w * h);
    if (GDALRasterIO(b, GF_Read, 0, 0, w, h, (void*)buf.data(), w, h, GDT_Float32, 0, 0) == CE_None) {
        double mn = buf[0], mx = buf[0];
        for (float v : buf) { if (v < mn) mn = v; if (v > mx) mx = v; }
        printf("band1 min=%g max=%g center=%g\n", mn, mx, buf[(size_t)(h / 2) * w + (w / 2)]);
    }
    int hasNd = 0;
    double nd = GDALGetRasterNoDataValue(b, &hasNd);
    printf("nodata: %s\n", hasNd ? std::to_string(nd).c_str() : "(none)");
    GDALClose(ds);
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) { printf("usage: diag_geoloc <file |\n  --list <file> |\n  --warp <file> <var> <timeIdx> |\n  --sample <openSpec> <lon> <lat> [srcEpsg] [dispEpsg] |\n  --transformprobe <file>\n"); return 1; }
    std::string arg1 = argv[1];
    if (arg1 == "--transformprobe") {
        if (argc < 3) { printf("usage: diag_geoloc --transformprobe <file>\n"); return 1; }
        return doTransformProbe(argv[2]);
    }
    if (arg1 == "--sample") {
        // 验证标识识别: 用文件的 geo/尺寸 + 给定(lon,lat)点反算像素并读波段原值
        if (argc < 5) { printf("usage: diag_geoloc --sample <openSpec> <lon> <lat> [srcEpsg] [dispEpsg]\n"); return 1; }
        int srcEpsg = argc >= 6 ? std::atoi(argv[5]) : 4326;
        int dispEpsg = argc >= 7 ? std::atoi(argv[6]) : srcEpsg;
        ensureGdal();
        std::vector<double> geo;
        GDALDatasetH ds = GDALOpenEx(argv[2], GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (!ds) { printf("open fail\n"); return 1; }
        RasterData rd;
        rd.openSpec = argv[2];
        rd.srcEpsg = srcEpsg;
        double gt[6];
        if (GDALGetGeoTransform(ds, gt) != CE_None) { GDALClose(ds); printf("no geotransform\n"); return 1; }
        int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds);
        GDALClose(ds);
        if (readRasterMetadata(argv[2], rd)) printf("metadata: %dx%d bands=%d\n", rd.width, rd.height, rd.bandCount);
        int col = -1, row = -1; double sx = 0, sy = 0; std::vector<double> vals;
        double lon = std::atof(argv[3]), lat = std::atof(argv[4]);
        bool ok = identifyRasterSample(argv[2], gt, w, h, lon, lat, dispEpsg, srcEpsg, col, row, sx, sy, vals);
        printf("srcEpsg=%d dispEpsg=%d\n", srcEpsg, dispEpsg);
        printf("hit=%s col=%d row=%d sx=%.6f sy=%.6f\n", ok ? "yes" : "no", col, row, sx, sy);
        for (size_t i = 0; i < vals.size(); i++)
            printf("  band%d raw = %.8g\n", (int)i + 1, vals[i]);
        return ok ? 0 : 2;
    }
    if (arg1 == "--list") {
        if (argc < 3) { printf("usage: diag_geoloc --list <file>\n"); return 1; }
        std::vector<GeolocVar> gv;
        bool ok = geolocationVars(argv[2], gv);
        printf("=== %s ===\n", argv[2]);
        printf("geolocation vars: %s\n", ok ? std::to_string(gv.size()).c_str() : "(none)");
        for (auto& v : gv) printf("  %-24s timeCount=%d\n", v.var.c_str(), v.timeCount);
        return ok ? 0 : 1;
    }
    if (arg1 == "--warp") {
        if (argc < 5) { printf("usage: diag_geoloc --warp <file> <var> <timeIdx>\n"); return 1; }
        return doWarp(argv[2], argv[3], std::atoi(argv[4]));
    }
    ensureGdal();
    dumpHeader(arg1);
    return 0;
}