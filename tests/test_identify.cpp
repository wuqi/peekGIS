#include "doctest.h"
#include "data/vector_reader.h"
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

using namespace peekg::data;

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

// 探针: 设置 PEEKGIS_TEST_WKT=<shp路径> 时, 实测 featureWkt 回源导出。
// 校验 FID 回源一致性 + WKT 可被 OGR 重新解析(往返), 并打印前若干字符看浮点格式。
TEST_CASE("featureWkt probe") {
    const char* p = std::getenv("PEEKGIS_TEST_WKT");
    if (!p || !*p) return;
    std::string path = p;
    ensureGdal();

    auto kDS = gdalKeeperEnsure(path);
    REQUIRE(kDS != nullptr);
    {
        std::unique_lock<std::mutex> ul(kDS->mu);
        REQUIRE(gdalKeeperWait(kDS, ul));
    }
    OGRLayerH lyr = GDALDatasetGetLayer(kDS->ds, 0);
    REQUIRE(lyr != nullptr);
    long long nf = OGR_L_GetFeatureCount(lyr, TRUE);
    OGREnvelope env;
    OGR_L_GetExtent(lyr, &env, TRUE);
    fprintf(stderr, "[wkt-probe] file=%s features=%lld\n", path.c_str(), nf);

    int nOK = 0, nBad = 0, nEmpty = 0;
    long long totalBytes = 0, maxBytes = 0;
    for (int i = 0; i < 5; i++) {
        long long fid = (nf > 0) ? (long long)((double)nf * (i + 1) / 6.0) : i;
        std::string wkt;
        double t0 = nowS();
        bool ok = featureWkt(path, 0, fid, wkt);
        fprintf(stderr, "[wkt-probe] fid=%lld ok=%d dt=%.4fs bytes=%d head=%.110s\n",
                fid, ok, nowS() - t0, (int)wkt.size(), wkt.c_str());
        if (!ok || wkt.empty()) { nEmpty++; continue; }
        nOK++;
        totalBytes += (long long)wkt.size();
        if ((long long)wkt.size() > maxBytes) maxBytes = (long long)wkt.size();
        // 往返: WKT 必须能被 OGR 重新解析回来
        OGRGeometryH g = nullptr;
        std::string buf = wkt;
        char* q = const_cast<char*>(buf.data());
        if (OGR_G_CreateFromWkt(&q, nullptr, &g) != OGRERR_NONE || !g) { nBad++; continue; }
        fprintf(stderr, "[wkt-probe]   reparse ok type=%s\n", OGR_G_GetGeometryName(g));
        OGR_G_DestroyGeometry(g);
    }
    fprintf(stderr, "[wkt-probe] ok=%d empty=%d badreparse=%d avg=%.0f max=%lld\n",
            nOK, nEmpty, nBad, nOK ? (double)totalBytes / nOK : 0.0, maxBytes);

    // 属性识别面板那条链路: identifyFeatures 必须回填 fid/srcPath/fileLayerIdx, 否则"复制WKT"不出按钮
    {
        double cx = (env.MinX + env.MaxX) / 2.0, cy = (env.MinY + env.MaxY) / 2.0;
        double tol = (env.MaxX - env.MinX) / 2000.0;
        IdentifyHit h;
        bool ok = identifyFeatures(path, 0, cx, cy, tol, 0, 0, h);
        fprintf(stderr, "[wkt-probe] identify ok=%d fid=%lld canFetchGeom=%d layerIdx=%d epsg=%d\n",
                ok, (long long)h.fid, (int)h.canFetchGeom(), h.fileLayerIdx, h.srcEpsg);
        if (ok) {
            CHECK(h.canFetchGeom());
            CHECK(h.srcPath == path);
            std::string wkt;
            CHECK(featureWkt(h.srcPath, h.fileLayerIdx, h.fid, wkt));
            CHECK(!wkt.empty());
            // 与按同一 FID 直接取应完全一致
            std::string direct;
            REQUIRE(featureWkt(path, 0, h.fid, direct));
            CHECK(direct == wkt);
            fprintf(stderr, "[wkt-probe] identify->wkt bytes=%d consistent=%d head=%.60s\n",
                    (int)wkt.size(), (int)(direct == wkt), wkt.c_str());
        }
    }

    // 不存在的 FID / 负 FID 必须返回 false 而不是崩溃
    std::string junk;
    CHECK(featureWkt(path, 0, nf + 999999, junk) == false);
    CHECK(junk.empty());
    CHECK(featureWkt(path, 0, -1, junk) == false);
    CHECK(junk.empty());
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
            if (ok) {
                // 回源定位信息(属性识别面板"复制WKT"依赖它)
                fprintf(stderr, "[probe]   hit fid=%lld canFetchGeom=%d layerIdx=%d srcPath=%s\n",
                        (long long)h.fid, (int)h.canFetchGeom(), h.fileLayerIdx, h.srcPath.c_str());
                std::string wkt;
                double tW = nowS();
                bool wok = featureWkt(h.srcPath, h.fileLayerIdx, h.fid, wkt);
                fprintf(stderr, "[probe]   featureWkt ok=%d dt=%.4fs bytes=%d head=%.80s\n",
                        wok, nowS() - tW, (int)wkt.size(), wkt.c_str());
            }
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
