#include "doctest.h"
#include "data/raster_reader.h"
#include "data/gdal_common.h"
#include "map/map_scene.h"
#include <gdal.h>
#include <cmath>
#include <string>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <set>

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

TEST_CASE("raster without GeoTransform falls back to pixel coordinates") {
    ensureGdal();
    const char* name = "/vsimem/peekgis_unreferenced_rgb.tif";
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    REQUIRE(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv, name, 13, 7, 3, GDT_Byte, nullptr);
    REQUIRE(ds != nullptr);
    unsigned char pixels[13 * 7];
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 13; x++)
            pixels[y * 13 + x] = (unsigned char)(x * 17 + y * 3);
    for (int b = 1; b <= 3; b++)
        REQUIRE(GDALRasterIO(GDALGetRasterBand(ds, b), GF_Write, 0, 0, 13, 7,
                             pixels, 13, 7, GDT_Byte, 0, 0) == CE_None);
    GDALClose(ds);

    RasterData rd;
    REQUIRE(readRasterMetadata(name, rd));
    CHECK_FALSE(rd.hasGeoTransform);
    CHECK(rd.srcEpsg == 0);
    CHECK(rd.sourceCrs == "未配准（像素坐标）");
    REQUIRE(rd.bands.size() == 3);
    CHECK(rd.bands[0].isByte);
    CHECK(rd.geo[0] == 0); CHECK(rd.geo[1] == 1);
    CHECK(rd.geo[2] == 0); CHECK(rd.geo[3] == 0);
    CHECK(rd.geo[4] == 0); CHECK(rd.geo[5] == -1);
    CHECK(rd.minx == 0); CHECK(rd.miny == -7);
    CHECK(rd.maxx == 13); CHECK(rd.maxy == 0);
    CHECK(defaultRasterRenderOptions(rd).mode == RasterRenderMode::RGB);

    double geo[6]{}; int w = 0, h = 0;
    auto values = readRasterBand(name, 1, 5, 3, geo, w, h);
    CHECK(values.size() == 15);
    CHECK(w == 5); CHECK(h == 3);
    CHECK(geo[0] == 0); CHECK(geo[3] == 0);
    CHECK(geo[1] == doctest::Approx(13.0 / 5.0));
    CHECK(geo[5] == doctest::Approx(-7.0 / 3.0));

    int col = -1, row = -1; double sx = 0, sy = 0;
    std::vector<double> samples;
    CHECK(identifyRasterSample(name, rd.geo, rd.width, rd.height, 2.5, -1.5,
                               0, 0, col, row, sx, sy, samples));
    CHECK(col == 3); CHECK(row == 2); CHECK(samples.size() == 3);
    GDALDeleteDataset(drv, name);
}

TEST_CASE("raster palette, nodata and gamma") {
    ensureGdal();
    const char* name = "/vsimem/peekgis_palette.tif";
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    REQUIRE(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv, name, 4, 1, 1, GDT_Byte, nullptr);
    REQUIRE(ds != nullptr);
    unsigned char vals[4] = {0,1,2,3};
    REQUIRE(GDALRasterIO(GDALGetRasterBand(ds,1), GF_Write, 0,0,4,1, vals,4,1,GDT_Byte,0,0) == CE_None);
    GDALColorTableH ct = GDALCreateColorTable(GPI_RGB);
    GDALColorEntry e{};
    e.c1=255; e.c2=0; e.c3=0; e.c4=255; GDALSetColorEntry(ct,0,&e);
    e.c1=0; e.c2=255; GDALSetColorEntry(ct,1,&e);
    e.c1=0; e.c2=0; e.c3=255; GDALSetColorEntry(ct,2,&e);
    e.c1=255; e.c2=255; e.c3=255; GDALSetColorEntry(ct,3,&e);
    GDALSetRasterColorTable(GDALGetRasterBand(ds,1), ct);
    GDALDestroyColorTable(ct);
    GDALClose(ds);

    RasterData rd; REQUIRE(readRasterMetadata(name, rd));
    REQUIRE(rd.bands.size() == 1);
    CHECK(rd.bands[0].colorInterp == RasterColorInterp::Palette);
    CHECK(rd.bands[0].colorTable.size() >= 4);
    auto opts = defaultRasterRenderOptions(rd);
    CHECK(opts.mode == RasterRenderMode::Pseudocolor);
    CHECK(opts.useSourceColorTable);
    RasterRgbAssemble as; as.mode=RasterRgbAssemble::Mode::Pseudocolor;
    as.useColorTable=true; as.colorTable=rd.bands[0].colorTable;
    int w=0,h=0; auto rgba=assembleRasterRgba(name,4,1,as,w,h);
    REQUIRE(rgba.size()==16);
    CHECK(rgba[0]==255); CHECK(rgba[1]==0);
    CHECK(rgba[4]==0); CHECK(rgba[5]==255);
    CHECK(rgba[8]==0); CHECK(rgba[9]==0); CHECK(rgba[10]==255);
    GDALDeleteDataset(drv,name);

    const char* grayName = "/vsimem/peekgis_nodata_gamma.tif";
    ds = GDALCreate(drv, grayName, 3, 1, 1, GDT_Byte, nullptr);
    REQUIRE(ds != nullptr);
    unsigned char gvals[3] = {0,128,255};
    REQUIRE(GDALRasterIO(GDALGetRasterBand(ds,1), GF_Write,0,0,3,1,gvals,3,1,GDT_Byte,0,0)==CE_None);
    GDALSetRasterNoDataValue(GDALGetRasterBand(ds,1), 255.0);
    GDALClose(ds);
    RasterRgbAssemble gray; gray.mode=RasterRgbAssemble::Mode::Gray; gray.mn=0; gray.mx=255;
    gray.gamma=2.0; gray.clipNoData=true;
    rgba=assembleRasterRgba(grayName,3,1,gray,w,h);
    REQUIRE(rgba.size()==12);
    CHECK(rgba[5] > 128);                 // gamma 提亮中间调
    CHECK(rgba[8+3] == 0);                // NoData 透明
    GDALDeleteDataset(drv,grayName);
}

