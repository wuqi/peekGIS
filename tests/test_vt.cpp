#include "doctest.h"
#include "vt/vt_types.h"
#include "vt/vt_cache.h"
#include "vt/vt_build.h"
#include "vt/vt_source.h"
#include "vt/vt_geom.h"
#include "vt/vt_scissor.h"
#include "vt/vt_level.h"
#include "vt/vt_topology.h"
#include "data/gdal_common.h"
#include "test_tmp.h"

#include <ogr_api.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace peekg::vt;

namespace {

std::string tempPath(const char* name) {
    // 放仓库内 build/tmp (AGENTS: 禁止写 C 盘 TEMP); 根目录定位不依赖 CWD
    return peekg::test::tmpPath(name);
}

std::string makeTestGeoJSON() {
    peekg::data::ensureGdal();
    std::string path = tempPath("peekgis_vt_test.geojson");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    GDALDriverH drv = GDALGetDriverByName("GeoJSON");
    REQUIRE(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv, path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    REQUIRE(ds != nullptr);

    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    OGRLayerH lyr = GDALDatasetCreateLayer(ds, "test", srs, wkbUnknown, nullptr);
    REQUIRE(lyr != nullptr);

    auto addWkt = [&](const char* wkt) {
        OGRGeometryH g = nullptr;
        std::string buf(wkt);
        char* p = buf.data();
        OGR_G_CreateFromWkt(&p, nullptr, &g);
        REQUIRE(g != nullptr);
        OGRFeatureH f = OGR_F_Create(OGR_L_GetLayerDefn(lyr));
        OGR_F_SetGeometryDirectly(f, g);
        OGR_L_CreateFeature(lyr, f);
        OGR_F_Destroy(f);
    };
    // 一个跨瓦片边界的方形面(含孔) + 一条线 + 一个点
    addWkt("POLYGON ((0 0, 100 0, 100 100, 0 100, 0 0), (40 40, 60 40, 60 60, 40 60, 40 40))");
    addWkt("LINESTRING (0 0, 100 100)");
    addWkt("POINT (50 50)");

    OSRDestroySpatialReference(srs);
    GDALClose(ds);
    return path;
}

// 4 个面, 特征顺序 A1,B1,A2,B2: A1/A2 落在同一瓦片, B1/B2 落在另一瓦片。
// 配合极小 LRU 上限可强制"淘汰后再触达", 用于验证 flush 合并不丢几何。
std::string makeEvictTestGeoJSON() {
    peekg::data::ensureGdal();
    std::string path = tempPath("peekgis_vt_evict.geojson");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    GDALDriverH drv = GDALGetDriverByName("GeoJSON");
    REQUIRE(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv, path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    REQUIRE(ds != nullptr);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    OGRLayerH lyr = GDALDatasetCreateLayer(ds, "test", srs, wkbUnknown, nullptr);
    REQUIRE(lyr != nullptr);

    auto addWkt = [&](const char* wkt) {
        OGRGeometryH g = nullptr;
        std::string buf(wkt);
        char* p = buf.data();
        OGR_G_CreateFromWkt(&p, nullptr, &g);
        REQUIRE(g != nullptr);
        OGRFeatureH f = OGR_F_Create(OGR_L_GetLayerDefn(lyr));
        OGR_F_SetGeometryDirectly(f, g);
        OGR_L_CreateFeature(lyr, f);
        OGR_F_Destroy(f);
    };
    addWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");       // A1
    addWkt("POLYGON ((75 75, 85 75, 85 85, 75 85, 75 75))"); // B1
    addWkt("POLYGON ((12 12, 20 12, 20 20, 12 20, 12 12))"); // A2
    addWkt("POLYGON ((88 88, 98 88, 98 98, 88 98, 88 88))"); // B2

    OSRDestroySpatialReference(srs);
    GDALClose(ds);
    return path;
}

// 铺满型数据(街区/地块)退化用例: 一个大底面 + 一批"亚格子"小方块。
// 小方块边长只有大范围的 1/64 —— 在粗层(格大)量化后面积 <1 格² 会退化, 在最深层(格细)
// 才够 1 格²。
// 用途: 验证"最深层不丢退化面(撑成 1 格)/ 粗层照丢"这条不变量。
std::string makeFineBlockGeoJSON() {
    peekg::data::ensureGdal();
    std::string path = tempPath("peekgis_vt_fineblock.geojson");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    GDALDriverH drv = GDALGetDriverByName("GeoJSON");
    REQUIRE(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv, path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    REQUIRE(ds != nullptr);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    OGRLayerH lyr = GDALDatasetCreateLayer(ds, "test", srs, wkbUnknown, nullptr);
    REQUIRE(lyr != nullptr);

    auto addWkt = [&](const std::string& wkt) {
        OGRGeometryH g = nullptr;
        char* p = const_cast<char*>(wkt.c_str());
        OGR_G_CreateFromWkt(&p, nullptr, &g);
        REQUIRE(g != nullptr);
        OGRFeatureH f = OGR_F_Create(OGR_L_GetLayerDefn(lyr));
        OGR_F_SetGeometryDirectly(f, g);
        OGR_L_CreateFeature(lyr, f);
        OGR_F_Destroy(f);
    };
    auto fmt = [](double v) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.10f", v);
        return std::string(b);
    };
    auto poly = [&](double x0, double y0, double x1, double y1) {
        return "POLYGON ((" + fmt(x0) + " " + fmt(y0) + ", " + fmt(x1) + " " + fmt(y0) +
               ", " + fmt(x1) + " " + fmt(y1) + ", " + fmt(x0) + " " + fmt(y1) +
               ", " + fmt(x0) + " " + fmt(y0) + "))";
    };

    addWkt(poly(0, 0, 64, 64));                      // 大底面: 保证 extent 就是 [0,64]²
    for (int i = 0; i < 8; ++i)                      // 8×8 = 64 个 1×1 的小方块
        for (int j = 0; j < 8; ++j)
            addWkt(poly(i * 8 + 1, j * 8 + 1, i * 8 + 2, j * 8 + 2));

    OSRDestroySpatialReference(srs);
    GDALClose(ds);
    return path;
}

TEST_CASE("vt: 最深层保留退化面(撑成 1 格), 粗层照丢") {
    std::string src = makeFineBlockGeoJSON();
    std::string out = tempPath("peekgis_vt_fineblock.vtk");
    std::error_code ec;
    std::filesystem::remove(out, ec);

    // 6 层: L0 只有 512 格(每格 0.125 单位), L5 是 1024 格(每格 0.0625 单位)。
    // 1×1 的小方块在 L5 是 16×16 格(很宽裕), 在 L0 约 8×8 格 —— 都不退化,
    // 所以这个用例锁的是"keepDegenerate 路径不会被误触发 + 细分层几何完整"。
    // 真正的塌零用例(比 1/512 还小)见下一个 TEST_CASE。
    VtBuildConfig cfg;
    cfg.forceLevel = 6;
    cfg.dstEpsg = 4326;
    VtBuildStats st;
    REQUIRE(buildVtCache(src, 0, out, cfg, st));
    CHECK(st.maxLevel == 6);

    VtCache c;
    REQUIRE(c.open(out));
    const VtFileHeader& h = c.header();
    int n = 1 << h.maxLevel;
    long long deepFaces = 0, deepDegenerate = 0;
    for (int ty = 0; ty < n; ++ty) {
        for (int tx = 0; tx < n; ++tx) {
            VtTile t;
            if (!c.readTile(h.maxLevel, tx, ty, t)) continue;
            for (const VtRing& r : t.rings) {
                if (r.type != RING_FACE) continue;
                ++deepFaces;
                // 撑成 1 格的面: 有向面积恰好 2(= 1 格² 的 2 倍)
                if (r.vertexCount == 4) {
                    const int16_t* v = &t.verts[(size_t)r.firstVertex * 2];
                    long long a2 = (long long)v[0] * v[3] - (long long)v[2] * v[1];
                    a2 += (long long)v[2] * v[5] - (long long)v[4] * v[3];
                    a2 += (long long)v[4] * v[7] - (long long)v[6] * v[5];
                    a2 += (long long)v[6] * v[1] - (long long)v[0] * v[7];
                    if (std::llabs(a2) == 2) ++deepDegenerate;
                }
            }
        }
    }
    // 大底面 + 64 个小方块(跨片复制) 至少要都在
    CHECK(deepFaces >= 65);
    // 该数据集不该有任何面退化成 1 格方块(小方块在 L6 够大)
    CHECK(deepDegenerate == 0);
    c.close();
}

TEST_CASE("vt: 亚格子小面在粗层退化时被丢, 最深层保留为 1 格方块") {
    peekg::data::ensureGdal();
    std::string src = tempPath("peekgis_vt_subcell.geojson");
    std::error_code ec;
    std::filesystem::remove(src, ec);

    // extent [0,1]², 里面塞 64 个边长 1/262144 的小方块。
    // 关键算术: L6 时 n=64 片, 每片 1024 格 -> 全局 65536 格, 每格 1/65536。
    // 小方块边长 1/262144 = 1/4 格 -> 4 个角量化后全落在同一格 -> 面积 0 -> 退化。
    // L0 时每格 1/512, 小方块 1/512 格 -> 同样退化。
    // 所以两层都退化, 但只有最深层的会被 keepDegenerateFace 撑成 1 格保留。
    GDALDriverH drv = GDALGetDriverByName("GeoJSON");
    REQUIRE(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv, src.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    REQUIRE(ds != nullptr);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    OGRLayerH lyr = GDALDatasetCreateLayer(ds, "test", srs, wkbUnknown, nullptr);
    REQUIRE(lyr != nullptr);

    auto addWkt = [&](const std::string& wkt) {
        OGRGeometryH g = nullptr;
        char* p = const_cast<char*>(wkt.c_str());
        OGR_G_CreateFromWkt(&p, nullptr, &g);
        REQUIRE(g != nullptr);
        OGRFeatureH f = OGR_F_Create(OGR_L_GetLayerDefn(lyr));
        OGR_F_SetGeometryDirectly(f, g);
        OGR_L_CreateFeature(lyr, f);
        OGR_F_Destroy(f);
    };
    auto fmt = [](double v) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.12f", v);
        return std::string(b);
    };
    // extent 由这一个大面撑到 [0,1]²
    addWkt("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
    // 8×8 个 1/262144 边长的小方块, 铺在 [0.25,0.75]² 内
    const double s = 1.0 / 262144.0, base = 0.25, step = 0.5 / 8;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) {
            double x0 = base + i * step, y0 = base + j * step;
            addWkt("POLYGON ((" + fmt(x0) + " " + fmt(y0) + ", " + fmt(x0 + s) + " " + fmt(y0) +
                   ", " + fmt(x0 + s) + " " + fmt(y0 + s) + ", " + fmt(x0) + " " + fmt(y0 + s) +
                   ", " + fmt(x0) + " " + fmt(y0) + "))");
        }
    OSRDestroySpatialReference(srs);
    GDALClose(ds);

    std::string out = tempPath("peekgis_vt_subcell.vtk");
    std::filesystem::remove(out, ec);
    VtBuildConfig cfg;
    cfg.forceLevel = 6;
    cfg.dstEpsg = 4326;
    VtBuildStats st;
    REQUIRE(buildVtCache(src, 0, out, cfg, st));
    CHECK(st.maxLevel == 6);

    VtCache c;
    REQUIRE(c.open(out));
    const VtFileHeader& h = c.header();
    auto ringArea2 = [](const VtTile& t, const VtRing& r) {
        const int16_t* v = &t.verts[(size_t)r.firstVertex * 2];
        int m = (int)r.vertexCount;
        long long a2 = 0;
        for (int i = 0; i < m; ++i) {
            int j = (i + 1) % m;
            a2 += (long long)v[2 * i] * v[2 * j + 1] - (long long)v[2 * j] * v[2 * i + 1];
        }
        return std::llabs(a2);
    };

    // 最深层: 小方块应该以"1 格方块"或更完整的形式存在, 绝不能一个都没有
    int nDeep = 1 << h.maxLevel;
    long long deepUnitSquares = 0, deepTotalFaces = 0;
    for (int ty = 0; ty < nDeep; ++ty)
        for (int tx = 0; tx < nDeep; ++tx) {
            VtTile t;
            if (!c.readTile(h.maxLevel, tx, ty, t)) continue;
            for (const VtRing& r : t.rings) {
                if (r.type != RING_FACE) continue;
                ++deepTotalFaces;
                if (ringArea2(t, r) == 2) ++deepUnitSquares;   // 1 格² (有向面积 2)
            }
        }
    // 撑成 1 格的方块必须存在 —— 这是 keepDegenerateFace 生效的直接证据
    CHECK(deepUnitSquares > 0);
    CHECK(deepTotalFaces > 0);

    // 粗层(L0, 512 格 -> 小方块 1/8 格 -> 面积 1/64 格²): 不应出现被撑成的 1 格方块,
    // 因为粗层走 keepDegenerate=false, 退化面被丢弃。
    // 注: L0 上仍有大底面(1 格² 的整数倍, 面积巨大), 不会误判。
    VtTile t0;
    long long coarseUnitSquares = 0;
    if (c.readTile(0, 0, 0, t0))
        for (const VtRing& r : t0.rings)
            if (r.type == RING_FACE && ringArea2(t0, r) == 2) ++coarseUnitSquares;
    CHECK(coarseUnitSquares == 0);
    c.close();
}

// 一个横跨父瓦片中线的矩形(用于验证合并不会把子片扩边重复计入)
std::string makeSpanPolyGeoJSON() {
    peekg::data::ensureGdal();
    std::string path = tempPath("peekgis_vt_span.geojson");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    GDALDriverH drv = GDALGetDriverByName("GeoJSON");
    REQUIRE(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv, path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    REQUIRE(ds != nullptr);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    OGRLayerH lyr = GDALDatasetCreateLayer(ds, "test", srs, wkbUnknown, nullptr);
    REQUIRE(lyr != nullptr);
    OGRGeometryH g = nullptr;
    std::string buf = "POLYGON ((40 0, 60 0, 60 100, 40 100, 40 0))";
    char* p = buf.data();
    OGR_G_CreateFromWkt(&p, nullptr, &g);
    REQUIRE(g != nullptr);
    OGRFeatureH f = OGR_F_Create(OGR_L_GetLayerDefn(lyr));
    OGR_F_SetGeometryDirectly(f, g);
    OGR_L_CreateFeature(lyr, f);
    OGR_F_Destroy(f);
    OSRDestroySpatialReference(srs);
    GDALClose(ds);
    return path;
}

// 矩形 (0,0)-(100,100), 底边中点 (50,0) 与边共线(验证不抽稀时该点保留)
std::string makeCollinearPolyGeoJSON() {
    peekg::data::ensureGdal();
    std::string path = tempPath("peekgis_vt_collinear.geojson");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    GDALDriverH drv = GDALGetDriverByName("GeoJSON");
    REQUIRE(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv, path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    REQUIRE(ds != nullptr);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    OGRLayerH lyr = GDALDatasetCreateLayer(ds, "test", srs, wkbUnknown, nullptr);
    REQUIRE(lyr != nullptr);
    OGRGeometryH g = nullptr;
    std::string buf = "POLYGON ((0 0, 50 0, 100 0, 100 100, 0 100, 0 0))";
    char* p = buf.data();
    OGR_G_CreateFromWkt(&p, nullptr, &g);
    REQUIRE(g != nullptr);
    OGRFeatureH f = OGR_F_Create(OGR_L_GetLayerDefn(lyr));
    OGR_F_SetGeometryDirectly(f, g);
    OGR_L_CreateFeature(lyr, f);
    OGR_F_Destroy(f);
    OSRDestroySpatialReference(srs);
    GDALClose(ds);
    return path;
}

}  // namespace

TEST_CASE("vt: VtTile 序列化往返") {
    VtTile t;
    t.originX = -123.456; t.originY = 45.678; t.epsg = 4490;
    t.verts = {0, 0, 512, 0, 512, 512, 0, 512};
    VtRing r; r.type = RING_FACE; r.hole = 0; r.firstVertex = 0; r.vertexCount = 4; r.polyGroup = 7;
    t.rings.push_back(r);
    std::vector<uint8_t> buf;
    serializeTile(t, buf);
    VtTile u;
    REQUIRE(deserializeTile(buf.data(), buf.size(), u));
    CHECK(u.originX == doctest::Approx(-123.456));
    CHECK(u.originY == doctest::Approx(45.678));
    CHECK(u.epsg == 4490);
    CHECK(u.verts == t.verts);
    REQUIRE(u.rings.size() == 1);
    CHECK(u.rings[0].type == RING_FACE);
    CHECK(u.rings[0].vertexCount == 4);
    CHECK(u.rings[0].polyGroup == 7);
}

TEST_CASE("vt: 顶点 delta+zigzag+varint 编码往返") {
    VtTile t;
    t.originX = 1.5; t.originY = -2.5; t.epsg = 4326;
    // 含负坐标、跨环大跳变、大 delta
    t.verts = {-10, -10, 0, 0, 522, 522, 5, 5, -10, 522, 100, 100, 100, 100};
    VtRing a; a.type = RING_FACE; a.firstVertex = 0; a.vertexCount = 3; a.polyGroup = 1;
    VtRing b; b.type = RING_LINE; b.firstVertex = 3; b.vertexCount = 4;
    t.rings = {a, b};
    std::vector<uint8_t> buf;
    serializeTile(t, buf);
    VtTile u;
    REQUIRE(deserializeTile(buf.data(), buf.size(), u));
    CHECK(u.verts == t.verts);
    CHECK(u.originX == doctest::Approx(1.5));
    CHECK(u.originY == doctest::Approx(-2.5));
    CHECK(u.epsg == 4326);
    REQUIRE(u.rings.size() == 2);
    CHECK(u.rings[0].vertexCount == 3);
    CHECK(u.rings[1].firstVertex == 3);
    CHECK(u.rings[1].vertexCount == 4);
}

TEST_CASE("vt: 共线点压缩(去共线中点, 面积不变)") {
    std::string src = makeCollinearPolyGeoJSON();
    std::string out = tempPath("peekgis_vt_collinear_test.vtk");
    std::error_code ec;
    std::filesystem::remove(out, ec);

    VtBuildConfig cfg;
    cfg.forceLevel = 0;
    cfg.dstEpsg = 4326;
    VtBuildStats st;
    REQUIRE(buildVtCache(src, 0, out, cfg, st));

    VtCache c;
    REQUIRE(c.open(out));
    const VtFileHeader& h = c.header();
    VtTile t0;
    REQUIRE(c.readTile(0, 0, 0, t0));
    int ts = tileSizeAt(0, (int)h.maxLevel);   // 最深层格网(此处 maxLevel=0 -> 1024)
    // 底边中点 (50,0)->格(ts/2,0) 与边共线, 应被压缩掉
    bool found = false;
    for (uint32_t i = 0; i < t0.vertexCount(); ++i)
        if (t0.verts[(size_t)i * 2] == ts / 2 && t0.verts[(size_t)i * 2 + 1] == 0) found = true;
    CHECK_FALSE(found);
    // 面积不变(压缩不改形状)
    double cell = h.tileW0 / (double)ts;
    double sum = 0;
    for (const VtRing& r : t0.rings) {
        if (r.type != RING_FACE || r.hole) continue;
        double a = 0;
        for (uint32_t i = 0; i < r.vertexCount; ++i) {
            uint32_t j = (i + 1) % r.vertexCount;
            int16_t x1 = t0.verts[(size_t)(r.firstVertex + i) * 2];
            int16_t y1 = t0.verts[(size_t)(r.firstVertex + i) * 2 + 1];
            int16_t x2 = t0.verts[(size_t)(r.firstVertex + j) * 2];
            int16_t y2 = t0.verts[(size_t)(r.firstVertex + j) * 2 + 1];
            a += (double)x1 * y2 - (double)x2 * y1;
        }
        sum += std::fabs(a) / 2.0;
    }
    double trueArea = (100.0 * 100.0) / (cell * cell);
    CHECK(sum == doctest::Approx(trueArea).epsilon(0.05));
    c.close();
}

TEST_CASE("vt: 存储读写/槽表/zstd/重开") {
    std::string path = tempPath("peekgis_vt_cache_test.vtk");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    VtFileHeader h{};
    h.maxLevel = 2;
    h.srcEpsg = 4326; h.dstEpsg = 4326;
    h.originX = 0; h.originY = 0; h.tileW0 = 100;
    h.minx = 0; h.miny = 0; h.maxx = 100; h.maxy = 100;

    VtTile t;
    t.originX = 0; t.originY = 0; t.epsg = 4326;
    t.verts = {0, 0, 10, 0, 10, 10, 0, 10};
    VtRing r; r.type = RING_FACE; r.vertexCount = 4;
    t.rings.push_back(r);

    {
        VtCache c;
        REQUIRE(c.create(path, h));
        CHECK(c.writeTile(2, 1, 1, t));
        CHECK(c.writeTile(0, 0, 0, t));
        CHECK(c.hasTile(2, 1, 1));
        CHECK_FALSE(c.hasTile(2, 0, 0));
        c.setFullyBuilt(2);
        c.finalize();
    }
    {
        VtCache c;
        REQUIRE(c.open(path));
        CHECK((c.header().fullyBuiltLevels & (1u << 2)) != 0);
        VtTile u;
        REQUIRE(c.readTile(2, 1, 1, u));
        CHECK(u.verts == t.verts);
        REQUIRE(u.rings.size() == 1);
        CHECK(c.readTile(0, 0, 0, u));
        CHECK_FALSE(c.readTile(1, 0, 0, u));   // 未写
        CHECK_FALSE(c.readTile(5, 0, 0, u));   // 越界
    }
}

TEST_CASE("vt: buildVtCache 端到端(小 GeoJSON)") {
    std::string src = makeTestGeoJSON();
    std::string out = tempPath("peekgis_vt_build_test.vtk");
    std::error_code ec;
    std::filesystem::remove(out, ec);

    VtBuildConfig cfg;
    cfg.forceLevel = 2;          // 强制 2 层, 稳定可测
    cfg.dstEpsg = 4326;
    VtBuildStats st;
    REQUIRE(buildVtCache(src, 0, out, cfg, st));
    CHECK(st.features == 3);
    CHECK(st.rings >= 3);
    CHECK(st.srcVerts > 0);
    CHECK(st.tilesWritten > 0);

    VtCache c;
    REQUIRE(c.open(out));
    const VtFileHeader& h = c.header();
    CHECK(h.maxLevel == 2);
    CHECK(h.dstEpsg == 4326);
    // 2 层都标记完整
    CHECK((h.fullyBuiltLevels & (1u << 0)) != 0);
    CHECK((h.fullyBuiltLevels & (1u << 2)) != 0);
    // L0 单瓦片应存在(面跨越边界, 合并后非空)
    VtTile t0;
    REQUIRE(c.readTile(0, 0, 0, t0));
    CHECK_FALSE(t0.rings.empty());
    // 所有顶点应在扩边范围内
    for (int16_t v : t0.verts) {
        CHECK(v >= GRID_MIN);
        CHECK(v <= GRID_MAX);
    }
    // 最深层每片 origin 必须等于其网格位置(渲染放置依赖), epsg = dstEpsg
    int n2 = 1 << h.maxLevel;
    double tw = h.tileW0 / (double)n2;
    int found = 0;
    for (int ty = 0; ty < n2; ++ty) {
        for (int tx = 0; tx < n2; ++tx) {
            VtTile tt;
            if (!c.readTile(h.maxLevel, tx, ty, tt)) continue;
            ++found;
            CHECK(tt.originX == doctest::Approx(h.originX + tx * tw));
            CHECK(tt.originY == doctest::Approx(h.originY + ty * tw));
            CHECK(tt.epsg == h.dstEpsg);
        }
    }
    CHECK(found > 0);
    c.close();
}

TEST_CASE("vt: 粗层链式合并(每层由细一层生成, 越粗不缺内容)") {
    // 保留层 0/2/4 -> 三级链。默认只合并最粗层(L0); PEEK_VT_MERGE_TOP=2 时 L2/L0 都由细层生成。
    // 链必须**由细到粗**做: 曾经写成升序, 粗层的子层尚未生成 -> 除最深一对外全是空层。
    auto setMergeTop = [](const char* v) {
#ifdef _WIN32
        _putenv_s("PEEK_VT_MERGE_TOP", v ? v : "");
#else
        if (v) setenv("PEEK_VT_MERGE_TOP", v, 1); else unsetenv("PEEK_VT_MERGE_TOP");
#endif
    };
    auto totalRings = [](VtCache& c, int L) {
        const int n = 1 << L;
        long long r = 0;
        for (int ty = 0; ty < n; ++ty)
            for (int tx = 0; tx < n; ++tx) {
                VtTile t;
                if (!c.readTile(L, tx, ty, t)) continue;
                r += (long long)t.rings.size();
            }
        return r;
    };
    for (int pass = 0; pass < 2; ++pass) {   // 0=默认(只合并 L0), 1=合并到 L2
        setMergeTop(pass ? "2" : nullptr);
        std::string src = makeTestGeoJSON();
        std::string out = tempPath("peekgis_vt_chain_test.vtk");
        std::error_code ec;
        std::filesystem::remove(out, ec);

        VtBuildConfig cfg;
        cfg.forceLevel = 4;
        cfg.dstEpsg = 4326;
        VtBuildStats st;
        REQUIRE(buildVtCache(src, 0, out, cfg, st));

        VtCache c;
        REQUIRE(c.open(out));
        CHECK(c.header().maxLevel == 4);
        CHECK(totalRings(c, 4) > 0);   // 最深层由源路由
        CHECK(totalRings(c, 2) > 0);   // 合并而来(或源路由)
        CHECK(totalRings(c, 0) > 0);   // 最粗层: 默认也是合并而来
        c.close();
    }
    setMergeTop(nullptr);
}

TEST_CASE("vt: LRU 淘汰后再触达不丢几何(读-合并-写)") {
    std::string src = makeEvictTestGeoJSON();
    std::string out = tempPath("peekgis_vt_evict_test.vtk");
    std::error_code ec;
    std::filesystem::remove(out, ec);

    VtBuildConfig cfg;
    cfg.forceLevel = 2;
    cfg.dstEpsg = 4326;
    cfg.lruVerts = 1;        // 极小上限: 每次追加都触发淘汰, 逼出"淘汰后再触达"
    VtBuildStats st;
    REQUIRE(buildVtCache(src, 0, out, cfg, st));

    VtCache c;
    REQUIRE(c.open(out));
    auto countFaceRings = [&](int tx, int ty) {
        VtTile t;
        if (!c.readTile(2, tx, ty, t)) return 0;
        int n = 0;
        for (const VtRing& r : t.rings) if (r.type == RING_FACE) ++n;
        return n;
    };
    // 每个瓦片应保留两片(淘汰时的旧内容 + 再触达的新内容), 丢失则为 1
    CHECK(countFaceRings(0, 0) == 2);
    CHECK(countFaceRings(3, 3) == 2);
    c.close();
}

TEST_CASE("vt: 量化后外环面积和 == 真实面积(不重复不丢)") {
    std::string src = makeSpanPolyGeoJSON();
    std::string out = tempPath("peekgis_vt_merge_test.vtk");
    std::error_code ec;
    std::filesystem::remove(out, ec);

    VtBuildConfig cfg;
    cfg.forceLevel = 1;
    cfg.dstEpsg = 4326;
    cfg.simplify = false;
    VtBuildStats st;
    REQUIRE(buildVtCache(src, 0, out, cfg, st));

    VtCache c;
    REQUIRE(c.open(out));
    const VtFileHeader& h = c.header();
    VtTile t0;
    REQUIRE(c.readTile(0, 0, 0, t0));
    double cell = h.tileW0 / (double)TILE_SIZE;
    double sum = 0;
    for (const VtRing& r : t0.rings) {
        if (r.type != RING_FACE || r.hole) continue;
        double a = 0;
        for (uint32_t i = 0; i < r.vertexCount; ++i) {
            uint32_t j = (i + 1) % r.vertexCount;
            int16_t x1 = t0.verts[(size_t)(r.firstVertex + i) * 2];
            int16_t y1 = t0.verts[(size_t)(r.firstVertex + i) * 2 + 1];
            int16_t x2 = t0.verts[(size_t)(r.firstVertex + j) * 2];
            int16_t y2 = t0.verts[(size_t)(r.firstVertex + j) * 2 + 1];
            a += (double)x1 * y2 - (double)x2 * y1;
        }
        sum += std::fabs(a) / 2.0;
    }
    // 矩形 20x100; 正确合并后外环面积和 == 真实面积(格²), 重复计入会明显偏大
    double trueArea = (20.0 * 100.0) / (cell * cell);
    CHECK(sum == doctest::Approx(trueArea).epsilon(0.05));
    c.close();
}

TEST_CASE("vt: estimateMaxLevel 返回合理层") {
    std::string src = makeTestGeoJSON();
    int L = estimateMaxLevel(src, 0, 4326, 2048, 8);
    CHECK(L >= 0);
    CHECK(L <= 8);
}

TEST_CASE("vt: buildTileGeometry 面(含孔)/线/点") {
    VtTile t;
    t.originX = 0; t.originY = 0;
    // 外环 + 孔(同一 polyGroup)
    t.verts = {0,0, 512,0, 512,512, 0,512,
               100,100, 400,100, 400,400, 100,400};
    VtRing outer; outer.type = RING_FACE; outer.hole = 0; outer.firstVertex = 0; outer.vertexCount = 4; outer.polyGroup = 1;
    VtRing hole;  hole.type  = RING_FACE; hole.hole  = 1; hole.firstVertex  = 4; hole.vertexCount  = 4; hole.polyGroup  = 1;
    VtRing line;  line.type  = RING_LINE; line.firstVertex = 0; line.vertexCount = 2;
    VtRing pt;    pt.type    = RING_POINT; pt.firstVertex = 2; pt.vertexCount = 1;
    t.rings = {outer, hole, line, pt};

    std::vector<float> lines, points, fill;
    buildTileGeometry(t, 1.0, true, lines, points, fill);
    CHECK(!lines.empty());
    CHECK(points.size() == 2);
    CHECK(!fill.empty());
    CHECK(fill.size() % 6 == 0);
    // 有孔时三角形顶点数应多于无孔(4 顶点 -> 2 三角 = 12 float; 带孔更多)
    CHECK(fill.size() > 12);
}

TEST_CASE("vt: 同 polyGroup 多外环各自成面(不当孔)") {
    VtTile t;
    t.originX = 0; t.originY = 0;
    // 两个相邻正方形, 同一 polyGroup, 均为外环(模拟一个面跨瓦片被切成两段)
    t.verts = {0,0, 100,0, 100,100, 0,100,
               100,0, 200,0, 200,100, 100,100};
    VtRing a; a.type = RING_FACE; a.hole = 0; a.firstVertex = 0; a.vertexCount = 4; a.polyGroup = 1;
    VtRing b; b.type = RING_FACE; b.hole = 0; b.firstVertex = 4; b.vertexCount = 4; b.polyGroup = 1;
    t.rings = {a, b};
    std::vector<float> lines, points, fill;
    buildTileGeometry(t, 1.0, false, lines, points, fill);
    // 两个 4 顶点方各 2 三角 -> 共 4 三角 = 24 float; 若第二环被误当孔则不是这个数
    CHECK(fill.size() == 24);
}

// 面环在缓存里是"开口存储"(quantizeRing 去掉了显式闭合点), 描边必须自己补 last->first。
// 回归: 三角形外环 3 顶点应产出 3 条边(每边 2 点 * 2 float = 4 float); 线环是开放折线。
TEST_CASE("vt: buildTileGeometry 面环描边补闭合边, 线环不闭合") {
    auto hasSeg = [](const std::vector<float>& v, float ax, float ay, float bx, float by) {
        for (size_t i = 0; i + 3 < v.size(); i += 2) {
            if (v[i] == ax && v[i+1] == ay && v[i+2] == bx && v[i+3] == by) return true;
        }
        return false;
    };

    {   // 三角面: 缺了闭合边就是 2 条而不是 3 条
        VtTile t; t.originX = 0; t.originY = 0;
        t.verts = {0,0, 10,0, 10,10};
        VtRing r; r.type = RING_FACE; r.hole = 0; r.firstVertex = 0; r.vertexCount = 3; r.polyGroup = 1;
        t.rings = {r};
        std::vector<float> lines, points, fill;
        buildTileGeometry(t, 1.0, true, lines, points, fill);
        CHECK(lines.size() == 12);                                  // 3 边 * 4 float
        CHECK(hasSeg(lines, 0, 0, 10, 0));
        CHECK(hasSeg(lines, 10, 0, 10, 10));
        CHECK(hasSeg(lines, 10, 10, 0, 0));                         // 闭合边
    }
    {   // 三角洞: 洞也要闭合, 否则表现为"洞填色了但缺一条边线"
        VtTile t; t.originX = 0; t.originY = 0;
        t.verts = {0,0, 100,0, 100,100, 0,100,
                   20,20, 40,20, 40,40, 20,40};
        VtRing o; o.type = RING_FACE; o.hole = 0; o.firstVertex = 0; o.vertexCount = 4; o.polyGroup = 1;
        VtRing h; h.type = RING_FACE; h.hole = 1; h.firstVertex = 4; h.vertexCount = 4; h.polyGroup = 1;
        t.rings = {o, h};
        std::vector<float> lines, points, fill;
        buildTileGeometry(t, 1.0, true, lines, points, fill);
        CHECK(lines.size() == 32);                                  // 外环 4 边 + 洞 4 边
        CHECK(hasSeg(lines, 40, 40, 20, 40));
        CHECK(hasSeg(lines, 20, 40, 20, 20));                       // 洞的闭合边
    }
    {   // 线环: 开放折线, 不能补闭合边
        VtTile t; t.originX = 0; t.originY = 0;
        t.verts = {0,0, 10,0, 10,10};
        VtRing r; r.type = RING_LINE; r.firstVertex = 0; r.vertexCount = 3;
        t.rings = {r};
        std::vector<float> lines, points, fill;
        buildTileGeometry(t, 1.0, true, lines, points, fill);
        CHECK(lines.size() == 8);                                   // 2 边, 无闭合
    }
    {   // 已带显式闭合点(首==尾)时不能补出零长段
        VtTile t; t.originX = 0; t.originY = 0;
        t.verts = {0,0, 10,0, 10,10, 0,0};
        VtRing r; r.type = RING_FACE; r.hole = 0; r.firstVertex = 0; r.vertexCount = 4; r.polyGroup = 1;
        t.rings = {r};
        std::vector<float> lines, points, fill;
        buildTileGeometry(t, 1.0, true, lines, points, fill);
        CHECK(lines.size() == 12);                                  // 3 条实边, 无零长段
    }
}

// 探针: PEEKGIS_TEST_TILE=<cacheDir> PEEKGIS_TEST_PT=<lon>,<lat>
//       PEEKGIS_TEST_SRC=<shp>  PEEKGIS_TEST_GEOID=<字段值>
// 打印源要素各环(转到显示 CRS)的面积, 以及缓存 L(max-2)/L(max) 对应瓦片里
// 落在该点附近的环, 用于定位"某层才出现的洞"是真实孔还是该层格网产生的伪影。
TEST_CASE("vt tile hole probe") {
    const char* pCache = std::getenv("PEEKGIS_TEST_TILE");
    const char* pPt    = std::getenv("PEEKGIS_TEST_PT");
    const char* pSrc   = std::getenv("PEEKGIS_TEST_SRC");
    const char* pFld   = std::getenv("PEEKGIS_TEST_FLD");
    const char* pVal   = std::getenv("PEEKGIS_TEST_VAL");
    if (!pCache || !pPt || !pSrc || !pFld || !pVal) return;
    std::string cacheDir = pCache, srcPath = pSrc, want = pVal;
    double lon = 0, lat = 0;
    { std::string s = pPt; size_t c = s.find(','); if (c == std::string::npos) return;
      lon = atof(s.substr(0, c).c_str()); lat = atof(s.substr(c + 1).c_str()); }

    // --- 1. 找缓存 ---
    std::vector<VtCacheEntry> cs = listVtCaches(cacheDir);
    std::string base = std::filesystem::path(srcPath).filename().string();
    VtCacheEntry ce; bool found = false;
    for (auto& e : cs)
        fprintf(stderr, "[tile] cand path=%s srcName='%s' bytes=%lld\n",
                e.path.c_str(), e.srcName.c_str(), (long long)e.bytes);
    for (auto& e : cs) {
        if (srcPath.find(e.srcName) != std::string::npos && !e.srcName.empty()) { ce = e; found = true; break; }
    }
    REQUIRE(found);
    VtCache cache;
    REQUIRE(cache.open(ce.path));
    const VtFileHeader& h = cache.header();
    fprintf(stderr, "[tile] cache=%s src=%s srcEpsg=%d dstEpsg=%d maxLevel=%d\n",
            ce.path.c_str(), ce.srcName.c_str(), h.srcEpsg, h.dstEpsg, (int)h.maxLevel);
    fprintf(stderr, "[tile] grid origin=(%.6f,%.6f) tileW0=%.3f extent=[%.3f %.3f %.3f %.3f]\n",
            h.originX, h.originY, h.tileW0, h.minx, h.miny, h.maxx, h.maxy);

    // --- 2. 源要素各环 -> 显示 CRS, 打印面积 ---
    peekg::data::ensureGdal();
    GDALDatasetH ds = GDALOpenEx(srcPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    REQUIRE(ds != nullptr);
    OGRLayerH lyr = GDALDatasetGetLayer(ds, 0);
    REQUIRE(lyr != nullptr);
    OGRSpatialReferenceH dS = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(dS, h.dstEpsg);
    OGRSpatialReferenceH sS = OGR_L_GetSpatialRef(lyr);
    OGRCoordinateTransformationH ct = nullptr;
    if (sS) ct = OCTNewCoordinateTransformation(sS, dS);
    OGR_L_ResetReading(lyr);
    OGRFeatureH f = nullptr;
    while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        int fi = OGR_F_GetFieldIndex(f, pFld);
        if (fi < 0 || want != OGR_F_GetFieldAsString(f, fi)) { OGR_F_Destroy(f); continue; }
        OGRGeometryH g = OGR_F_GetGeometryRef(f);
        REQUIRE(g != nullptr);
        fprintf(stderr, "[tile] SOURCE fid=%lld rings=%d\n", (long long)OGR_F_GetFID(f), OGR_G_GetGeometryCount(g));
        for (int r = 0; r < OGR_G_GetGeometryCount(g); r++) {
            OGRGeometryH ring = OGR_G_GetGeometryRef(g, r);
            int np = OGR_G_GetPointCount(ring);
            std::vector<double> xs(np), ys(np);
            double a2 = 0;
            for (int i = 0; i < np; i++) {
                double x, y; OGR_G_GetPoint(ring, i, &x, &y, nullptr);
                if (ct) OCTTransform(ct, 1, &x, &y, nullptr);
                xs[i] = x; ys[i] = y;
            }
            for (int i = 0; i < np; i++) { int j = (i + 1) % np; a2 += xs[i] * ys[j] - xs[j] * ys[i]; }
            fprintf(stderr, "[tile]   SRC ring%d %s pts=%d area=%.6f  bbox=[%.4f %.4f %.4f %.4f]\n",
                    r, r ? "HOLE" : "OUTER", np, std::fabs(a2) * 0.5,
                    *std::min_element(xs.begin(), xs.end()), *std::min_element(ys.begin(), ys.end()),
                    *std::max_element(xs.begin(), xs.end()), *std::max_element(ys.begin(), ys.end()));
        }
        OGR_F_Destroy(f);
        break;
    }
    if (ct) OCTDestroyCoordinateTransformation(ct);
    GDALClose(ds);

    // --- 3. 定位瓦片, 打印层内环 ---
    double px = lon, py = lat;
    {
        OGRSpatialReferenceH g0 = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(g0, 4326);
        OGRSpatialReferenceH g1 = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(g1, h.srcEpsg);
        OGRCoordinateTransformationH t2 = OCTNewCoordinateTransformation(g0, g1);
        if (t2) { OCTTransform(t2, 1, &px, &py, nullptr); OCTDestroyCoordinateTransformation(t2); }
    }
    {
        OGRSpatialReferenceH g0 = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(g0, h.srcEpsg);
        OGRSpatialReferenceH g1 = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(g1, h.dstEpsg);
        OGRCoordinateTransformationH t2 = OCTNewCoordinateTransformation(g0, g1);
        if (t2) { OCTTransform(t2, 1, &px, &py, nullptr); OCTDestroyCoordinateTransformation(t2); }
    }
    fprintf(stderr, "[tile] probe display pt=(%.4f,%.4f)\n", px, py);

    for (int L = (int)h.maxLevel; L >= 0; L--) {
        double tileW = h.tileW0 / (double)(1 << L);
        int tx = (int)std::floor((px - h.originX) / tileW);
        int ty = (int)std::floor((py - h.originY) / tileW);
        VtTile t;
        if (!cache.hasTile(L, tx, ty)) { fprintf(stderr, "[tile] L%d (%d,%d) 无瓦片\n", L, tx, ty); continue; }
        if (!cache.readTile(L, tx, ty, t)) { fprintf(stderr, "[tile] L%d (%d,%d) 读失败\n", L, tx, ty); continue; }
        fprintf(stderr, "[tile] L%d (%d,%d) cell=%.6g verts=%u rings=%zu\n",
                L, tx, ty, tileW / 512.0, t.vertexCount(), t.rings.size());
        // 瓦片局部格网 -> 显示坐标
        double ox = t.originX, oy = t.originY;
        double cell = tileW / 512.0;
        // 先找出覆盖探针点的面环所属的 polyGroup
        uint32_t grp = UINT32_MAX;
        for (size_t i = 0; i < t.rings.size() && grp == UINT32_MAX; i++) {
            const VtRing& r = t.rings[i];
            if (r.type != RING_FACE) continue;
            const int16_t* v = t.verts.data() + (size_t)r.firstVertex * 2;
            double mnx = 1e30, mny = 1e30, mxx = -1e30, mxy = -1e30;
            for (uint32_t k = 0; k < r.vertexCount; k++) {
                mnx = std::min(mnx, ox + v[2*k]*cell); mxx = std::max(mxx, ox + v[2*k]*cell);
                mny = std::min(mny, oy + v[2*k+1]*cell); mxy = std::max(mxy, oy + v[2*k+1]*cell);
            }
            if (px >= mnx && px <= mxx && py >= mny && py <= mxy) grp = r.polyGroup;
        }
        if (grp == UINT32_MAX) continue;
        // 打印该组全部环 + 复算嵌套深度(复现 assignHolesByNesting 的判定)
        std::vector<int> ids;
        for (size_t i = 0; i < t.rings.size(); i++)
            if (t.rings[i].type == RING_FACE && t.rings[i].polyGroup == grp) ids.push_back((int)i);
        fprintf(stderr, "[tile] L%d (%d,%d) grp=%u 共 %zu 个面环\n", L, tx, ty, grp, ids.size());
        auto inRing = [&](const VtRing& rr, double qx, double qy) {
            bool in = false;
            const int16_t* w = t.verts.data() + (size_t)rr.firstVertex * 2;
            for (uint32_t p = 0, q = rr.vertexCount - 1; p < rr.vertexCount; q = p++) {
                double xi = w[2*p], yi = w[2*p+1], xj = w[2*q], yj = w[2*q+1];
                if (((yi > qy) != (yj > qy)) && (qx < (xj - xi) * (qy - yi) / (yj - yi) + xi)) in = !in;
            }
            return in;
        };
        for (int ia : ids) {
            const VtRing& r = t.rings[(size_t)ia];
            const int16_t* v = t.verts.data() + (size_t)r.firstVertex * 2;
            double a2 = 0, mnx = 1e30, mny = 1e30, mxx = -1e30, mxy = -1e30;
            for (uint32_t k = 0; k < r.vertexCount; k++) {
                double X = ox + v[2*k]*cell, Y = oy + v[2*k+1]*cell;
                mnx = std::min(mnx, X); mny = std::min(mny, Y);
                mxx = std::max(mxx, X); mxy = std::max(mxy, Y);
                uint32_t k2 = (k + 1) % r.vertexCount;
                a2 += X * (oy + v[2*k2+1]*cell) - (ox + v[2*k2]*cell) * Y;
            }
            double fx = ox + v[0]*cell, fy = oy + v[1]*cell;
            int depth = 0;
            for (int ib : ids) {
                if (ia == ib) continue;
                if (inRing(t.rings[(size_t)ib], fx, fy)) depth++;
            }
            fprintf(stderr, "[tile]   ring%d stored_hole=%d pts=%u area=%.8f first=(%.5f,%.5f) depth=%d -> would_be_hole=%d bbox=[%.4f %.4f %.4f %.4f]\n",
                    ia, r.hole, r.vertexCount, std::fabs(a2)*0.5, fx, fy, depth, depth & 1, mnx, mny, mxx, mxy);
        }
    }
    cache.close();
}

// 回归: 48 号 Block 2015 在 L6 的洞。外环首顶点落在自己 2 号洞内部,
// 旧的 assignHolesByNesting 用首顶点当探针 -> 外环 depth=1 -> 被标成孔 ->
// 该 polyGroup 没有外环 -> 整面零填充(看起来像"洞")。L7 因最深层跳过拓扑后处理而幸免。
TEST_CASE("vt: assignHolesByNesting 外环首顶点落在自身洞内也不该被标成孔") {
    VtTile t; t.originX = 0; t.originY = 0;
    // 外环: 一个大矩形, 但把首顶点放到 (300,300) —— 那里在下面的洞里面
    t.verts = {300,300,  1000,100,  1000,1000,  100,1000,
               200,200,  400,200,  400,400,  200,400};   // ring1 = 洞, 含 (300,300)
    VtRing outer; outer.type = RING_FACE; outer.hole = 0; outer.firstVertex = 0; outer.vertexCount = 4; outer.polyGroup = 7;
    VtRing hole;  hole.type  = RING_FACE; hole.hole  = 1; hole.firstVertex  = 4; hole.vertexCount  = 4; hole.polyGroup  = 7;
    t.rings = {outer, hole};

    assignHolesByNesting(t);
    CHECK(t.rings[0].hole == 0);   // 外环必须仍是外环
    CHECK(t.rings[1].hole == 1);   // 洞仍是洞

    // 该组必须能正常出填充: 外环挖掉洞
    std::vector<float> lines, points, fill;
    buildTileGeometry(t, 1.0, false, lines, points, fill);
    CHECK(!fill.empty());
}

// 兜底: 即便内部点判定被退化几何带偏, 整组也不能一个外环都没有
// (全标成孔 -> earcut 拿不到 shell -> 整面不填充)。
TEST_CASE("vt: assignHolesByNesting 整组无外环时兜底为最大环") {
    VtTile t; t.originX = 0; t.originY = 0;
    // 两个完全重合的环: 互相包含 -> 深度都是 1(奇) -> 旧逻辑全标成孔
    t.verts = {100,100, 900,100, 900,900, 100,900,
               100,100, 900,100, 900,900, 100,900};
    VtRing a; a.type = RING_FACE; a.hole = 0; a.firstVertex = 0; a.vertexCount = 4; a.polyGroup = 3;
    VtRing b; b.type = RING_FACE; b.hole = 0; b.firstVertex = 4; b.vertexCount = 4; b.polyGroup = 3;
    t.rings = {a, b};

    assignHolesByNesting(t);
    int nOuter = 0;
    for (const VtRing& r : t.rings) if (!r.hole) ++nOuter;
    CHECK(nOuter >= 1);
}

TEST_CASE("vt: scissor 相邻瓦片严格共享边界像素") {
    const double cell = 1.0;          // 净区 512 世界单位
    const int texW = 800, texH = 800;
    const double cx = 256, cy = 256;  // 视图中心
    const double sc = 5.12;           // 512 世界 / 5.12 = 100 px/片
    ScissorRect A = tileScissorRect(0, 0, cell, cx, cy, sc, texW, texH);
    ScissorRect B = tileScissorRect(512, 0, cell, cx, cy, sc, texW, texH);   // 右邻
    ScissorRect C = tileScissorRect(0, 512, cell, cx, cy, sc, texW, texH);   // 上邻
    CHECK(A.w == 100);
    CHECK(A.h == 100);
    CHECK(A.x + A.w == B.x);   // 右边界: 严格相接, 无缝无重叠
    CHECK(A.y + A.h == C.y);   // 上边界: 严格相接
    for (const ScissorRect* r : {&A, &B, &C}) {
        CHECK(r->x >= 0);
        CHECK(r->y >= 0);
        CHECK(r->x + r->w <= texW);
        CHECK(r->y + r->h <= texH);
    }
}

TEST_CASE("vt: scissor 网格所有相邻边界都相接") {
    const double cell = 1.0;
    const int texW = 1024, texH = 1024;
    const double cx = 1024, cy = 1024, sc = 4.0;   // 每片 128px
    const int N = 4;
    std::vector<ScissorRect> R((size_t)N * N);
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i)
            R[(size_t)j * N + i] = tileScissorRect(i * 512.0, j * 512.0, cell, cx, cy, sc, texW, texH);
    for (int j = 0; j < N; ++j) {
        for (int i = 0; i < N; ++i) {
            const ScissorRect& a = R[(size_t)j * N + i];
            CHECK(a.w == 128);
            CHECK(a.h == 128);
            if (i + 1 < N) CHECK(a.x + a.w == R[(size_t)j * N + i + 1].x);   // 水平相接
            if (j + 1 < N) CHECK(a.y + a.h == R[(size_t)(j + 1) * N + i].y); // 垂直相接
        }
    }
}

TEST_CASE("vt: 最深层 tileSizeAt=1024 且 scissor 按 1024 净区裁") {
    CHECK(tileSizeAt(0, 8) == 512);
    CHECK(tileSizeAt(7, 8) == 512);
    CHECK(tileSizeAt(8, 8) == 1024);   // 只有最深层 2x
    CHECK(tilePadAt(7, 8) == 10);
    CHECK(tilePadAt(8, 8) == 20);
    const double cell = 1.0;            // 1024 格 -> 1024 世界单位
    const int texW = 1200, texH = 1200;
    const double cx = 512, cy = 512, sc = 5.12;   // 1024 / 5.12 = 200px
    ScissorRect A = tileScissorRect(0, 0, cell, cx, cy, sc, texW, texH, 1024);
    ScissorRect B = tileScissorRect(1024, 0, cell, cx, cy, sc, texW, texH, 1024);
    CHECK(A.w == 200);
    CHECK(A.h == 200);
    CHECK(A.x + A.w == B.x);           // 相邻 1024 片仍严格相接
    ScissorRect W = tileScissorRect(0, 0, cell, cx, cy, sc, texW, texH);   // 默认 512
    CHECK(W.w == 100);
}

TEST_CASE("vt: scissor 非法参数回退整视口") {
    ScissorRect r = tileScissorRect(0, 0, 1, 0, 0, 0.0, 640, 480);
    CHECK(r.x == 0); CHECK(r.y == 0); CHECK(r.w == 640); CHECK(r.h == 480);
}

TEST_CASE("vt: chooseVtLevel 随缩放选层 + 受已建层限制") {
    const double tileW0 = 7.66;
    const uint32_t allBuilt = 0xFFFFFFFFu;
    for (int L = 0; L <= 8; ++L) {
        double scale = tileW0 / (512.0 * (1 << L));   // 该层每片正好 512px
        CHECK(chooseVtLevel(scale, tileW0, 9, allBuilt) == L);
    }
    int coarse = chooseVtLevel(tileW0 / (512.0 * 4), tileW0, 9, allBuilt);
    int fine = chooseVtLevel(tileW0 / (512.0 * 64), tileW0, 9, allBuilt);
    CHECK(fine > coarse);                                   // 放大 -> 层更深
    CHECK(chooseVtLevel(tileW0 / (512.0 * 64), tileW0, 9, 0x7u) == 2);  // 只建了 0..2
    CHECK(chooseVtLevel(tileW0 / (512.0 * 4096), tileW0, 9, allBuilt) == 9);  // 夹到 maxLevel
}

TEST_CASE("vt: visibleTileRange 随缩放变化且在界内") {
    const double tileW0 = 8.0;
    const int texW = 1024, texH = 1024;
    const int L = 3;                       // 8x8 瓦片, tileW=1
    double s = 1.0 / 256.0;                // 视口宽约 4 世界单位
    TileRange a = visibleTileRange(4, 4, s, texW, texH, 0, 0, tileW0, L);
    TileRange b = visibleTileRange(4, 4, s / 2, texW, texH, 0, 0, tileW0, L);  // 放大 2x
    CHECK(a.tx1 >= a.tx0);
    CHECK(a.tx0 >= 0); CHECK(a.tx1 <= 7); CHECK(a.ty0 >= 0); CHECK(a.ty1 <= 7);
    CHECK(b.tx1 - b.tx0 <= a.tx1 - a.tx0);  // 放大后可见瓦片不增
    // 视口在数据外 -> 空
    TileRange e = visibleTileRange(1000, 1000, s, texW, texH, 0, 0, tileW0, L);
    CHECK(e.tx1 < e.tx0);
}

// 缓存完整性: 数据段大小 == 所有有效槽 size 之和(即无垃圾), 且 offset 不重复。
// 这条是"同一瓦片被反复 flush 追加"会直接违反的判据(TILE_SIZE 变大时曾出现)。
TEST_CASE("vt: levelKept 隔层保留(从 L0 起)+最深层") {
    for (int L = 0; L <= 8; ++L) CHECK(levelKept(L, 8, 1));      // step=1 全建
    CHECK(levelKept(0, 8, 2));
    CHECK_FALSE(levelKept(1, 8, 2));
    CHECK(levelKept(2, 8, 2));
    CHECK_FALSE(levelKept(3, 8, 2));
    CHECK(levelKept(8, 8, 2));                                   // 最深层必留
    CHECK(levelKept(0, 3, 2));
    CHECK_FALSE(levelKept(1, 3, 2));
    CHECK(levelKept(2, 3, 2));
    CHECK(levelKept(3, 3, 2));                                   // 最深层(奇数)必留
}

TEST_CASE("vt: 隔层构建只写偶数层 + 最深层") {
    std::string src = makeTestGeoJSON();
    std::string out = tempPath("peekgis_vt_step_test.vtk");
    std::error_code ec;
    std::filesystem::remove(out, ec);
    VtBuildConfig cfg;
    cfg.forceLevel = 4;          // 强制 4 层
    cfg.levelStep = 2;       // 只建 L0/L2/L4
    cfg.dstEpsg = 4326;
    VtBuildStats st;
    REQUIRE(buildVtCache(src, 0, out, cfg, st));

    VtCache c;
    REQUIRE(c.open(out));
    const VtFileHeader& h = c.header();
    CHECK(h.maxLevel == 4);
    CHECK((h.fullyBuiltLevels & (1u << 0)) != 0);
    CHECK((h.fullyBuiltLevels & (1u << 2)) != 0);
    CHECK((h.fullyBuiltLevels & (1u << 4)) != 0);
    CHECK((h.fullyBuiltLevels & (1u << 1)) == 0);   // 奇数层未建
    CHECK((h.fullyBuiltLevels & (1u << 3)) == 0);
    // 未建层无任何瓦片
    int n1 = 1 << 1;
    for (int ty = 0; ty < n1; ++ty)
        for (int tx = 0; tx < n1; ++tx)
            CHECK_FALSE(c.hasTile(1, tx, ty));
    c.close();
    std::filesystem::remove(out, ec);
}

TEST_CASE("vt: 片内拓扑抽稀 —— 共享边一致(无缝)") {
    VtTile t;
    t.originX = 0; t.originY = 0; t.epsg = 4326;
    auto addRing = [&](std::vector<int16_t> pts, uint32_t pg) {
        VtRing r; r.type = RING_FACE; r.hole = 0; r.polyGroup = pg;
        r.firstVertex = t.vertexCount(); r.vertexCount = (uint32_t)(pts.size() / 2);
        t.verts.insert(t.verts.end(), pts.begin(), pts.end());
        t.rings.push_back(r);
    };
    addRing({0,0, 10,0, 10,3, 10,7, 10,10, 0,10}, 1);           // 左方块, x=10 上带共线点
    addRing({10,0, 20,0, 20,10, 10,10, 10,7, 10,3}, 2);         // 右方块, 共享边点序列相同
    REQUIRE(processTileTopology(t, 1.0, 1000, 0.0));            // tol=1; netSize 大 -> 不钉
    REQUIRE(t.rings.size() == 2);
    auto edgeX10 = [&](const VtRing& r) {
        std::vector<std::pair<int16_t,int16_t>> v;
        for (uint32_t i = 0; i < r.vertexCount; ++i) {
            int16_t x = t.verts[(size_t)(r.firstVertex+i)*2];
            int16_t y = t.verts[(size_t)(r.firstVertex+i)*2+1];
            if (x == 10) v.push_back({x, y});
        }
        return v;
    };
    auto a = edgeX10(t.rings[0]), b = edgeX10(t.rings[1]);
    CHECK(a == b);          // 共享边逐点一致 -> 无缝
    CHECK(a.size() == 2);   // 共线点被抽掉, 只剩两端
}

TEST_CASE("vt: 片内小面并入邻面") {
    VtTile t;
    t.originX = 0; t.originY = 0; t.epsg = 4326;
    auto addRing = [&](std::vector<int16_t> pts, uint32_t pg) {
        VtRing r; r.type = RING_FACE; r.hole = 0; r.polyGroup = pg;
        r.firstVertex = t.vertexCount(); r.vertexCount = (uint32_t)(pts.size() / 2);
        t.verts.insert(t.verts.end(), pts.begin(), pts.end());
        t.rings.push_back(r);
    };
    addRing({0,0, 100,0, 100,2, 100,100, 0,100}, 1);   // 大方块(带 x=100 上 y=2 的接点)
    addRing({100,0, 102,0, 102,2, 100,2}, 2);          // 小方块(4 格² < 16)
    REQUIRE(processTileTopology(t, 0.0, 1000, 16.0));  // tol=0; 小面并入邻面
    CHECK(t.rings.size() == 1);                        // 两面并成一个
}

// 缓存完整性: 数据段大小 == 所有有效槽 size 之和(即无垃圾), 且 offset 不重复。
TEST_CASE("vt: 缓存完整性(数据段无垃圾/无重复 offset)") {
    std::string src = makeTestGeoJSON();
    std::string out = tempPath("peekgis_vt_integrity.vtk");
    std::error_code ec;
    std::filesystem::remove(out, ec);
    VtBuildConfig cfg;
    cfg.forceLevel = 2;
    cfg.dstEpsg = 4326;
    VtBuildStats st;
    REQUIRE(buildVtCache(src, 0, out, cfg, st));

    std::ifstream f(out, std::ios::binary);
    REQUIRE(f);
    VtFileHeader h{};
    f.read((char*)&h, sizeof(h));
    REQUIRE(std::memcmp(h.magic, VT_MAGIC, 8) == 0);
    // 稀疏槽表: 行目录 + 紧凑槽区(不再有满额 4^L 槽表)
    REQUIRE(h.rowDirCount > 0);
    REQUIRE(h.slotAreaOffset > 0);
    std::vector<VtRowDir> rows(h.rowDirCount);
    f.clear();
    f.seekg((std::streamoff)h.rowDirOffset);
    f.read((char*)rows.data(), (std::streamsize)(rows.size() * sizeof(VtRowDir)));
    REQUIRE((size_t)f.gcount() == rows.size() * sizeof(VtRowDir));

    // 校验: 槽区范围连续且不越界; 有效块无重复 offset; 数据段无垃圾
    uint64_t sum = 0, n = 0;
    std::set<uint64_t> offs;
    uint32_t expectSlot = 0;
    for (const auto& r : rows) {
        CHECK(r.slotStart == expectSlot);          // 槽紧凑连续, 无空洞浪费
        expectSlot += r.count;
        for (uint32_t i = 0; i < r.count; ++i) {
            VtSlot s{};
            f.clear();
            f.seekg((std::streamoff)(h.slotAreaOffset + ((uint64_t)r.slotStart + i) * sizeof(VtSlot)));
            f.read((char*)&s, sizeof(s));
            if (!s.valid) continue;
            sum += s.size;
            ++n;
            offs.insert(s.offset);
        }
    }
    CHECK(h.slotAreaOffset + (uint64_t)expectSlot * sizeof(VtSlot) == h.rowDirOffset);
    CHECK(n > 0);
    CHECK(offs.size() == n);                  // 无重复 offset: 每片只写一次
    CHECK(sum == h.dataEnd - h.dataStart);    // 数据段无垃圾
    // 稀疏化收益: 槽数应远小于满额 4^L 之和(这是 B1 的核心目的)
    uint64_t dense = 0;
    for (int L = 0; L <= (int)h.maxLevel; ++L) dense += slotCount(L);
    CHECK((uint64_t)expectSlot < dense);
    f.close();
    std::filesystem::remove(out, ec);
}

// 回归(真实时序): 渲染层在构建**开始**时就 open 了输出文件 —— 那时还没 finalize,
// 行目录不存在, 它的 rowDir_ 是空的。构建完成后 app 调 reloadHeader() 切回视口模式。
// 若 reloadHeader 只重载头不重载行目录, 这个实例就永远读不到片 ->
// "构建完成后仍一片都画不出, 必须退出重开(重新 open 才会 loadRowDir)才行"。
TEST_CASE("vt: 构建前打开的读端 reloadHeader 后能读片(未 finalize 时已 open)") {
    std::string out = tempPath("peekgis_vt_preread.vtk");
    std::error_code ec;
    std::filesystem::remove(out, ec);

    // 1) 构建端 create: 只有头, 无行目录
    VtFileHeader h{};
    std::memcpy(h.magic, VT_MAGIC, 8);
    h.version = VT_VERSION;
    h.headerSize = sizeof(VtFileHeader);
    h.tileSize = 512;
    h.pad = 10;
    h.fineTileSize = 1024;
    h.maxLevel = 2;
    h.srcEpsg = 4326;
    h.dstEpsg = 4326;
    h.minx = 0; h.miny = 0; h.maxx = 1; h.maxy = 1;
    h.originX = 0; h.originY = 0; h.tileW0 = 1;
    VtCache w;
    REQUIRE(w.create(out, h));

    // 2) 渲染层在 finalize 之前就 open(等价于 app 构建期挂渲染层)
    VtCache r;
    REQUIRE(r.open(out));
    CHECK(r.dirEmpty() == true);          // 未 finalize: 无行目录

    // 3) 构建端写片 + finalize
    VtTile t;
    t.originX = 0; t.originY = 0; t.epsg = 4326;
    for (int i = 0; i < 8; ++i) { t.verts.push_back(i * 10); t.verts.push_back(i * 10); }
    VtRing rg{ 0, 4 };                   // 4 点 -> 2 段线
    t.rings.push_back(rg);
    REQUIRE(w.writeTile(2, 0, 0, t));
    w.setFullyBuilt(2);
    REQUIRE(w.finalize());

    // 4) 交接: 读端只 reloadHeader(不是重新 open) —— 必须能读到片
    REQUIRE(r.reloadHeader());
    CHECK(r.dirEmpty() == false);         // 行目录已重载
    CHECK(r.header().rowDirCount > 0);
    CHECK(r.hasTile(2, 0, 0));
    VtTile t2;
    REQUIRE(r.readTile(2, 0, 0, t2));
    CHECK(t2.vertexCount() == 8);
    CHECK(t2.rings.size() == 1);

    w.close();
    r.close();
    std::filesystem::remove(out, ec);
}

TEST_CASE("vt: chooseVtLevelWanted 期望层") {
    // 目标: 每片约 512 像素 -> tileW0/(512*2^L) ≈ scale
    double tileW0 = 1000.0;
    CHECK(chooseVtLevelWanted(1000.0 / 1024.0, tileW0, 24) == 1);
    CHECK(chooseVtLevelWanted(1000.0 / 2048.0, tileW0, 24) == 2);
    CHECK(chooseVtLevelWanted(1000.0 / 512.0, tileW0, 24) == 0);
    // 足够深: 期望层超过 cap -> 封顶
    CHECK(chooseVtLevelWanted(1000.0 / (512.0 * (double)(1 << 20)), tileW0, 8) == 8);
    CHECK(chooseVtLevelWanted(1000.0 / (512.0 * (double)(1 << 20)), tileW0, 24) == 20);
    // 非法输入 -> 0
    CHECK(chooseVtLevelWanted(0.0, tileW0, 24) == 0);
    CHECK(chooseVtLevelWanted(1.0, 0.0, 24) == 0);
    CHECK(chooseVtLevelWanted(-1.0, tileW0, 24) == 0);
}

TEST_CASE("vt: RawRegionStream 越最深层的可见区直读(分块/路由/只读可见区)") {
    peekg::data::ensureGdal();
    std::string src = makeTestGeoJSON();
    // 网格: origin(0,0), L0 边长 128 覆盖 [0,100]^2 单象限; 直读层 L4 (maxLevel=2)
    const double originX = 0, originY = 0, S = 128.0;
    const int level = 4, maxLevel = 2;
    // 可见区: 全要素所在象限
    double rx0 = 0, ry0 = 0, rx1 = 100, ry1 = 100;

    RawRegionStream rs;
    REQUIRE(rs.open(src, 0, 4326, level, maxLevel, originX, originY, S, rx0, ry0, rx1, ry1));
    CHECK(rs.isOpen());
    CHECK(rs.featureCount() == 3);   // 面 + 线 + 点

    long long s1 = 0; double m1 = 0; bool d1 = false;
    rs.chunk(1, 50.0, s1, m1, d1);          // 限 1 个要素 -> 未读完
    CHECK(s1 == 1);
    CHECK_FALSE(d1);

    long long s2 = 0; double m2 = 0; bool d2 = false;
    rs.chunk(1000, 50.0, s2, m2, d2);       // 继续读到 EOF
    CHECK(s2 == 2);
    CHECK(d2);
    CHECK(rs.featureCount() == 3);          // 未受影响

    std::vector<std::pair<uint64_t, VtTile>> tiles;
    rs.takeTiles(tiles);
    CHECK_FALSE(tiles.empty());
    // 所有瓦片均在可见区的瓦片下标范围内(L4: tileW = 128/16 = 8)
    double tileW = S / (double)(1 << level);
    int vtx0 = (int)std::floor((rx0 - originX) / tileW);
    int vtx1 = (int)std::floor((rx1 - originX) / tileW);
    int vty0 = (int)std::floor((ry0 - originY) / tileW);
    int vty1 = (int)std::floor((ry1 - originY) / tileW);
    bool sawFace = false, sawLine = false, sawPoint = false;
    for (auto& kv : tiles) {
        uint64_t k = kv.first;
        int lv = (int)((k >> 48) & 0xff);
        CHECK(lv == level);
        int tx = (int)((k >> 24) & 0xffffff);
        int ty = (int)(k & 0xffffff);
        CHECK(tx >= vtx0); CHECK(tx <= vtx1);
        CHECK(ty >= vty0); CHECK(ty <= vty1);
        for (const VtRing& r : kv.second.rings) {
            if (r.type == RING_FACE) sawFace = true;
            if (r.type == RING_LINE) sawLine = true;
            if (r.type == RING_POINT) sawPoint = true;
        }
    }
    CHECK(sawFace);    // 方形面(含孔)
    CHECK(sawLine);    // 对角线
    CHECK(sawPoint);   // 点
    rs.close();
}

TEST_CASE("vt: RawRegionStream 区域外要素被空间过滤排除(不交付/不计数)") {
    peekg::data::ensureGdal();
    std::string path = tempPath("peekgis_vt_raw_skip.geojson");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    GDALDriverH drv = GDALGetDriverByName("GeoJSON");
    REQUIRE(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv, path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    REQUIRE(ds != nullptr);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    OGRLayerH lyr = GDALDatasetCreateLayer(ds, "test", srs, wkbUnknown, nullptr);
    REQUIRE(lyr != nullptr);
    auto addWkt = [&](const char* wkt) {
        OGRGeometryH g = nullptr;
        std::string buf(wkt);
        char* p = buf.data();
        OGR_G_CreateFromWkt(&p, nullptr, &g);
        REQUIRE(g != nullptr);
        OGRFeatureH f = OGR_F_Create(OGR_L_GetLayerDefn(lyr));
        OGR_F_SetGeometryDirectly(f, g);
        OGR_L_CreateFeature(lyr, f);
        OGR_F_Destroy(f);
    };
    addWkt("POINT (50 50)");        // 可见区内
    addWkt("POINT (5000 5000)");    // 可见区外远处(被空间过滤排除)
    OSRDestroySpatialReference(srs);
    GDALClose(ds);

    RawRegionStream rs;
    REQUIRE(rs.open(path, 0, 4326, 4, 2, 0, 0, 128.0, 0, 0, 100, 100));
    // 区域外要素不参与计数/读取: featureCount 是过滤后的命中子集
    CHECK(rs.featureCount() == 1);
    long long scanned = 0; double ms = 0; bool done = false;
    rs.chunk(1000, 50.0, scanned, ms, done);
    CHECK(scanned == 1);   // 只读到区域内那一个
    CHECK(done);
    std::vector<std::pair<uint64_t, VtTile>> tiles;
    rs.takeTiles(tiles);
    // 只有可见区(0..100)内的点会路由到瓦片; 5000 处的点被过滤掉
    CHECK_FALSE(tiles.empty());
    for (auto& kv : tiles) {
        int tx = (int)((kv.first >> 24) & 0xffffff);
        int ty = (int)(kv.first & 0xffffff);
        CHECK(tx <= 12);   // 100/8 = 12.5 -> 可见区 tx 上限
        CHECK(ty <= 12);
    }
    rs.close();
    std::filesystem::remove(path, ec);
}

TEST_CASE("vt: 覆盖度栅格 序列化往返 + 渲染按像素聚合") {
    using namespace peekg::vt;
    // 1) 序列化往返: (格号<<8|计数) 追加在环之后, 旧缓存(无该段)也能解
    VtTile t;
    t.originX = 10.0; t.originY = 20.0; t.epsg = 4269;
    const int N = 16;
    for (int i = 0; i < 5; ++i) t.cover.push_back((uint32_t)(i * 7) << 8 | (uint32_t)(i + 1));
    std::vector<uint8_t> raw;
    serializeTile(t, raw);
    VtTile t2;
    REQUIRE(deserializeTile(raw.data(), raw.size(), t2));
    REQUIRE(t2.cover.size() == t.cover.size());
    for (size_t i = 0; i < t.cover.size(); ++i) CHECK(t2.cover[i] == t.cover[i]);
    CHECK(t2.originX == 10.0);
    CHECK(t2.epsg == 4269);
    // 旧格式(只有环, 没有覆盖度段)必须仍能解
    VtTile t3;
    std::vector<uint8_t> old;
    t3.originX = 1.0; t3.originY = 2.0; t3.epsg = 4326;
    serializeTile(t3, old);
    size_t cut = old.size();   // serializeTile 会写 coverCount=0, 模拟旧文件直接截断
    REQUIRE(deserializeTile(old.data(), cut - 4, t3));
    CHECK(t3.cover.empty());

    // 2) 渲染: 每像素最多一个标记, 标记大小随该格计数增长(密度), 且有像素上限
    auto marks = [&](double cellPx, uint32_t cnt, int& nFillVerts) {
        VtTile c;
        c.originX = 0.0; c.originY = 0.0; c.epsg = 4269;
        for (int i = 0; i < 16; ++i) c.cover.push_back((uint32_t)i << 8 | cnt);  // 4x4 格
        std::vector<float> lines, points, fill;
        buildTileGeometry(c, 1.0, true, lines, points, fill, 0.0, cellPx, N);
        nFillVerts = (int)(fill.size() / 2);
        return fill;
    };
    int nv = 0;
    marks(1.0, 1, nv);      CHECK(nv == 16 * 6);   // 1 格 = 1 像素: 16 格 -> 16 个标记
    marks(0.25, 1, nv);     CHECK(nv == 4 * 6);    // 1 格 = 0.25 像素: K=4 -> 4x4 并成 1 个
    marks(0.0625, 1, nv);   CHECK(nv == 1 * 6);    // K=16 -> 整行并成 1 个
    // 大小随计数: 1px/格 时, 计数 1 -> 1px, 计数 8 -> 3px
    auto widthPx = [&](uint32_t cnt) {
        int n2 = 0;
        std::vector<float> f = marks(1.0, cnt, n2);
        REQUIRE(f.size() == 16 * 12);   // 16 个标记 x 6 顶点 x 2 float
        return (double)f[2] - f[0];
    };
    CHECK(widthPx(1) == doctest::Approx(1.0));
    CHECK(widthPx(2) == doctest::Approx(1.5));
    CHECK(widthPx(8) == doctest::Approx(3.0));
    // 像素上限 6px: 8px/格 时单个 1 格的标记也只画 6px 宽
    {
        VtTile c;
        c.originX = 0.0; c.originY = 0.0; c.epsg = 4269;
        c.cover.push_back(0u << 8 | 1u);
        std::vector<float> lines, points, fill;
        buildTileGeometry(c, 1.0, true, lines, points, fill, 0.0, 8.0, N);
        REQUIRE(fill.size() == 12);
        CHECK(fill[2] - fill[0] == doctest::Approx(6.0));   // 8px 被截到 6px
    }
}
