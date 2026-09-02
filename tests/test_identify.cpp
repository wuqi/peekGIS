#include "doctest.h"
#include "data/gdal_datasource.h"
#include "data/attr_table.h"
#include "data/gdal_common.h"
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <filesystem>
#include <system_error>

namespace {
double nowS() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}
}

// 探针: 设置 PEEKGIS_TEST_ATTR=<shp路径> 时, 实测属性表分页读取。
TEST_CASE("attr table probe") {
    const char* p = std::getenv("PEEKGIS_TEST_ATTR");
    if (!p || !*p) return;
    std::string path = p;
    ensureGdal();
    AttrLayerInfo info;
    bool ok = attrOpenLayer(path, 0, info);
    fprintf(stderr, "[attr-probe] open ok=%d name=%s drv=%s fields=%d total=%lld canEncode=%d\n",
            ok, info.layerName.c_str(), info.driverName.c_str(), (int)info.fields.size(),
            info.total, (int)info.canEncode);
    long long cnt = attrFeatureCount(path, 0);
    fprintf(stderr, "[attr-probe] count=%lld\n", cnt);
    if (ok) {
        for (int pg = 0; pg < 2; pg++) {
            AttrPageData pd;
            bool pok = attrFetchPage(path, 0, pg, 5, TextEncoding::Utf8, info, pd);
            fprintf(stderr, "[attr-probe] page=%d ok=%d rows=%d firstFid=%lld\n",
                    pg, pok, (int)pd.rows.size(), (long long)pd.firstFid);
            if (!pd.rows.empty()) {
                auto& row = pd.rows[0];
                fprintf(stderr, "[attr-probe]   row0 fid=%lld cells=%d hasGeom=%d ",
                        (long long)row.fid, (int)row.cells.size(), (int)row.hasGeom);
                for (auto& c : row.cells) fprintf(stderr, "[%s]", c.text.c_str());
                fprintf(stderr, "\n");
            }
        }
    }
}

