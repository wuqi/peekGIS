#include "data/gdal_common.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include <filesystem>

using namespace peekg::data;

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// Windows 下 std::ifstream(std::string) 按 ANSI(GBK) 解码窄字符串, 中文文件名会打不开;
// 这里把 UTF-8 路径正确转成 fs::path, 再统一走 filesystem 检查与 GDAL(GDAL 内部按 UTF-8)。
#ifdef _WIN32
static fs::path u8ToPath(const std::string& u8) {
    if (u8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), w.data(), n);
    return fs::path(w);
}
#else
static fs::path u8ToPath(const std::string& u8) { return fs::path(u8); }
#endif

// 定位测试数据文件(与 tests/testdata/README.md 约定一致):
// 1. 环境变量 envName; 2. 相对工程根 tests/testdata/ 下的 rels(逐级向上回溯);
// 3. abs 绝对兜底。找不到返回空串(调用处打印"跳过", 不做硬失败)。
static std::string locateFile(const char* envName, const std::vector<const char*>& rels,
                              const std::vector<const char*>& abs) {
    auto good = [](const std::string& p) {
        if (p.empty()) return false;
        std::error_code ec;
        return fs::exists(u8ToPath(p), ec);
    };
    if (const char* env = std::getenv(envName))
        if (good(env)) return env;
    auto findUp = [&](const std::string& rel) -> std::string {
        fs::path cur = fs::current_path();
        for (int i = 0; i < 8; ++i) {
            std::string p = (cur / rel).string();
            if (good(p)) return p;
            cur = cur.parent_path();
            if (cur.empty()) break;
        }
        return "";
    };
    for (const char* r : rels) {
        std::string p = findUp(r);
        if (!p.empty()) return p;
    }
    for (const char* a : abs)
        if (good(a)) return a;
    return "";
}

static void dumpRaster(const std::string& path) {
    ensureGdal();
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) { printf("RASTER OPEN FAIL: %s\n", path.c_str()); return; }
    printf("=== Raster: %s ===\n", path.c_str());
    printf("size=%dx%d bands=%d\n", GDALGetRasterXSize(ds), GDALGetRasterYSize(ds),
           GDALGetRasterCount(ds));
    double gt[6] = {0,0,0,0,0,0};
    if (GDALGetGeoTransform(ds, gt) == CE_None)
        printf("geo:      %f %f %f / %f %f %f\n", gt[0], gt[1], gt[2], gt[3], gt[4], gt[5]);
    else
        printf("geo:      (none)\n");
    const char* wkt = GDALGetProjectionRef(ds);
    printf("projection: [%s]\n", wkt ? wkt : "(null)");
    if (wkt && wkt[0]) {
        OGRSpatialReferenceH srs = OSRNewSpatialReference(wkt);
        if (srs) {
            const char* auth = OSRGetAuthorityName(srs, nullptr);
            const char* code = OSRGetAuthorityCode(srs, nullptr);
            printf("  auth=%s code=%s\n", auth ? auth : "(null)", code ? code : "(null)");
            OSRDestroySpatialReference(srs);
        }
    }
    GDALClose(ds);
}

static void srsCodes(OGRSpatialReferenceH srs, const char* label) {
    if (!srs) { printf("  [%s] null\n", label); return; }
    const char* auth = OSRGetAuthorityName(srs, nullptr);
    const char* code = OSRGetAuthorityCode(srs, nullptr);
    printf("  [%s] auth=%s code=%s\n", label, auth ? auth : "(null)", code ? code : "(null)");
    int isGeo = OSRIsGeographic(srs);
    int isProj = OSRIsProjected(srs);
    printf("  [%s] isGeo=%d isProj=%d\n", label, isGeo, isProj);
    char* wkt = nullptr;
    if (OSRExportToWkt(srs, &wkt) == OGRERR_NONE && wkt) {
        printf("  [%s] wkt=[%s]\n", label, wkt);
        CPLFree(wkt);
    }
}

static void dumpVector(const std::string& path) {
    ensureGdal();
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) { printf("VECTOR OPEN FAIL: %s\n", path.c_str()); return; }
    printf("=== Vector: %s ===\n", path.c_str());
    int nl = GDALDatasetGetLayerCount(ds);
    for (int li = 0; li < nl; li++) {
        OGRLayerH lyr = GDALDatasetGetLayer(ds, li);
        OGREnvelope env;
        if (OGR_L_GetExtent(lyr, &env, TRUE) == OGRERR_NONE)
            printf("layer[%d] %s extent: (%.4f,%.4f)-(%.4f,%.4f)\n", li, OGR_L_GetName(lyr),
                   env.MinX, env.MinY, env.MaxX, env.MaxY);
        OGRSpatialReferenceH srs = OGR_L_GetSpatialRef(lyr);
        srsCodes(srs, "orig");
        int epsg = gdalSrsEpsg(srs);
        printf("  -> gdalSrsEpsg = %d\n", epsg);
        if (srs) {
            // OSRFindMatches 尝试
            int nEntries = 0;
            int* conf = nullptr;
            OGRSpatialReferenceH* list = OSRFindMatches(srs, nullptr, &nEntries, &conf);
            printf("  FindMatches n=%d\n", nEntries);
            if (list) {
                for (int i = 0; i < nEntries && i < 6; i++) {
                    const char* ca = OSRGetAuthorityName(list[i], nullptr);
                    const char* cc = OSRGetAuthorityCode(list[i], nullptr);
                    printf("    match[%d] c=%d auth=%s code=%s\n", i,
                           conf ? conf[i] : -1, ca ? ca : "(null)", cc ? cc : "(null)");
                }
                OSRFreeSRSArray(list);
            }
            CPLFree(conf);
        }
    }
    GDALClose(ds);
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    // 栅格用例: IMAGINE 影像 (UTM Zone 55S)
    std::string img = locateFile("PEEKGIS_TEST_S55_IMG",
        { "tests/testdata/s55/S55_12.img", "testdata/s55/S55_12.img" },
        { "G:/s55/下S55_12.img" });
    if (img.empty()) printf("跳过: 未找到 S55_12.img (设 PEEKGIS_TEST_S55_IMG)\n");
    else dumpRaster(img);

    // 矢量用例: 公开版 25 万新疆 shapefile (含 ArcGIS 私有 .prj)
    std::string shp = locateFile("PEEKGIS_TEST_AANP",
        { "tests/testdata/aanp.shp", "testdata/aanp.shp" },
        { "G:/cp/公开版25万-新疆shp/aanp.shp" });
    if (shp.empty()) printf("跳过: 未找到 aanp.shp (设 PEEKGIS_TEST_AANP)\n");
    else dumpVector(shp);
    return 0;
}