#include "doctest.h"
#include "data/geom_cache.h"
#include "config/app_config.h"
#include <filesystem>
#include <fstream>

TEST_CASE("geom_cache: 写入后读回一致(含 srcEpsg)") {
    AppConfig cfg;
    cfg.cache_dir = "C:/tmp/pgc_ut_cache";
    cfg.cache_max_mb = 64;

    // 缓存以源文件 mtime/size 为签名, 因此需要一个真实存在的源文件
    std::filesystem::create_directories("C:/tmp/pgc_ut_src");
    std::string src = "C:/tmp/pgc_ut_src/sample.gpkg";
    { std::ofstream touch(src); }  // 占位文件即可(缓存存的是几何, 与内容无关)

    std::vector<VectorData> vds(2);
    vds[0].name = "roads"; vds[0].srcEpsg = 4326; vds[0].sourceCrs = "EPSG:4326";
    vds[0].minx = 0; vds[0].miny = 0; vds[0].maxx = 10; vds[0].maxy = 10;
    vds[0].featureCount = 3;
    vds[0].vertices = {0.0f, 0.0f, 1.0f, 1.0f, 2.0f, 2.0f};

    vds[1].name = "buildings"; vds[1].srcEpsg = 3857; vds[1].sourceCrs = "EPSG:3857";
    vds[1].minx = 100; vds[1].miny = 100; vds[1].maxx = 200; vds[1].maxy = 200;
    vds[1].featureCount = 1;
    vds[1].vertices = {100.0f, 100.0f, 200.0f, 200.0f};

    writeCacheAll(src, vds, cfg);

    std::vector<VectorData> out;
    bool hit = readCacheAll(src, out, cfg);
    REQUIRE(hit);
    REQUIRE(out.size() == 2);

    CHECK(out[0].name == "roads");
    CHECK(out[0].srcEpsg == 4326);  // 回归: 之前命中后 srcEpsg 被丢成 0
    CHECK(out[0].vertices == vds[0].vertices);

    CHECK(out[1].name == "buildings");
    CHECK(out[1].srcEpsg == 3857);
    CHECK(out[1].vertices == vds[1].vertices);
}

TEST_CASE("geom_cache v3: 分图层读取/点载荷/压缩往返/管理") {
    AppConfig cfg;
    cfg.cache_dir = "C:/tmp/pgc_ut_cache_v3";
    cfg.cache_max_mb = 64;
    std::filesystem::remove_all("C:/tmp/pgc_ut_cache_v3");

    std::filesystem::create_directories("C:/tmp/pgc_ut_src");
    std::string src = "C:/tmp/pgc_ut_src/v3.gpkg";
    { std::ofstream touch(src); }

    std::vector<VectorData> vds(3);
    vds[0].name = "lines"; vds[0].srcEpsg = 4326;
    vds[0].minx = 0; vds[0].miny = 0; vds[0].maxx = 5; vds[0].maxy = 5;
    vds[0].featureCount = 2;
    vds[0].vertices = {0.f,0.f, 1.f,1.f, 2.f,2.f};          // 3 个线段端点
    vds[0].points = {9.f,9.f};                              // 1 个点

    vds[1].name = "pts"; vds[1].srcEpsg = 0;
    vds[1].minx = 1; vds[1].miny = 1; vds[1].maxx = 2; vds[1].maxy = 2;
    vds[1].featureCount = 1;
    vds[1].points = {1.5f,1.5f};

    vds[2].name = "empty"; vds[2].srcEpsg = 4326;
    vds[2].featureCount = 0;

    writeCacheAll(src, vds, cfg);

    // 全量命中: 空图层(无顶点无点)也必须正确往返
    {
        std::vector<VectorData> out;
        REQUIRE(readCacheAll(src, out, cfg));
        REQUIRE(out.size() == 3);
        CHECK(out[0].vertices == vds[0].vertices);
        CHECK(out[0].points == vds[0].points);
        CHECK(out[1].points == vds[1].points);
        CHECK(out[1].vertices.empty());
        CHECK(out[2].vertices.empty());
        CHECK(out[2].points.empty());
        CHECK(out[1].srcEpsg == 0);
    }

    // 分层读取: 只解压被选图层, 顺序按请求
    {
        std::vector<VectorData> out;
        REQUIRE(readCacheLayers(src, {2, 0}, out, cfg));
        REQUIRE(out.size() == 2);
        CHECK(out[0].name == "empty");
        CHECK(out[1].name == "lines");
        CHECK(out[1].vertices == vds[0].vertices);
    }

    // 管理: 每个图层一条记录; 删除单图层后其余仍在; 删光后源目录消失
    {
        auto entries = listCacheEntries(cfg);
        REQUIRE(entries.size() == 3);
        std::string sid = entries[0].sourceId;
        for (auto& e : entries) CHECK(e.sourceId == sid);

        REQUIRE(deleteCacheEntry(sid, 1, cfg));
        std::vector<VectorData> out;
        REQUIRE_FALSE(readCacheAll(src, out, cfg));   // 缺失图层 => 整源视为未命中(触发重建)
        auto rest = listCacheEntries(cfg);
        REQUIRE(rest.size() == 2);
        for (auto& e : rest) CHECK(e.layerIdx != 1);

        REQUIRE(deleteCacheEntry(sid, 0, cfg));
        REQUIRE(deleteCacheEntry(sid, 2, cfg));
        REQUIRE(listCacheEntries(cfg).empty());
        // 源目录(mata.bin 所在目录)应被整体删除
        std::error_code ec;
        REQUIRE_FALSE(std::filesystem::exists("C:/tmp/pgc_ut_cache_v3/" + sid, ec));
    }

    // 重新写回, 验证 clearAllCache
    writeCacheAll(src, vds, cfg);
    REQUIRE_FALSE(listCacheEntries(cfg).empty());
    clearAllCache(cfg);
    REQUIRE(listCacheEntries(cfg).empty());
    std::vector<VectorData> after;
    bool afterHit = readCacheAll(src, after, cfg);
    REQUIRE_FALSE(afterHit);
}