// 探针: 设置 PEEKGIS_TEST_IDENTIFY=<shp路径> 时, 实测该文件的 identify 分层耗时。
// 无环境变量则直接跳过, 不影响常规测试。
TEST_CASE("identify performance probe") {
    const char* p = std::getenv("PEEKGIS_TEST_IDENTIFY");
    if (!p || !*p) {
        if (getenv("PEEKGIS_TEST_IDENTIFY_ALL")) {
            MESSAGE("SKIP: set PEEKGIS_TEST_IDENTIFY=<path>");
        }
        return;
    }
std::string path = p;
    ensureGdal();

    // 1) 打开耗时
    double t0 = nowS();
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
double tOpen = nowS() - t0;
    if (!ds) {
        fprintf(stderr, "[probe] OPEN FAILED: %s\n", CPLGetLastErrorMsg());
    }
    REQUIRE(ds != nullptr);
    OGRLayerH lyr = GDALDatasetGetLayer(ds, 0);
    OGREnvelope env;
    OGR_L_GetExtent(lyr, &env, TRUE);
    long long nf = OGR_L_GetFeatureCount(lyr, TRUE);
    int epsg = 0;
    OGRSpatialReferenceH srs = OGR_L_GetSpatialRef(lyr);
    if (srs) {
        const char* code = OSRGetAuthorityCode(srs, nullptr);
        if (code) epsg = std::atoi(code);
    }
   fprintf(stderr,"[probe] file=%s  open=%.3fs  extent=(%.1f,%.1f)-(%.1f,%.1f)  features=%lld  epsg=%d\n",
           path.c_str(), tOpen, env.MinX, env.MinY, env.MaxX, env.MaxY, nf, epsg);
    GDALClose(ds);

    double cx = (env.MinX + env.MaxX) / 2.0;
    double cy = (env.MinY + env.MaxY) / 2.0;
    double tol = (env.MaxX - env.MinX) / 4000.0;   // 约 1/2000 跨度

// 2) identifyFeatures 完整耗时; 验证 keeper 复用: 首轮含打开, 之后应 ~O(0.1s)
    {
        double hitX = 0, hitY = 0;
        int foundAt = -1;
        for (int i = 0; i < 9 && foundAt < 0; i++) {
            double fx = env.MinX + (env.MaxX - env.MinX) * (0.25 + 0.25 * (i % 3));
            double fy = env.MinY + (env.MaxY - env.MinY) * (0.25 + 0.25 * (i / 3));
            IdentifyHit h;
            t0 = nowS();
            bool ok = identifyFeatures(path, 0, fx, fy, tol, 0, 0, h);
            if (ok) { hitX = fx; hitY = fy; foundAt = i; }
            fprintf(stderr, "[probe] identify(native) grid=%d ok=%d dt=%.3fs\n", i, ok, nowS() - t0);
        }
        if (foundAt >= 0) {
            double fx = hitX, fy = hitY;
            IdentifyHit h;
            t0 = nowS();
            bool ok = identifyFeatures(path, 0, fx, fy, tol, 0, 0, h);
            fprintf(stderr, "[probe] identify(native) reuse ok=%d dt=%.3fs\n", ok, nowS() - t0);
            if (epsg != 0 && epsg != 4326) {
                IdentifyHit h2;
                t0 = nowS();
                bool ok2 = identifyFeatures(path, 0, fx, fy, tol, 4326, epsg, h2);
                fprintf(stderr, "[probe] identify(4326->%d) ok=%d dt=%.3fs\n",
                        epsg, ok2, nowS() - t0);
            }
        }
    }

// 3.5) shx 只读诊断: 尺寸 vs 要素数; 通过应用归口(gdalOpenVector)打开计时
    {
        std::string shx = path.substr(0, path.size() - 4) + ".shx";
        long long shxBytes = std::filesystem::file_size(shx);
        fprintf(stderr, "[probe] shx=%lld bytes (features=%lld -> need ~%lld)\n", shxBytes,
                nf, nf * 8LL + 100);
        double t1 = nowS();
        GDALDatasetH dsV = gdalOpenVector(path);
        fprintf(stderr, "[probe] open(gdalOpenVector) dt=%.3fs ok=%d\n", nowS() - t1, dsV != nullptr);
        if (dsV) {
            OGRLayerH lV = GDALDatasetGetLayer(dsV, 0);
            long long nV = OGR_L_GetFeatureCount(lV, FALSE);
            fprintf(stderr, "[probe] count(gdalOpenVector)=%lld\n", nV);
            GDALClose(dsV);
        }
    }

    // 3.6) 读取吞吐: 顺序 GetNextFeature vs 随机 GetFeature(fid); 打开走应用归口(gdalOpenVector)
    {
        double t0 = nowS();
        ds = gdalOpenVector(path);
        fprintf(stderr, "[probe] reopen(gdalOpenVector) dt=%.3fs\n", nowS() - t0);
        OGRLayerH l3 = GDALDatasetGetLayer(ds, 0);
        long long lim = std::min<long long>(200000, nf);
        t0 = nowS();
        long long got = 0;
        OGR_L_ResetReading(l3);
        OGRFeatureH f;
        while ((f = OGR_L_GetNextFeature(l3)) != nullptr) {
            got++;
            OGR_F_Destroy(f);
            if (got >= lim) break;
        }
        fprintf(stderr, "[probe] GetNextFeature %lld recs dt=%.3fs (%.0f rec/s)\n",
                got, nowS() - t0, got / (nowS() - t0));
        t0 = nowS();
        long long got2 = 0;
        srand(42);
        for (long long i = 0; i < 20000; i++) {
            GIntBig fid = (GIntBig)(rand() / (double)RAND_MAX * lim);
            f = OGR_L_GetFeature(l3, fid);
            if (f) { got2++; OGR_F_Destroy(f); }
        }
        fprintf(stderr, "[probe] GetFeature(random) %lld recs dt=%.3fs (%.0f rec/s)\n",
                got2, nowS() - t0, got2 / (nowS() - t0));
        f = OGR_L_GetNextFeature(l3);   // 顺序位置已消耗, 仅作 API 收尾
        if (f) OGR_F_Destroy(f);
        GDALClose(ds);
    }

// 4) 手工分层: 过滤段 vs 距离段(打开走 gdalOpenVector, 反映真实应用)
    {
        double t0 = nowS();
        ds = gdalOpenVector(path);
        double tOpen2 = nowS() - t0;
        OGRLayerH l2 = GDALDatasetGetLayer(ds, 0);
        OGR_L_ResetReading(l2);
        OGR_L_SetSpatialFilterRect(l2, cx - tol, cy - tol, cx + tol, cy + tol);
        OGRGeometryH pt = OGR_G_CreateGeometry(wkbPoint);
        OGR_G_SetPoint_2D(pt, 0, cx, cy);
        t0 = nowS();
        long long scanned = 0, dists = 0;
        OGRFeatureH f;
        while ((f = OGR_L_GetNextFeature(l2)) != nullptr) {
            scanned++;
            OGRGeometryH g = OGR_F_GetGeometryRef(f);
            if (g) { OGR_G_Distance(g, pt); dists++; }
            OGR_F_Destroy(f);
        }
        double tScan = nowS() - t0;
       fprintf(stderr,"[probe] manual reopen=%.3fs scan=%.3fs scanned=%lld dists=%lld\n",
               tOpen2, tScan, scanned, dists);
        OGR_G_DestroyGeometry(pt);
        GDALClose(ds);
    }
}
