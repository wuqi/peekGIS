#pragma once
#include <string>
#include <vector>
#include "data/vector_reader.h"
#include "data/raster_reader.h"

using peekg::data::VectorData;
using peekg::data::RasterData;

struct LayerInfo {
    std::string name;
    std::string sourceCrs;   // 文本形式, 如 "EPSG:4326" 或 "unknown"
    long long featureCount = 0;
    bool visible = true;
};

// 栅格图层渲染模式
enum class RasterRenderMode { StretchGray, Pseudocolor, RGB };

// 单栅格图层渲染选项(存于 MapLayer, 供调整界面与渲染使用)
struct RasterRenderOptions {
    RasterRenderMode mode = RasterRenderMode::StretchGray;
    int grayBand = 1;                 // 拉伸/伪彩色用波段(1-based)
    int rBand = 1, gBand = 2, bBand = 3;  // RGB 三波段(可重复)
    bool useAutoMinMax = true;        // 自动用数据集 min/max
    double minRaw = 0, maxRaw = 0;    // 手动拉伸范围
    double gamma = 1.0;
    int colorMap = 0;                 // 伪彩色色带预设索引
    bool clipNoData = false;
};

// 图层种类
enum class LayerKind { Vector, Raster };

// 一个已加载的图层: 元信息 + 源-CRS 几何(用于重投影)
struct MapLayer {
    LayerInfo info;
    LayerKind kind = LayerKind::Vector;
    VectorData data;          // 矢量: 源-CRS 顶点 + srcEpsg + 范围
    RasterData raster;        // 栅格: 元数据 + 波段(实际像素由 backend 纹理持有)
    RasterRenderOptions rastOpts;   // 栅格渲染选项
    int rasterHandle = -1;    // backend.rasters 索引(与 layers 顺序无关, 删除/渲染精确对应)
    std::string sourcePath;   // 来源文件路径(用于同文件去重/重开替换)
    int sourceLayerIdx = 0;   // 源文件中的图层索引(属性识别用)
    float color[4] = {0.3f, 0.8f, 0.9f, 0.35f}; // RGBA 渲染色; 第4分量=面填充不透明度

    // 流式加载期间 staging 展示坐标的一致性跟踪(仅加载器主线程消费期使用)
    int stagingKey = -1;       // staging 已用的坐标键: 0=原始坐标, >0=重投影到的显示CRS, -1=尚无块
    int openSeq = 0;           // 打开顺序(App 递增): 自动定位只允许"最新打开"的图层抢镜头
    bool stagingMixed = false; // staging 是否混用了多个不同坐标键(应改为从源数据重建)
    bool cpuOnlyStaging = false; // 缓存命中整层单块: 几何只在 L.data(源CRS), backend 无 GPU staging
    bool cacheBucketInit = false; // 缓存块桶层(cacheChunk): 已建块桶容器并摄入整层 meta/范围
    bool bakeCached = false;      // 该层烘焙图来自磁盘缓存(本轮未重新烘焙)
    bool bake = false;            // 该层是否走烘焙(大数据); false=走原矢量路径(小数据)
    int bakeMaxLevel = 0;         // 栅格金字塔最深层(自动算出)
    double bakeMinx = 0, bakeMiny = 0, bakeMaxx = 0, bakeMaxy = 0;  // 烘焙层显示坐标 bbox
};

struct ViewState {
    double centerX = 0, centerY = 0;  // 显示CRS下的视图中心
    double scale = 1.0;               // 每像素对应的世界单位(显示CRS)
    int vpW = 1, vpH = 1;             // 地图视口像素尺寸
};

class MapScene {
public:
    ViewState view;
    std::vector<MapLayer> layers;     // 所有已加载图层(累积)
    int displayEpsg = 0;              // 当前显示 CRS(0=未指定)
    bool needRefit = true;           // 请求重新适配视图

    double bboxMinX = 0, bboxMinY = 0, bboxMaxX = 0, bboxMaxY = 0;
    bool hasExtent = false;

    void addLayer(const VectorData& vd);   // 追加图层并合并范围
    void addRasterLayer(const RasterData& rd); // 追加栅格图层并合并范围
    void removeLayer(int idx);             // 移除指定图层并重建范围
    void expandExtent(double minx, double miny, double maxx, double maxy);  // 增量合并范围
    void clearLayers();
    void setExtent(double minx, double miny, double maxx, double maxy);
    void fitToView(int w, int h);
    void zoomToLayer(int idx);   // 右键菜单: 适配到指定图层范围
    void pan(double dxPixels, double dyPixels);
    void zoomAt(double factor, double mx, double my);  // mx,my: 相对地图像素
    void screenToWorld(double sx, double sy, double& wx, double& wy) const;
};
