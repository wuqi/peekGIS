#include "doctest.h"
#include "data/geom_util.h"
#include "data/vector_reader.h"
#include <ogr_api.h>
#include <string>
#include <functional>
#include <cstdlib>
#include <fstream>
#include <filesystem>

using namespace peekg::data;

namespace {
bool threwLengthError = false;
bool scopeDidThrow(const std::function<void()>& f) {
    threwLengthError = false;
    try { f(); }
    catch (const std::exception& e) {
        if (std::string(e.what()).find("long") != std::string::npos) threwLengthError = true;
        return true;
    }
    return false;
}

// 定位测试数据 poly.shp: 环境变量 PEEKGIS_TEST_POLY > 项目相对目录 > 已知候选路径
// (vcpkg buildtrees / HeadFirstGDAL)。返回存在的路径, 找不到返回空串(调用处跳过)。
static std::string locatePolyShp() {
    auto good = [](const std::string& p) { return p.size() && std::ifstream(p).good(); };
    const char* env = std::getenv("PEEKGIS_TEST_POLY");
    if (env && good(env)) return env;
    // 相对工程根 + 逐级向上回溯, 无论从哪个子目录启动都能找到
    namespace ch = std::filesystem;
    auto findUp = [&](const std::string& rel) -> std::string {
        ch::path cur = ch::current_path();
        for (int i = 0; i < 8; ++i) {
            std::string p = (cur / rel).string();
            if (good(p)) return p;
            cur = cur.parent_path();
            if (cur.empty()) break;
        }
        return "";
    };
    for (const char* r : { "tests/testdata/poly.shp", "testdata/poly.shp" }) {
        std::string p = findUp(r);
        if (!p.empty()) return p;
    }
    const char* cands[] = {
        "D:/Github/HeadFirstGDAL/headfirstgdal/src/gdal-3.13.0/autotest/ogr/data/poly.shp",
        "D:/Github/HeadFirstGDAL/headfirstgdal/src/gdal-3.13.0/swig/java/test_data/poly.shp",
        "D:/vcpkg/vcpkg/buildtrees/gdal/src/v3.12.4-ae01cc4ed9.clean/swig/java/test_data/poly.shp",
    };
    for (const char* c : cands)
        if (good(c)) return c;
    return "";
}
}

static std::vector<float> extractFromWkt(const char* wkt) {
    OGRGeometryH g = nullptr;
    std::string buf(wkt);           // OGR 会就地改写缓冲区, 必须用可变副本(不能传字面量)
    char* p = buf.data();
    OGR_G_CreateFromWkt(&p, nullptr, &g);
    std::vector<float> v;
    addGeometry(g, v);
    OGR_G_DestroyGeometry(g);
    return v;
}

TEST_CASE("addGeometry: LineString 段数正确") {
    auto v = extractFromWkt("LINESTRING (0 0, 1 1, 2 2)");
    // 3 点 -> 2 段 -> 每线段 4 float
    CHECK(v.size() == 8);
    CHECK(v[0] == 0.0f); CHECK(v[1] == 0.0f);
    CHECK(v[2] == 1.0f); CHECK(v[3] == 1.0f);
}

TEST_CASE("addGeometry: Polygon 外环+内环") {
    // 外环 5 点(闭合) -> 4 段; 内环 4 点(闭合) -> 3 段; 共 7 段 -> 28 float
    auto v = extractFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0),"
                            "(2 2, 4 2, 4 4, 2 2))");
    CHECK(v.size() == 7 * 4);
}

TEST_CASE("addGeometry: MultiPolygon") {
    auto v = extractFromWkt("MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)),"
                            "((5 5, 6 5, 6 6, 5 5)))");
    // 两个三角形面, 每个 3 段 = 6 段 -> 24 float
    CHECK(v.size() == 24);
}

TEST_CASE("addGeometry: 空/空几何不崩溃") {
    std::vector<float> v;
    addGeometry(nullptr, v);
    CHECK(v.empty());
}

