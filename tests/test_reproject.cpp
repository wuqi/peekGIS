#include "doctest.h"
#include "data/reproject.h"
#include "data/gdal_common.h"
#include <ogr_spatialref.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

TEST_CASE("proj runtime probe: PEEKGIS_TEST_PROJ=1 to dump search paths + TWD97") {
    if (!std::getenv("PEEKGIS_TEST_PROJ")) return;
    ensureGdal();
    fprintf(stderr, "[projprobe] PROJ_LIB env=%s\n",
            ::getenv("PROJ_LIB") ? ::getenv("PROJ_LIB") : "(unset)");

    // TWD97 (EPSG:3826, 台湾) -> WGS84 交叉验证
    std::vector<float> src = {121000.0f, 2500000.0f};   // 假定 3826 台面坐标 (米)
    std::vector<float> dst;
    bool ok = reprojectVertices(src, 3826, 4326, dst);
    fprintf(stderr, "[projprobe] 3826->4326 ok=%d dst=(%.6f, %.6f)\n",
            ok, ok ? (double)dst[0] : 0.0, ok ? (double)dst[1] : 0.0);

    // 直接 import + CT, 打印底层错误
    OGRSpatialReference oSrc, oDst;
    bool iOk = (oSrc.importFromEPSG(3826) == OGRERR_NONE) && (oDst.importFromEPSG(4326) == OGRERR_NONE);
    fprintf(stderr, "[projprobe] import 3826/4326 both_ok=%d\n", iOk);
    if (iOk) {
        oSrc.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        oDst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        OGRCoordinateTransformation* ct = OGRCreateCoordinateTransformation(&oSrc, &oDst);
        fprintf(stderr, "[projprobe] ct=%s\n", ct ? "OK" : "NULL");
        if (ct) {
            double xs[1] = {121000.0}, ys[1] = {2500000.0};
            int r = ct->Transform(1, xs, ys, nullptr);
            fprintf(stderr, "[projprobe] ct transform r=%d -> (%.6f, %.6f)\n", r, xs[0], ys[0]);
            OGRCoordinateTransformation::DestroyCT(ct);
        } else {
            fprintf(stderr, "[projprobe] CT create failed: %s\n", CPLGetLastErrorMsg());
        }
    }
}

TEST_CASE("reproject: 4326 -> 3857 (与 Web Mercator 公式一致)") {
    ensureGdal();
    const double lon = 116.39, lat = 39.90;
    std::vector<float> src = {(float)lon, (float)lat};
    std::vector<float> dst;
    bool ok = reprojectVertices(src, 4326, 3857, dst);
    REQUIRE(ok);
    REQUIRE(dst.size() == 2);

    // 用标准 Web Mercator 公式独立算出期望值做交叉验证
    const double R = 6378137.0;
    double ex = R * (lon * M_PI / 180.0);
    double ey = R * std::log(std::tan(M_PI / 4.0 + (lat * M_PI / 180.0) / 2.0));
    CHECK(dst[0] == doctest::Approx(ex).epsilon(1e-4));
    CHECK(dst[1] == doctest::Approx(ey).epsilon(1e-4));
}

TEST_CASE("reproject: 同 CRS 原样返回") {
    ensureGdal();
    std::vector<float> src = {1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<float> dst;
    bool ok = reprojectVertices(src, 4326, 4326, dst);
    REQUIRE(ok);
    CHECK(dst == src);
}

TEST_CASE("reproject: 空输入安全") {
    std::vector<float> dst;
    CHECK(reprojectVertices({}, 4326, 3857, dst));
    CHECK(dst.empty());
}

TEST_CASE("computeExtent: 正确求包围盒") {
    std::vector<float> v = {0.0f, 0.0f, 10.0f, 2.0f, 5.0f, 9.0f};
    double minx, miny, maxx, maxy;
    computeExtent(v, minx, miny, maxx, maxy);
    CHECK(minx == 0.0); CHECK(miny == 0.0);
    CHECK(maxx == 10.0); CHECK(maxy == 9.0);
}