TEST_CASE("raster rotated affine corners and inverse") {
    ensureGdal();
    const char* name = "/vsimem/peekgis_rotated.tif";
    GDALDriverH drv = GDALGetDriverByName("GTiff"); REQUIRE(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv,name,2,2,1,GDT_Byte,nullptr); REQUIRE(ds != nullptr);
    unsigned char px[4] = {1,2,3,4};
    REQUIRE(GDALRasterIO(GDALGetRasterBand(ds,1),GF_Write,0,0,2,2,px,2,2,GDT_Byte,0,0)==CE_None);
    double gt[6] = {0,1,0.5,10,0,-1}; REQUIRE(GDALSetGeoTransform(ds,gt)==CE_None); GDALClose(ds);
    RasterData rd; REQUIRE(readRasterMetadata(name,rd));
    CHECK(rd.hasGeoTransform);
    double x[4],y[4]; REQUIRE(rasterDisplayCorners(rd,0,x,y));
    CHECK(x[0]==0); CHECK(y[0]==10); CHECK(x[1]==2); CHECK(y[1]==10);
    CHECK(x[2]==3); CHECK(y[2]==8); CHECK(x[3]==1); CHECK(y[3]==8);
    int col=-1,row=-1; double sx=0,sy=0; std::vector<double> values;
    CHECK(identifyRasterSample(name,rd.geo,rd.width,rd.height,0.5,9,0,0,col,row,sx,sy,values));
    CHECK(col==0); CHECK(row==1);
    GDALDeleteDataset(drv,name);
}

// 可选真实文件探针: PEEKGIS_TEST_RASTER=F:/path/image.tif tests.exe
// 不写缓存/临时文件, 只验证元数据兜底与底图 RGBA 确实有内容。
TEST_CASE("raster integration probe (PEEKGIS_TEST_RASTER)") {
    const char* env = std::getenv("PEEKGIS_TEST_RASTER");
    if (!env || !*env) { MESSAGE("跳过: 未设置 PEEKGIS_TEST_RASTER"); return; }
    RasterData rd;
    REQUIRE(readRasterMetadata(env, rd));
    REQUIRE(rd.bandCount >= 3);
    CHECK_FALSE(rd.hasGeoTransform);
    CHECK(rd.minx < rd.maxx);
    CHECK(rd.miny < rd.maxy);

    RasterRgbAssemble as;
    as.mode = RasterRgbAssemble::Mode::RGB;
    as.b1 = 1; as.b2 = 2; as.b3 = 3;
    as.mn = 0; as.mx = 255;
    int h = 512;
    int w = std::max(1, (int)std::lround(rd.width * (double)h / rd.height));
    int outW = 0, outH = 0;
    auto rgba = assembleRasterRgba(rd.openSpec, w, h, as, outW, outH);
    REQUIRE(rgba.size() == (size_t)outW * outH * 4);
    std::set<unsigned int> colors;
    size_t visible = 0;
    for (size_t i = 0; i < rgba.size(); i += 4) {
        if (rgba[i + 3]) visible++;
        colors.insert(((unsigned int)rgba[i] << 16) |
                     ((unsigned int)rgba[i + 1] << 8) | (unsigned int)rgba[i + 2]);
    }
    CHECK(visible == (size_t)outW * outH);
    CHECK(colors.size() > 100);
    MESSAGE("image=", rd.width, "x", rd.height, " base=", outW, "x", outH,
            " colors=", colors.size());
}
