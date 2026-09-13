#include "doctest.h"
#include "vt/vt_types.h"
#include "vt/vt_cache.h"
#include "vt/vt_build.h"
#include "vt/vt_source.h"
#include "vt/vt_geom.h"
#include "vt/vt_scissor.h"
#include "data/gdal_common.h"

#include <ogr_api.h>
#include <filesystem>
#include <string>
#include <vector>

using namespace peekg::vt;

namespace {

std::string tempPath(const char* name) {
    auto d = std::filesystem::temp_directory_path();
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
    buildTileGeometry(t, 1.0, lines, points, fill);
    CHECK(!lines.empty());
    CHECK(points.size() == 2);
    CHECK(!fill.empty());
    CHECK(fill.size() % 6 == 0);
    // 有孔时三角形顶点数应多于无孔(4 顶点 -> 2 三角 = 12 float; 带孔更多)
    CHECK(fill.size() > 12);
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

TEST_CASE("vt: scissor 非法参数回退整视口") {
    ScissorRect r = tileScissorRect(0, 0, 1, 0, 0, 0.0, 640, 480);
    CHECK(r.x == 0); CHECK(r.y == 0); CHECK(r.w == 640); CHECK(r.h == 480);
}