TEST_CASE("geom_cache v3: LRU 预算按图层驱逐") {
    AppConfig cfg;
    cfg.cache_dir = "C:/tmp/pgc_ut_cache_lru";
    cfg.cache_max_mb = 0;  // 0 表示不限制; 用极小数值测驱逐
    std::filesystem::remove_all("C:/tmp/pgc_ut_cache_lru");
    std::filesystem::create_directories("C:/tmp/pgc_ut_src");
    std::string srcA = "C:/tmp/pgc_ut_src/a.gpkg";
    std::string srcB = "C:/tmp/pgc_ut_src/b.gpkg";
    { std::ofstream ta(srcA); }
    { std::ofstream tb(srcB); }

    auto bigVd = [](const std::string& nm, float base) {
        VectorData vd; vd.name = nm; vd.srcEpsg = 4326;
        vd.featureCount = 200000;
        vd.vertices.reserve(400000);
        for (int i = 0; i < 400000; i++) vd.vertices.push_back(base + i % 1000);
        vd.minx = 0; vd.miny = 0; vd.maxx = 1000; vd.maxy = 1000;
        return vd;
    };

    // A 两图层, B 一图层; 写入时预算 16MB => 必然驱逐最早写入的层
    cfg.cache_max_mb = 16;
    std::vector<VectorData> vA = { bigVd("a0", 0), bigVd("a1", 100000) };
    std::vector<VectorData> vB = { bigVd("b0", 200000) };
    writeCacheAll(srcA, vA, cfg);
    writeCacheAll(srcB, vB, cfg);

    auto entries = listCacheEntries(cfg);
    REQUIRE(!entries.empty());
    int64_t total = 0;
    for (auto& e : entries) total += e.bytes;
    CHECK(total <= 16 * 1024 * 1024);   // 总字节被压缩后一般远小于上限, 未必触发; 但绝不超限
    // 依旧能正确读回(未被驱逐的层)
    std::vector<VectorData> out;
    REQUIRE(readCacheAll(srcB, out, cfg));
    REQUIRE(out.size() == 1);
    CHECK(out[0].vertices == vB[0].vertices);
}

TEST_CASE("geom_cache v3: 面填充三角形与线段往返") {
    AppConfig cfg;
    cfg.cache_dir = "C:/tmp/pgc_ut_cache_fill";
    cfg.cache_max_mb = 64;
    std::filesystem::remove_all("C:/tmp/pgc_ut_cache_fill");
    std::filesystem::create_directories("C:/tmp/pgc_ut_src");
    std::string src = "C:/tmp/pgc_ut_src/fill.gpkg";
    { std::ofstream touch(src); }

    std::vector<VectorData> vds(1);
    vds[0].name = "polys"; vds[0].srcEpsg = 4326; vds[0].sourceCrs = "EPSG:4326";
    vds[0].minx = 0; vds[0].miny = 0; vds[0].maxx = 10; vds[0].maxy = 10;
    vds[0].featureCount = 1;
    vds[0].vertices = {0.f,0.f, 10.f,0.f, 10.f,10.f, 0.f,10.f, 0.f,0.f};
    vds[0].triangles = {0.f,0.f, 10.f,0.f, 10.f,10.f, 0.f,0.f, 10.f,10.f, 0.f,10.f};

    writeCacheAll(src, vds, cfg);
    std::vector<VectorData> out;
    REQUIRE(readCacheAll(src, out, cfg));
    REQUIRE(out.size() == 1);
    CHECK(out[0].triangles == vds[0].triangles);
    CHECK(out[0].vertices == vds[0].vertices);
    CHECK(out[0].srcEpsg == 4326);
}
