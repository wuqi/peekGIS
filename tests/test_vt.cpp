#include "doctest.h"
#include "vt/vt_types.h"
#include "vt/vt_cache.h"
#include "vt/vt_build.h"
#include "vt/vt_source.h"
#include "vt/vt_geom.h"
#include "vt/vt_scissor.h"
#include "vt/vt_level.h"
#include "data/gdal_common.h"

#include <ogr_api.h>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace peekg::vt;

namespace {

std::string tempPath(const char* name) {
    // 放仓库内 build/tmp (AGENTS: 禁止写 C 盘 TEMP)
    auto root = std::filesystem::absolute(std::filesystem::path(__FILE__)).parent_path().parent_path();
    auto d = root / "build" / "tmp";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return (d / name).string();
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
    cfg.levels = 0;
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
    cfg.levels = 2;          // 强制 2 层, 稳定可测
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

TEST_CASE("vt: LRU 淘汰后再触达不丢几何(读-合并-写)") {
    std::string src = makeEvictTestGeoJSON();
    std::string out = tempPath("peekgis_vt_evict_test.vtk");
    std::error_code ec;
    std::filesystem::remove(out, ec);

    VtBuildConfig cfg;
    cfg.levels = 2;
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
    cfg.levels = 1;
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
    cfg.levels = 4;          // 强制 4 层
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

// 缓存完整性: 数据段大小 == 所有有效槽 size 之和(即无垃圾), 且 offset 不重复。
// 这条是"同一瓦片被反复 flush 追加"会直接违反的判据(TILE_SIZE 变大时曾出现)。
TEST_CASE("vt: 缓存完整性(数据段无垃圾/无重复 offset)") {
    std::string src = makeTestGeoJSON();
    std::string out = tempPath("peekgis_vt_integrity.vtk");
    std::error_code ec;
    std::filesystem::remove(out, ec);
    VtBuildConfig cfg;
    cfg.levels = 2;
    cfg.dstEpsg = 4326;
    VtBuildStats st;
    REQUIRE(buildVtCache(src, 0, out, cfg, st));

    std::ifstream f(out, std::ios::binary);
    REQUIRE(f);
    VtFileHeader h{};
    f.read((char*)&h, sizeof(h));
    REQUIRE(std::memcmp(h.magic, VT_MAGIC, 8) == 0);
    uint64_t sum = 0, n = 0, off = h.slotTableOffset;
    std::set<uint64_t> offs;
    for (int L = 0; L <= (int)h.maxLevel; ++L) {
        uint64_t sc = slotCount(L);
        std::vector<VtSlot> tbl((size_t)sc);
        f.clear();
        f.seekg((std::streamoff)off);
        f.read((char*)tbl.data(), (std::streamsize)(sc * sizeof(VtSlot)));
        for (uint64_t i = 0; i < sc; ++i) {
            if (!tbl[i].valid) continue;
            sum += tbl[i].size;
            ++n;
            offs.insert(tbl[i].offset);
        }
        off += sc * sizeof(VtSlot);
    }
    CHECK(n > 0);
    CHECK(offs.size() == n);                  // 无重复 offset: 每片只写一次
    CHECK(sum == h.dataEnd - h.dataStart);    // 数据段无垃圾
    f.close();
    std::filesystem::remove(out, ec);
}