TEST_CASE("addGeometry: Point / MultiPoint 收集到 points") {
    OGRGeometryH g = nullptr;
    std::string buf("POINT (1 2)");
    char* p = buf.data();
    OGR_G_CreateFromWkt(&p, nullptr, &g);
    std::vector<float> v, pts;
    addGeometry(g, v, &pts);
    OGR_G_DestroyGeometry(g);
    CHECK(v.empty());                 // 点不进线段
    CHECK(pts.size() == 2);
    CHECK(pts[0] == 1.0f); CHECK(pts[1] == 2.0f);

    std::string buf2("MULTIPOINT (0 0, 3 4)");
    char* p2 = buf2.data();
    OGRGeometryH g2 = nullptr;
    OGR_G_CreateFromWkt(&p2, nullptr, &g2);
    std::vector<float> v2, pts2;
    addGeometry(g2, v2, &pts2);
    OGR_G_DestroyGeometry(g2);
    CHECK(v2.empty());
    CHECK(pts2.size() == 4);
}

// ---- 半透明面填充(三角剖分) ----
static void filledFromWkt(const char* wkt, std::vector<float>& line, std::vector<float>& tris) {
    OGRGeometryH g = nullptr;
    std::string buf(wkt);           // OGR 会就地改写, 必须用可变副本
    char* p = buf.data();
    OGR_G_CreateFromWkt(&p, nullptr, &g);
    addFilledGeometry(g, line, tris);
    OGR_G_DestroyGeometry(g);
}

TEST_CASE("addFilledGeometry: 方形外环 -> 2 填充三角形, 描边仍在") {
    std::vector<float> line, tris;
    filledFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", line, tris);
    // 外环 4 段描边 -> 16 float; 方形三角化 = 2 三角形 -> 6 float
    CHECK(line.size() == 16);
    // 方形三角化 = 2 三角形 -> 6 顶点 -> 12 float
    CHECK(tris.size() == 12);
    for (float f : tris) {
        CHECK(f >= 0.0f);
        CHECK(f <= 10.0f);   // 三角形顶点来自外环顶点集
    }
}

TEST_CASE("addFilledGeometry: 带洞面 -> 描边含洞, 三角数多于无洞") {
    std::vector<float> line, tris;
    filledFromWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 4 2, 4 4, 2 2))", line, tris);
    // 外环 4 段 + 内环 3 段 = 7 段 -> 28 float
    CHECK(line.size() == 28);
    // 桥接洞需要更多三角形(无洞时 2 个=6 float)
    CHECK(tris.size() > 6);
    CHECK(tris.size() % 6 == 0);          // 一定是完整三角形
}

TEST_CASE("addFilledGeometry: 线状无填充, 点走 points") {
    std::vector<float> line, tris, pts;
    OGRGeometryH g = nullptr;
    std::string buf("LINESTRING (0 0, 1 1, 2 2)");
    char* p = buf.data();
    OGR_G_CreateFromWkt(&p, nullptr, &g);
    addFilledGeometry(g, line, tris, &pts);
    OGR_G_DestroyGeometry(g);
    CHECK(line.size() == 8);
    CHECK(tris.empty());                  // 线不产生填充
    CHECK(pts.empty());
}

TEST_CASE("addFilledGeometry: real poly.shp 全部要素不抛异常") {
    std::string src = locatePolyShp();
    if (src.empty()) { MESSAGE("跳过: 未找到 poly.shp (设 PEEKGIS_TEST_POLY)"); return; }
    OGRRegisterAll();
    OGRSFDriverH drv = OGRGetDriverByName("ESRI Shapefile");
    REQUIRE(drv != nullptr);
    OGRDataSourceH ds = OGR_Dr_Open(drv, src.c_str(), 0);
    REQUIRE(ds != nullptr);
    OGRLayerH lyr = OGR_DS_GetLayer(ds, 0);
    REQUIRE(lyr != nullptr);
    OGR_L_ResetReading(lyr);
    OGRFeatureH f;
    int n = 0;
    while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        OGRGeometryH g = OGR_F_GetGeometryRef(f);
        REQUIRE(g != nullptr);
        std::vector<float> line, tris;
        bool threw = scopeDidThrow([&]() { addFilledGeometry(g, line, tris); });
        OGR_F_Destroy(f);
        CHECK_FALSE(threw);
        if (threw) { OGR_DS_Destroy(ds); return; }
        n++;
    }
    CHECK(n > 0);
    OGR_DS_Destroy(ds);
}

