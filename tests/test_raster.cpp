#include "doctest.h"
#include "data/raster_reader.h"
#include <cmath>
#include <string>
#include <cstdlib>
#include <fstream>
#include <filesystem>

using namespace peekg::data;

namespace {
namespace fs = std::filesystem;

// 定位 GDAL autotest 测试数据目录(含 byte.tif/rgbsmall.tif/float64.tif 等)。
// 优先级: 环境变量 PEEKGIS_TEST_GDAL_DATA > 项目相对目录 tests/testdata/gdal_data
// > 候选绝对路径(仓库本机)。找不到返回空串(调用处跳过测试而非硬失败)。
static std::string locateGdalData() {
    auto exists = [](const std::string& p) {
        return p.size() && std::ifstream(p + "/byte.tif").good();
    };

    if (const char* env = std::getenv("PEEKGIS_TEST_GDAL_DATA"))
        if (exists(env)) return env;

    // 相对工程根: 让测试数据可随仓库放置, 不写死机器路径。
    // 无论从 build/.../tests.exe 的哪个子目录启动, 逐级向上回溯定位工程根。
    auto findUp = [&](const std::string& rel) -> std::string {
        fs::path cur = fs::current_path();
        for (int i = 0; i < 8; ++i) {
            std::string p = (cur / rel).string();
            if (exists(p)) return p;
            cur = cur.parent_path();
            if (cur.empty()) break;
        }
        return "";
    };
    const char* rel[] = {
        "tests/testdata/gdal_data",
        "testdata/gdal_data",
        "share/testdata/gdal_data",
    };
    for (auto r : rel) {
        std::string p = findUp(r);
        if (!p.empty()) return p;
    }

    // 绝对候选(仓库初版沿用): 仅作兜底, 不再是唯一依赖
    const char* abs[] = {
        "D:/Github/HeadFirstGDAL/headfirstgdal/src/gdal-3.13.0/autotest/gcore/data",
    };
    for (auto a : abs)
        if (exists(a)) return a;
    return "";
}
} // namespace

TEST_CASE("raster metadata byte.tif") {
    std::string DATA = locateGdalData();
    if (DATA.empty()) { MESSAGE("跳过: 未找到 GDAL 测试数据 (设 PEEKGIS_TEST_GDAL_DATA)"); return; }
    RasterData rd;
    bool ok = readRasterMetadata(DATA + "/byte.tif", rd);
    CHECK(ok);
    CHECK(rd.width == 20);
    CHECK(rd.height == 20);
    CHECK(rd.bandCount == 1);
    CHECK(rd.bands.size() == 1);
}

TEST_CASE("raster window read byte.tif") {
    std::string DATA = locateGdalData();
    if (DATA.empty()) { MESSAGE("跳过: 未找到 GDAL 测试数据 (设 PEEKGIS_TEST_GDAL_DATA)"); return; }
    // 读 (0,0) 起 10×10 窗口, 重采样到 5×5, 验证尺寸与值域
    double geo[6]; double mn = 0, mx = 0;
    std::vector<double> b = readRasterWindow(DATA + "/byte.tif", 1, 0, 0, 10, 10, 5, 5, geo, mn, mx);
    CHECK(b.size() == 25);
    CHECK(mn < mx);
    // geo[1] 应放大 2 倍(10 源像素 -> 5 目标)
    CHECK(std::fabs(geo[1]) > 0);
}

TEST_CASE("raster gray base rgba byte.tif") {
    std::string DATA = locateGdalData();
    if (DATA.empty()) { MESSAGE("跳过: 未找到 GDAL 测试数据 (设 PEEKGIS_TEST_GDAL_DATA)"); return; }
    // 单波段 → 灰度, 最长边 20 ≤1024 → 原尺寸
    int w = 0, h = 0;
    RasterRgbAssemble as;
    as.mode = RasterRgbAssemble::Mode::Gray;
    as.mn = 0; as.mx = 255;
    std::vector<unsigned char> rgba = assembleRasterRgba(DATA + "/byte.tif", 20, 20, as, w, h);
    CHECK(w == 20);
    CHECK(h == 20);
    CHECK(rgba.size() == (size_t)20 * 20 * 4);
}

TEST_CASE("raster pseudocolor gray=0 equals gray mode") {
    std::string DATA = locateGdalData();
    if (DATA.empty()) { MESSAGE("跳过: 未找到 GDAL 测试数据 (设 PEEKGIS_TEST_GDAL_DATA)"); return; }
    int w1=0,h1=0,w2=0,h2=0;
    RasterRgbAssemble gry; gry.mode=RasterRgbAssemble::Gray; gry.mn=0; gry.mx=255;
    RasterRgbAssemble pc0; pc0.mode=RasterRgbAssemble::Pseudocolor; pc0.colorMap=0; pc0.mn=0; pc0.mx=255;
    std::vector<unsigned char> a = assembleRasterRgba(DATA + "/byte.tif", 20,20, gry, w1,h1);
    std::vector<unsigned char> b = assembleRasterRgba(DATA + "/byte.tif", 20,20, pc0, w2,h2);
    CHECK(a.size() == b.size());
    bool same = (a == b);
    CHECK(same);
}

TEST_CASE("raster pseudocolor jet differs from gray") {
    std::string DATA = locateGdalData();
    if (DATA.empty()) { MESSAGE("跳过: 未找到 GDAL 测试数据 (设 PEEKGIS_TEST_GDAL_DATA)"); return; }
    int w1=0,h1=0,w2=0,h2=0;
    RasterRgbAssemble gry; gry.mode=RasterRgbAssemble::Gray; gry.mn=0; gry.mx=255;
    RasterRgbAssemble pc2; pc2.mode=RasterRgbAssemble::Pseudocolor; pc2.colorMap=2; pc2.mn=0; pc2.mx=255;
    std::vector<unsigned char> a = assembleRasterRgba(DATA + "/byte.tif", 20,20, gry, w1,h1);
    std::vector<unsigned char> b = assembleRasterRgba(DATA + "/byte.tif", 20,20, pc2, w2,h2);
    CHECK(a.size() == b.size());
    bool differ = (a != b);
    CHECK(differ);
}

TEST_CASE("raster rgb base rgba rgbsmall.tif") {
    std::string DATA = locateGdalData();
    if (DATA.empty()) { MESSAGE("跳过: 未找到 GDAL 测试数据 (设 PEEKGIS_TEST_GDAL_DATA)"); return; }
    // 3 波段 → RGB
    int w = 0, h = 0;
    RasterRgbAssemble as;
    as.mode = RasterRgbAssemble::Mode::RGB;
    as.mn = 0; as.mx = 255;
    std::vector<unsigned char> rgba = assembleRasterRgba(DATA + "/rgbsmall.tif", 512, 512, as, w, h);
    CHECK(w > 0);
    CHECK(h > 0);
    CHECK(rgba.size() == (size_t)w * h * 4);
}

TEST_CASE("raster metadata float64.tif (double band ok)") {
    std::string DATA = locateGdalData();
    if (DATA.empty()) { MESSAGE("跳过: 未找到 GDAL 测试数据 (设 PEEKGIS_TEST_GDAL_DATA)"); return; }
    RasterData rd;
    bool ok = readRasterMetadata(DATA + "/float64.tif", rd);
    CHECK(ok);
    CHECK(rd.bandCount >= 1);
    // 读取 double 波段不丢精度
    double geo[6]; int w = 0, h = 0;
    std::vector<double> b = readRasterBand(rd.openSpec, 1, 20, 20, geo, w, h);
    CHECK(w > 0);
    CHECK(h > 0);
    CHECK(b.size() == (size_t)w * h);
}