#pragma once
#include <string>
#include <vector>

// 栅格数据读取(raster_reader): geolocation 系 netCDF 缓存 + 栅格元数据/波段/窗口/
// RGBA 组装/子数据集枚举。全部收在 peekg::data 命名空间。
namespace peekg::data {

// 双击栅格识别: 在 (dx,dy)(显示 CRS) 处取波段原始值 + 像方坐标 + 源 CRS 坐标。
// geo/width/height 为该栅格在源 CRS 下的仿射与尺寸; displayEpsg!=0 且 != srcEpsg 时把
// (dx,dy) 重投影回源 CRS 再反算像素。命中(像素在界内)返回 true, 写 outCol/outRow、
// outSx/outSy(源CRS X/Y)与每个波段的原始值(outBandValues, 数组长度=波段数)。
bool identifyRasterSample(const std::string& openSpec,
                          const double geo[6], int width, int height,
                          double dx, double dy,
                          int displayEpsg, int srcEpsg,
                          int& outCol, int& outRow,
                          double& outSx, double& outSy,
                          std::vector<double>& outBandValues);

// 单个波段信息
struct RasterBandInfo {
    std::string name;      // 波段名(UTF-8), 回退 "Band N"
    int index = 0;         // 1-based 波段号
    double min = 0, max = 0;
    bool hasMinMax = false;
    bool isColorTable = false;  // 有 Palette 颜色表(伪彩色用)
};

// 栅格图层(单 dataset 或单个 subdataset)
struct RasterData {
    std::string name;        // 显示名(文件名或子数据集名)
    std::string sourceCrs;   // 文本, 如 "EPSG:4326" 或 "unknown"
    int srcEpsg = 0;         // 源 EPSG; 0 未知
    std::string openSpec;    // 用于打开: 单 dataset 为路径, subdataset 为完整子名
    double geo[6] = {0,0,0,0,0,0};  // 仿射变换(像素->世界)
    int width = 0, height = 0;      // 像素尺寸
    double minx = 0, miny = 0, maxx = 0, maxy = 0;  // 地理范围(源 CRS)
    double dispMinx = 0, dispMiny = 0, dispMaxx = 0, dispMaxy = 0;  // 地理范围(显示 CRS, 重投影后)
    bool hasDispExtent = false;    // dispMinx/Miny... 是否已填充
    int bandCount = 0;
    std::vector<RasterBandInfo> bands;
    long long maxDim = 0;    // max(width,height)
    bool hasRasterPyramids = false;  // 是否有 R 金字塔
};

// 读取栅格元数据(尺寸/波段/min/max/范围/子数据集列表)。不读像素, 通常 <100ms。
bool readRasterMetadata(const std::string& path, RasterData& out);

// 栅格像素读取参数
struct RasterReadOpts {
    int dstW = 0, dstH = 0;          // 目标尺寸(降采样); 0=原始尺寸
    std::vector<int> bands;          // 要读的波段(1-based); 空=默认
};

// 将栅格一个波段读为 double 数组(按 opt 降采样)。geo 为缩小后的新仿射, 写入 outGeo。
// 像素数据类型(Float32/Float64)按波段原生精度保留, 避免 NDVI 等 double 波段丢失精度。
// 返回像素数组(Row-major, w*h), 失败返回空。
std::vector<double> readRasterBand(const std::string& openSpec, int band1,
                                   int dstW, int dstH,
                                   double outGeo[6], int& outW, int& outH);

// 读栅格一个原始像素窗口并重采样到 dw×dh(块用)。窗口超界自动截断。
// 用 GDALRasterIO 窗口+目标尺寸降采样(有金字塔时从 overview 快读)。
// geo 为窗口左上角新仿射(分辨率含窗口缩放)。返回 double 数组(row-major dw*dh), 失败空。
std::vector<double> readRasterWindow(const std::string& openSpec, int band1,
                                     int x0, int y0, int w0, int h0,
                                     int dw, int dh,
                                     double outGeo[6], double& outMin, double& outMax);

// 重投影栅格四角到 displayEpsg, 填充 rd.dispMinx/Miny...。失败或 srcEpsg==displayEpsg 时标记 hasDispExtent=false(用源范围)。
void reprojectRasterExtent(RasterData& rd, int displayEpsg);

// 枚举文件的 subdataset 列表(空表示单 dataset)。每个元素即 openSpec。
bool readRasterSubDatasets(const std::string& path, std::vector<std::string>& out);

// 组装底图/细节块 RGBA8 像素(供纹理上传)。mode/rBand/gBand/bBand/min/max 见渲染选项。
// 归一化(t-min)/(max-min) clamp 0..1 用 double, 保 NDVI 等精度。失败返回空。
struct RasterRgbAssemble {
    enum Mode { Gray, RGB, Pseudocolor } mode = Gray;
    int b1 = 1, b2 = 2, b3 = 3;   // Gray/Pseudocolor 用 b1; RGB 用 b1/b2/b3
    double mn = 0, mx = 255;
    int colorMap = 0;             // 伪彩色色带预设索引(仅 Pseudocolor 用)
};
std::vector<unsigned char> assembleRasterRgba(const std::string& openSpec,
                                              int dstW, int dstH,
                                              const RasterRgbAssemble& as,
                                              int& outW, int& outH);

// 组装一个像素窗口细节块的 RGBA8(供细节块纹理)。读窗口并重采样到 dw×dh。
// mode/b1/b2/b3/mn/mx 与 assembleRasterRgba 同语义(与底图共用拉伸, 保证叠加不跳变)。
// outW/outH 输出 dw×dh。失败返回空。
std::vector<unsigned char> assembleRasterWindowRgba(const std::string& openSpec,
                                                    const RasterRgbAssemble& as,
                                                    int x0, int y0, int w0, int h0,
                                                    int dw, int dh,
                                                    int& outW, int& outH);

}  // namespace peekg::data