TEST_CASE("identifyFeatures: poly.shp 面内点命中并取到属性") {
    std::string src = locatePolyShp();
    if (src.empty()) { MESSAGE("跳过: 未找到 poly.shp (设 PEEKGIS_TEST_POLY)"); return; }
    OGRRegisterAll();
    OGRSFDriverH drv = OGRGetDriverByName("ESRI Shapefile");
    REQUIRE(drv != nullptr);
    OGRDataSourceH ds = OGR_Dr_Open(drv, src.c_str(), 0);
    REQUIRE(ds != nullptr);
    OGRLayerH lyr = OGR_DS_GetLayer(ds, 0);
    REQUIRE(lyr != nullptr);
    OGR_L_ResetReading(lyr);
    OGRFeatureH f0 = OGR_L_GetNextFeature(lyr);
    REQUIRE(f0 != nullptr);
    OGRGeometryH g = OGR_F_GetGeometryRef(f0);
    REQUIRE(g != nullptr);
    OGRGeometryH ip = OGR_G_PointOnSurface(g);
    OGR_F_Destroy(f0);
    if (ip) {
        double px = OGR_G_GetX(ip, 0), py = OGR_G_GetY(ip, 0);
        OGR_G_DestroyGeometry(ip);
        OGR_DS_Destroy(ds);
        IdentifyHit hit;
        bool ok = identifyFeatures(src.c_str(), 0, px, py, 0.01, 4326, 4326, hit);
        CHECK(ok);
        CHECK(!hit.layerName.empty());
        CHECK(!hit.attrs.empty());              // 至少一个字段
        CHECK(hit.srcEpsg == 4326);
        if (hit.geomType.find("POLYGON") != std::string::npos) {
            CHECK(!hit.outline.empty());        // 描边线段
            CHECK(!hit.fillTris.empty());       // 面填充三角形
            CHECK(hit.fillTris.size() % 6 == 0);
            CHECK((int)hit.outline.size() % 4 == 0);
        }
    } else {
        OGR_DS_Destroy(ds);
        CHECK(false);
    }
}

TEST_CASE("identifyFeatures: 外部远点不命中") {
    std::string src = locatePolyShp();
    if (src.empty()) { MESSAGE("跳过: 未找到 poly.shp (设 PEEKGIS_TEST_POLY)"); return; }
    IdentifyHit hit;
    bool ok = identifyFeatures(src.c_str(), 0, 99999.9, -99999.9, 0.001, 4326, 4326, hit);
    CHECK_FALSE(ok);
}

TEST_CASE("loadWktToVectorData: Point") {
    VectorData vd;
    bool ok = loadWktToVectorData("abc", "POINT (1.5 2.5)", "EPSG:4326", 4326, vd);
    CHECK(ok);
    CHECK(vd.name == "abc");
    CHECK(vd.srcEpsg == 4326);
    CHECK(vd.points.size() == 2);
    CHECK(vd.points[0] == 1.5f);
    CHECK(vd.points[1] == 2.5f);
    CHECK(vd.vertices.empty());
    CHECK(vd.triangles.empty());
    CHECK(vd.minx <= 1.5f);
    CHECK(vd.maxx >= 1.5f);
    CHECK(vd.miny <= 2.5f);
    CHECK(vd.maxy >= 2.5f);
}

TEST_CASE("loadWktToVectorData: Polygon 描边+填充") {
    VectorData vd;
    bool ok = loadWktToVectorData("xyz", "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "EPSG:3857", 3857, vd);
    CHECK(ok);
    CHECK(vd.name == "xyz");
    CHECK(vd.srcEpsg == 3857);
    CHECK(!vd.vertices.empty());   // 外环描边
    CHECK(!vd.triangles.empty());  // 2 个三角形填充
    CHECK(vd.triangles.size() % 6 == 0);
    CHECK(vd.points.empty());
    CHECK(vd.minx == 0.0f);
    CHECK(vd.maxy == 10.0f);
}

TEST_CASE("loadWktToVectorData: 空/非法 WKT 失败") {
    VectorData vd;
    CHECK_FALSE(loadWktToVectorData("bad", "", "EPSG:4326", 4326, vd));
    CHECK_FALSE(loadWktToVectorData("bad", "THIS IS NOT WKT ((", "EPSG:4326", 4326, vd));
    CHECK_FALSE(loadWktToVectorData("bad", "GEOMETRYCOLLECTION EMPTY", "EPSG:4326", 4326, vd));
}
