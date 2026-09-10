#include "doctest.h"
#include "data/gdal_common.h"
#include "platform/exe_path.h"
#include "toolbox/gdal_proc.h"
#include "toolbox/tool_registry.h"

using namespace peekg::data;

#include <gdal.h>
#include <ogr_spatialref.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

TEST_CASE("gdal_proc: --version 子进程") {
    ensureGdal();
    std::string out, err;
    int code = -1;
    bool launched = GdalCli::run(exeDir() + "/gdal.exe", { "--version" }, &out, &err, &code);
    REQUIRE(launched);
    CHECK(code == 0);
    CHECK(out.find("GDAL 3.12") != std::string::npos);
}

TEST_CASE("tool_registry: 全量导入 + 查找 + 参数模型") {
    ensureGdal();
    ToolRegistry r;
    REQUIRE(r.ensureLoaded(exeDir() + "/gdal.exe"));
    CHECK(r.ready());
    CHECK(r.leaves().size() >= 100);

    const ToolDef* rep = r.find("gdal:raster:reproject");
    REQUIRE(rep != nullptr);
    REQUIRE(rep->isLeaf);
    CHECK_FALSE(rep->description.empty());

    const ArgDef* input = nullptr;
    const ArgDef* output = nullptr;
    for (const ArgDef& a : rep->args) {
        if (a.name == "input") input = &a;
        if (a.name == "output") output = &a;
    }
    REQUIRE(input != nullptr);
    REQUIRE(output != nullptr);
    CHECK(input->required);
    CHECK(input->isInput);
    CHECK(input->datasetType == "raster");
    CHECK(output->isOutput);

    // 局部参数细节
    const ArgDef* resampling = nullptr;
    for (const ArgDef& a : rep->args) if (a.name == "resampling") resampling = &a;
    REQUIRE(resampling != nullptr);
    CHECK(std::find(resampling->choices.begin(), resampling->choices.end(), "bilinear") != resampling->choices.end());

    const ArgDef* sizeArg = nullptr;
    for (const ArgDef& a : rep->args) if (a.name == "size") sizeArg = &a;
    if (sizeArg != nullptr) {
        CHECK(sizeArg->type == "integer_list");
        CHECK(sizeArg->minCount == 2);
        CHECK(sizeArg->maxCount == 2);
    }

    // 顶层分类完整
    CHECK(r.find("gdal:raster") != nullptr);
    CHECK(r.find("gdal:vector") != nullptr);
}

TEST_CASE("tool_registry: buildArgs 组装") {
    ToolRegistry r;
    REQUIRE(r.ensureLoaded(exeDir() + "/gdal.exe"));
    const ToolDef* rep = r.find("gdal:raster:reproject");
    REQUIRE(rep != nullptr);

    // 必填数据集缺失 -> 报错
    std::vector<std::string> args;
    std::string err;
    bool ok = r.buildArgs(*rep, { { "dst-crs", "EPSG:4326" } }, &args, &err);
    CHECK_FALSE(ok);
    CHECK(err.find("input") != std::string::npos);

    // 正常: input/output 位置参数按 schema 顺序, 其余 option 化
    ok = r.buildArgs(*rep, {
                           { "input", "G:/DATA/in.tif" },
                           { "output", "G:/DATA/out.tif" },
                           { "dst-crs", "EPSG:4326" },
                           { "size", "4000,4000" },
                           { "target-aligned-pixels", "true" },   // 布尔 flag -> --name
                           { "overwrite", "false" },               // 布尔 false -> 省略
                       }, &args, &err);
    REQUIRE(ok);
    CHECK(args[0] == "G:/DATA/in.tif");
    CHECK(args.back() == "G:/DATA/out.tif");
    CHECK(std::find(args.begin(), args.end(), "--dst-crs=EPSG:4326") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--size=4000,4000") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--target-aligned-pixels") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--overwrite") == args.end());
}

// 端到端: 造一个小栅格(VRT+PGM), gdal reproject 子进程跑通, 读回校验。
// 用含中文的临时目录验证 UTF-8 路径/参数引号处理。
TEST_CASE("tool_registry e2e: reproject 子进程跑通含中文路径") {
    ensureGdal();
    ToolRegistry r;
    REQUIRE(r.ensureLoaded(exeDir() + "/gdal.exe"));
    const ToolDef* rep = r.find("gdal:raster:reproject");
    REQUIRE(rep != nullptr);

    auto dir = fs::temp_directory_path() / "peek_toolbox_reproject_test";
    fs::create_directories(dir);

    // 进程内造 2x2 GTiff(EPSG:4326)作为源, 交给 gdal 子进程 reproject。
    std::string srcPath = (dir / "src.tif").string();     // UTF-8
    std::string outPath = (dir / "out.tif").string();
    {
        GDALDriverH drv = GDALGetDriverByName("GTiff");
        REQUIRE(drv != nullptr);
        GDALDatasetH ds = GDALCreate(drv, srcPath.c_str(), 2, 2, 1, GDT_Byte, nullptr);
        REQUIRE(ds != nullptr);
        double geo[6] = { 116.0, 0.005, 0.0, 40.0, 0.0, -0.005 };
        GDALSetGeoTransform(ds, geo);
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        REQUIRE(srs != nullptr);
        REQUIRE(OSRImportFromEPSG(srs, 4326) == OGRERR_NONE);
        GDALSetSpatialRef(ds, srs);
        OSRDestroySpatialReference(srs);
        unsigned char pix[4] = { 128, 64, 64, 255 };
        GDALRasterBandH band = GDALGetRasterBand(ds, 1);
        REQUIRE(band != nullptr);
        REQUIRE(GDALRasterIO(band, GF_Write, 0, 0, 2, 2, pix, 2, 2, GDT_Byte, 0, 0) == CE_None);
        GDALClose(ds);
    }

    std::map<std::string, std::string> opts = {
        { "input", srcPath }, { "output", outPath },
        { "dst-crs", "EPSG:3857" }, { "resampling", "nearest" }, { "size", "4,4" },
        { "overwrite", "true" },
    };
    std::vector<std::string> args;
    std::string err;
    REQUIRE(r.runArgs(*rep, opts, &args, &err));

    std::string out, errOut;
    int code = -1;
    REQUIRE(GdalCli::run(exeDir() + "/gdal.exe", args, &out, &errOut, &code));
    REQUIRE_MESSAGE(code == 0, "gdal 失败: " << errOut);

    GDALDatasetH ds = GDALOpenEx(outPath.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    REQUIRE(ds != nullptr);
    CHECK(GDALGetRasterXSize(ds) == 4);
    CHECK(GDALGetRasterYSize(ds) == 4);
    CHECK(GDALGetRasterBand(ds, 1) != nullptr);
    // 输出为 Web Mercator (GDAL 按用户输入 EPSG 写入 authority)
    OGRSpatialReferenceH srs = GDALGetSpatialRef(ds);
    CHECK(srs != nullptr);
    if (srs != nullptr) CHECK(gdalSrsEpsg(srs) == 3857);
    GDALClose(ds);

    fs::remove_all(dir);
}