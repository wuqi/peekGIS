#include "data/raster_reader.h"
#include "data/gdal_common.h"
#include "data/geoloc.h"
#include "data/reproject.h"
#include "platform/path_util.h"
#include <gdal.h>
#include <ogr_srs_api.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <limits>
#include <filesystem>

namespace peekg::data {
namespace {

// 解析 openSpec: 可能是普通路径, 也可能是 "格式:子数据集全名"。
// 统一用 GDALOpenEx 打开为栅格数据集。
GDALDatasetH openRaster(const std::string& openSpec) {
    ensureGdal();
    const char* name = openSpec.c_str();
    return GDALOpenEx(name, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
}

// 提取仿射变换 -> 范围的辅助
void rasterExtentFromGeo(const double* gt, int w, int h, double& minx, double& miny,
                         double& maxx, double& maxy) {
    // 像素(i,j)世界坐标: x=gt0 + i*gt1 + j*gt2;  y=gt3 + i*gt4 + j*gt5
    // 取四角
    double x0 = gt[0], y0 = gt[3];                      // (0,0)
    double x1 = gt[0] + w * gt[1], y1 = gt[3] + w * gt[4];   // (w,0)
    double x2 = gt[0] + h * gt[2], y2 = gt[3] + h * gt[5];   // (0,h)
    double x3 = gt[0] + w * gt[1] + h * gt[2];                // (w,h)
    double y3 = gt[3] + w * gt[4] + h * gt[5];
    minx = std::min({x0, x1, x2, x3});
    maxx = std::max({x0, x1, x2, x3});
    miny = std::min({y0, y1, y2, y3});
    maxy = std::max({y0, y1, y2, y3});
}

}  // namespace

bool identifyRasterSample(const std::string& openSpec,
                          const double geo[6], int width, int height,
                          double dx, double dy,
                          int displayEpsg, int srcEpsg,
                          int& outCol, int& outRow,
                          double& outSx, double& outSy,
                          std::vector<double>& outBandValues) {
    ensureGdal();
    // 显示(世界)坐标 -> 源 CRS 坐标(必要时单点重投影)
    double sx = dx, sy = dy;
    if (displayEpsg != 0 && srcEpsg != 0 && displayEpsg != srcEpsg) {
        if (!reprojectPoint(dx, dy, displayEpsg, srcEpsg, sx, sy)) {
            sx = dx; sy = dy;   // 重投影失败按同一坐标系处理(反算可能不准, 但尽力)
        }
    }
    outSx = sx; outSy = sy;

    // 仿射反算像素(像方坐标)
    if (geo[1] == 0 || geo[5] == 0) return false;
    double col = (sx - geo[0]) / geo[1];
    double row = (sy - geo[3]) / geo[5];
    int ic = (int)std::floor(col + 0.5);
    int ir = (int)std::floor(row + 0.5);
    if (ic < 0 || ir < 0 || ic >= width || ir >= height) return false;
    outCol = ic; outRow = ir;

    GDALDatasetH ds = GDALOpenEx(openSpec.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) return false;
    outBandValues.clear();
    int nBands = GDALGetRasterCount(ds);
    for (int b = 1; b <= nBands; b++) {
        GDALRasterBandH band = GDALGetRasterBand(ds, b);
        double v = 0;
        if (GDALRasterIO(band, GF_Read, ic, ir, 1, 1, &v, 1, 1, GDT_Float64, 0, 0) != CE_None)
            v = std::numeric_limits<double>::quiet_NaN();
        outBandValues.push_back(v);
    }
    GDALClose(ds);
    return true;
}

bool readRasterMetadata(const std::string& path, RasterData& out) {
    // geolocation 系 netCDF: 按「用户选的变量+时次」修复并 warp 成 EPSG:4326 GeoTIFF 缓存,
    // 再当普通栅格读。这样 lat/lon 等坐标子数据集不会出现在列表/图层里, 数据层直接落位。
    std::string openSpec = path;
    std::string shownName = baseName(path);
    std::string geoPath, geoVar;
    int geoTime = 0;
    if (parseGeolocSpec(path, geoPath, geoVar, geoTime)) {
        std::string cache = geolocCachePath(geoPath, geoVar, geoTime);
        if (std::filesystem::exists(cache) || buildGeolocationWarp(geoPath, geoVar, geoTime, cache)) {
            openSpec = cache;
            shownName = geoVar;
        }
    } else if (netcdfHasGeolocation(path)) {
        // 裸路径兜底(未走 subdataset 对话框): 首个变量、首个时次
        std::vector<GeolocVar> vv;
        if (geolocationVars(path, vv) && !vv.empty()) {
            std::string cache = geolocCachePath(path, vv[0].var, 0);
            if (std::filesystem::exists(cache) || buildGeolocationWarp(path, vv[0].var, 0, cache)) {
                openSpec = cache;
                shownName = vv[0].var;
            }
        }
    }
    GDALDatasetH ds = openRaster(openSpec);
    if (!ds) return false;
    out.openSpec = openSpec;
    out.name = shownName;
    out.width = GDALGetRasterXSize(ds);
    out.height = GDALGetRasterYSize(ds);
    out.maxDim = std::max((long long)out.width, (long long)out.height);
    out.bandCount = GDALGetRasterCount(ds);

    double gt[6] = {0,0,0,0,0,0};
    CPLErr ge = GDALGetGeoTransform(ds, gt);
    if (ge == CE_None && (gt[1] != 0 || gt[5] != 0)) {
        for (int i = 0; i < 6; i++) out.geo[i] = gt[i];
        rasterExtentFromGeo(gt, out.width, out.height, out.minx, out.miny, out.maxx, out.maxy);
    }

    // CRS
    const char* wkt = GDALGetProjectionRef(ds);
    if (wkt && wkt[0]) {
        OGRSpatialReferenceH srs = OSRNewSpatialReference(wkt);
        if (srs) {
            out.srcEpsg = gdalSrsEpsg(srs);
            if (out.srcEpsg) out.sourceCrs = "EPSG:" + std::to_string(out.srcEpsg);
            else out.sourceCrs = "unknown";
            OSRDestroySpatialReference(srs);
        } else out.sourceCrs = "unknown";
    } else out.sourceCrs = "unknown";

    // 金字塔
    // HAS_PYRAMIDS 常见于TIFF; 其他格式看是否有任意overview
    const char* pyr = GDALGetMetadataItem(ds, "HAS_PYRAMIDS", nullptr);
    out.hasRasterPyramids = (pyr && strcmp(pyr, "YES") == 0);

    // 波段
    for (int b = 1; b <= out.bandCount; b++) {
        GDALRasterBandH hb = GDALGetRasterBand(ds, b);
        RasterBandInfo info;
        info.index = b;
        const char* desc = GDALGetDescription(hb);
        if (desc && desc[0]) info.name = desc;
        else info.name = "波段 " + std::to_string(b);
        GDALColorInterp ci = GDALGetRasterColorInterpretation(hb);
        if (ci == GCI_PaletteIndex && GDALGetRasterColorTable(hb) != nullptr)
            info.isColorTable = true;
        // min/max: 试试已有的统计(approx), 失败则计算
        double mn = 0, mx = 0, mean = 0, sd = 0;
        CPLErr st = GDALGetRasterStatistics(hb, TRUE, TRUE, &mn, &mx, &mean, &sd);
        if (st == CE_None) { info.min = mn; info.max = mx; info.hasMinMax = true; }
        out.bands.push_back(std::move(info));
    }
    GDALClose(ds);
    return true;
}

std::vector<double> readRasterBand(const std::string& openSpec, int band1,
                                  int dstW, int dstH,
                                  double outGeo[6], int& outW, int& outH) {
    GDALDatasetH ds = openRaster(openSpec);
    if (!ds) return {};
    int sw = GDALGetRasterXSize(ds), sh = GDALGetRasterYSize(ds);
    int rw = dstW > 0 ? dstW : sw;
    int rh = dstH > 0 ? dstH : sh;
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    outW = rw; outH = rh;

    GDALRasterBandH hb = GDALGetRasterBand(ds, band1);
    // 按波段原生类型选择读取精度: Float64 用 double, 其余用 float(无损足够)。
    // 这样 NDVI 等 double 波段不丢精度; Int16/UInt8/Int32 等用 float 表示也精确。
    GDALDataType dt = GDALGetRasterDataType(hb);
    bool useDouble = (dt == GDT_Float64);
    std::vector<double> buf((size_t)rw * rh);
    std::vector<float> buf32;
    void* raw = nullptr;
    GDALDataType reqType = useDouble ? GDT_Float64 : GDT_Float32;
    if (useDouble) raw = (void*)buf.data();
    else { buf32.resize((size_t)rw * rh); raw = (void*)buf32.data(); }

    CPLErr e = GDALRasterIO(hb, GF_Read, 0, 0, sw, sh, raw, rw, rh, reqType, 0, 0);
    if (e == CE_None && !useDouble) {
        for (size_t i = 0; i < buf32.size(); i++) buf[i] = buf32[i];
    }
    // 新仿射: 每像素世界大小 = (原绘图区域pt_size * 原pt_size/新pt_size)
    // 为简单, 缩放线性假设: 新分辨率为原分辨率 * (sw/rw)。x/y 方向独立。
    if (e == CE_None && outGeo) {
        double gt[6]; bool has = GDALGetGeoTransform(ds, gt) == CE_None;
        if (has) {
            // 保持左上角不变, 分辨率按对应轴缩放
            outGeo[0] = gt[0];
            outGeo[1] = gt[1] * ((double)sw / rw);
            outGeo[2] = gt[2] * ((double)sh / rh);
            outGeo[3] = gt[3];
            outGeo[4] = gt[4] * ((double)sw / rw);
            outGeo[5] = gt[5] * ((double)sh / rh);
        } else {
            std::fill(outGeo, outGeo + 6, 0.0);
        }
    }
    GDALClose(ds);
    if (e != CE_None) { outW = 0; outH = 0; return {}; }
    return buf;
}

// 读窗口(像素)并重采样到 dw×dh
std::vector<double> readRasterWindow(const std::string& openSpec, int band1,
                                     int x0, int y0, int w0, int h0,
                                     int dw, int dh,
                                     double outGeo[6], double& outMin, double& outMax) {
    outMin = 0; outMax = 0;
    GDALDatasetH ds = openRaster(openSpec);
    if (!ds) return {};
    int sw = GDALGetRasterXSize(ds), sh = GDALGetRasterYSize(ds);
    // 窗口截断到有效范围
    int x = std::max(0, x0), y = std::max(0, y0);
    int w = std::min(w0 + std::max(0, -x0), sw - x);
    int h = std::min(h0 + std::max(0, -y0), sh - y);
    if (w <= 0 || h <= 0) { GDALClose(ds); return {}; }
    int rw = dw > 0 ? dw : w;
    int rh = dh > 0 ? dh : h;
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;

    GDALRasterBandH hb = GDALGetRasterBand(ds, band1);
    GDALDataType dt = GDALGetRasterDataType(hb);
    bool useDouble = (dt == GDT_Float64);
    std::vector<double> buf((size_t)rw * rh);
    std::vector<float> buf32;
    GDALDataType reqType = useDouble ? GDT_Float64 : GDT_Float32;
    void* raw;
    if (useDouble) raw = (void*)buf.data();
    else { buf32.resize((size_t)rw * rh); raw = (void*)buf32.data(); }

    CPLErr e = GDALRasterIO(hb, GF_Read, x, y, w, h, raw, rw, rh, reqType, 0, 0);
    if (e == CE_None && !useDouble) {
        for (size_t i = 0; i < buf32.size(); i++) buf[i] = buf32[i];
    }
    if (e == CE_None && outGeo) {
        double gt[6]; bool has = GDALGetGeoTransform(ds, gt) == CE_None;
        if (has) {
            outGeo[0] = gt[0] + x * gt[1] + y * gt[2];   // 窗口左上角世界坐标
            outGeo[1] = gt[1] * ((double)w / rw);
            outGeo[2] = gt[2] * ((double)w / rw);
            outGeo[3] = gt[3] + x * gt[4] + y * gt[5];
            outGeo[4] = gt[4] * ((double)h / rh);
            outGeo[5] = gt[5] * ((double)h / rh);
        } else {
            std::fill(outGeo, outGeo + 6, 0.0);
        }
    }
    if (e == CE_None) {
        double mn = buf[0], mx = buf[0];
        for (double v : buf) { if (v < mn) mn = v; if (v > mx) mx = v; }
        outMin = mn; outMax = mx;
    }
    GDALClose(ds);
    if (e != CE_None) return {};
    return buf;
}

// 伪彩色色带前向声明(实现在下方)
void pseudoColorMapSample(int index, double t, double& r, double& g, double& b);

std::vector<unsigned char> assembleRasterWindowRgba(const std::string& openSpec,
                                                    const RasterRgbAssemble& as,
                                                    int x0, int y0, int w0, int h0,
                                                    int dw, int dh,
                                                    int& outW, int& outH) {
    outW = 0; outH = 0;
    auto readWin = [&](int band1) -> std::vector<double> {
        if (band1 <= 0) return {};
        double geo[6]; double mn, mx;
        return readRasterWindow(openSpec, band1, x0, y0, w0, h0, dw, dh, geo, mn, mx);
    };
    std::vector<double> a = readWin(as.b1);
    if (a.empty()) return {};
    int w = dw > 0 ? dw : w0;
    int h = dh > 0 ? dh : h0;
    std::vector<unsigned char> rgba((size_t)w * h * 4, 0);
    auto toU8 = [](double v, double mn, double mx) -> unsigned char {
        if (!(mx > mn)) return (unsigned char)0;
        double t = (v - mn) / (mx - mn);
        if (t < 0) t = 0; if (t > 1) t = 1;
        return (unsigned char)(t * 255.0 + 0.5);
    };
    if (as.mode == RasterRgbAssemble::Mode::Gray) {
        for (int i = 0; i < w * h; i++) {
            unsigned char g = toU8(a[i], as.mn, as.mx);
            rgba[i*4+0] = g; rgba[i*4+1] = g; rgba[i*4+2] = g; rgba[i*4+3] = 255;
        }
    } else if (as.mode == RasterRgbAssemble::Mode::Pseudocolor) {
        for (int i = 0; i < w * h; i++) {
            double t = (a[i] - as.mn) / (as.mx - as.mn);
            if (!(as.mx > as.mn)) t = 0;
            if (t < 0) t = 0; if (t > 1) t = 1;
            double r, g, b;
            pseudoColorMapSample(as.colorMap, t, r, g, b);
            rgba[i*4+0] = (unsigned char)(r * 255.0 + 0.5);
            rgba[i*4+1] = (unsigned char)(g * 255.0 + 0.5);
            rgba[i*4+2] = (unsigned char)(b * 255.0 + 0.5);
            rgba[i*4+3] = 255;
        }
    } else {
        std::vector<double> b = readWin(as.b2);
        std::vector<double> c = readWin(as.b3);
        if (b.empty() || c.empty()) return {};
        for (int i = 0; i < w * h; i++) {
            rgba[i*4+0] = toU8(a[i], as.mn, as.mx);
            rgba[i*4+1] = toU8(b[i], as.mn, as.mx);
            rgba[i*4+2] = toU8(c[i], as.mn, as.mx);
            rgba[i*4+3] = 255;
        }
    }
    outW = w; outH = h;
    return rgba;
}

// 重投影栅格四角
void reprojectRasterExtent(RasterData& rd, int displayEpsg) {
    rd.hasDispExtent = false;
    if (rd.srcEpsg == 0 || displayEpsg == 0 || rd.srcEpsg == displayEpsg) return;
    // 四角点: (0,0), (width,0), (width,height), (0,height) 映射到源CRS
    double gt[6];
    for (int i = 0; i < 6; i++) gt[i] = rd.geo[i];
    double px[4], py[4];
    px[0] = gt[0];                      py[0] = gt[3];
    px[1] = gt[0] + rd.width * gt[1];   py[1] = gt[3] + rd.width * gt[4];
    px[2] = gt[0] + rd.width * gt[1] + rd.height * gt[2];  py[2] = gt[3] + rd.width * gt[4] + rd.height * gt[5];
    px[3] = gt[0] + rd.height * gt[2];  py[3] = gt[3] + rd.height * gt[5];
    double dx[4], dy[4];
    bool ok = true;
    for (int i = 0; i < 4; i++) {
        if (!reprojectPoint(px[i], py[i], rd.srcEpsg, displayEpsg, dx[i], dy[i])) {
            ok = false; break;
        }
    }
    if (!ok) return;
    rd.dispMinx = std::min({dx[0], dx[1], dx[2], dx[3]});
    rd.dispMaxx = std::max({dx[0], dx[1], dx[2], dx[3]});
    rd.dispMiny = std::min({dy[0], dy[1], dy[2], dy[3]});
    rd.dispMaxy = std::max({dy[0], dy[1], dy[2], dy[3]});
    rd.hasDispExtent = true;
}

bool readRasterSubDatasets(const std::string& path, std::vector<std::string>& out) {
    ensureGdal();
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) return false;
    char** sub = GDALGetMetadata(ds, "SUBDATASETS");
    if (sub) {
        for (int i = 0; sub[i]; i++) {
            std::string kv = sub[i];
            // key 形如 "SUBDATASET_1_NAME=full_name"; 只要 NAME 项
            if (kv.find("_NAME=") != std::string::npos) {
                size_t eq = kv.find('=');
                if (eq != std::string::npos) out.push_back(kv.substr(eq + 1));
            }
        }
    }
    GDALClose(ds);
    // 有子数据集则返回它们; 空表示单 dataset(调用方直接读 path)
    return true;
}

// 伪彩色色带预设: 每项 kControlPoints 个 (t, r, g, b) 控制点(t∈[0,1])。
// index 为预设编号(colorMap); 采样值 t∈[0,1] 在相邻控制点间线性插值。
void pseudoColorMapSample(int index, double t, double& r, double& g, double& b) {
    // 控制点表: 每行一个预设的 (r,g,b) 数组, 依次对应 t=0..N-1/(N-1)
    static const float gray[][3] = {{0,0,0},{1,1,1}};
    static const float viridis[][3] = {
        {0.267f,0.005f,0.329f},{0.282f,0.211f,0.522f},{0.279f,0.347f,0.606f},
        {0.219f,0.464f,0.556f},{0.129f,0.562f,0.439f},{0.153f,0.697f,0.375f},
        {0.420f,0.812f,0.242f},{0.903f,0.951f,0.349f},{0.997f,0.901f,0.685f},
        {0.941f,0.733f,0.952f},{0.969f,0.432f,0.882f},{0.922f,0.381f,0.106f},
    };
    static const float jet[][3] = {
        {0,0,0.515f},{0,0,1},{0,0.5f,1},{0,1,1},{0.5f,1,0.5f},
        {1,1,0},{1,0.5f,0},{1,0,0},{0.5f,0,0},
    };
    static const float turbo[][3] = {
        {0.190f,0.050f,0.231f},{0.314f,0.041f,0.466f},{0.388f,0.256f,0.465f},
        {0.428f,0.466f,0.374f},{0.404f,0.680f,0.067f},{0.000f,0.781f,0.208f},
        {0.000f,0.848f,0.505f},{0.000f,0.877f,0.746f},{0.000f,0.784f,0.935f},
        {0.248f,0.649f,0.969f},{0.471f,0.436f,0.067f},{0.666f,0.820f,0.712f},
        {0.906f,0.780f,0.188f},{0.997f,0.728f,0.078f},{0.988f,0.636f,0.086f},
        {0.797f,0.296f,0.106f},{0.775f,0.096f,0.057f},{0.969f,0.275f,0.043f},
        {0.971f,0.561f,0.165f},{0.998f,0.760f,0.248f},{0.989f,0.895f,0.300f},
    };
    if (t <= 0) t = 0; if (t >= 1) t = 1;
    const float (*tab)[3]; int n;
    switch (index) {
        case 1: tab = viridis; n = (int)(sizeof(viridis) / sizeof(viridis[0])); break;
        case 2: tab = jet;     n = (int)(sizeof(jet) / sizeof(jet[0])); break;
        case 3: tab = turbo;   n = (int)(sizeof(turbo) / sizeof(turbo[0])); break;
        default: tab = gray;   n = 2; break;   // 0=灰度
    }
    double f = t * (n - 1);
    int i = (int)f;
    if (i >= n - 1) { i = n - 2; f = 1; }
    double u = f - i;
    r = tab[i][0] + u * (tab[i+1][0] - tab[i][0]);
    g = tab[i][1] + u * (tab[i+1][1] - tab[i][1]);
    b = tab[i][2] + u * (tab[i+1][2] - tab[i][2]);
}

std::vector<unsigned char> assembleRasterRgba(const std::string& openSpec,
                                              int dstW, int dstH,
                                              const RasterRgbAssemble& as,
                                              int& outW, int& outH) {
    auto readBand = [&](int band1) -> std::vector<double> {
        if (band1 <= 0) return {};
        double geo[6]; int w = 0, h = 0;
        return readRasterBand(openSpec, band1, dstW, dstH, geo, w, h);
    };
    std::vector<double> a = readBand(as.b1);
    if (a.empty()) { outW = 0; outH = 0; return {}; }
    // 计算实际尺寸(所有波段相同)
    // 重新读一次以获得 w/h: readRasterBand 已给出, 但这里用了内部调用.
    // 简便: 用读入数组长度推尺寸——但 dstW/dstH 需正确.
    int w = dstW, h = dstH;
    std::vector<unsigned char> rgba((size_t)w * h * 4, 0);

    auto toU8 = [](double v, double mn, double mx) -> unsigned char {
        // 归一化 clamp; 0 值(NoData) 直接 0(透明由 4 通道? 这里不区分, 保持 rgb 同值)
        if (!(mx > mn)) return (unsigned char)0;
        double t = (v - mn) / (mx - mn);
        if (t < 0) t = 0; if (t > 1) t = 1;
        return (unsigned char)(t * 255.0 + 0.5);
    };

    if (as.mode == RasterRgbAssemble::Mode::Gray) {
        for (int i = 0; i < w * h; i++) {
            unsigned char g = toU8(a[i], as.mn, as.mx);
            rgba[i*4+0] = g; rgba[i*4+1] = g; rgba[i*4+2] = g; rgba[i*4+3] = 255;
        }
        outW = w; outH = h;
        return rgba;
    } else if (as.mode == RasterRgbAssemble::Mode::Pseudocolor) {
        for (int i = 0; i < w * h; i++) {
            double t = (a[i] - as.mn) / (as.mx - as.mn);
            if (!(as.mx > as.mn)) t = 0;
            if (t < 0) t = 0; if (t > 1) t = 1;
            double r, g, b;
            pseudoColorMapSample(as.colorMap, t, r, g, b);
            rgba[i*4+0] = (unsigned char)(r * 255.0 + 0.5);
            rgba[i*4+1] = (unsigned char)(g * 255.0 + 0.5);
            rgba[i*4+2] = (unsigned char)(b * 255.0 + 0.5);
            rgba[i*4+3] = 255;
        }
        outW = w; outH = h;
        return rgba;
    } else {
        std::vector<double> b = readBand(as.b2);
        std::vector<double> c = readBand(as.b3);
        if (b.empty() || c.empty()) { outW = 0; outH = 0; return {}; }
        for (int i = 0; i < w * h; i++) {
            rgba[i*4+0] = toU8(a[i], as.mn, as.mx);
            rgba[i*4+1] = toU8(b[i], as.mn, as.mx);
            rgba[i*4+2] = toU8(c[i], as.mn, as.mx);
            rgba[i*4+3] = 255;
        }
        outW = w; outH = h;
        return rgba;
    }
}

}  // namespace peekg::data