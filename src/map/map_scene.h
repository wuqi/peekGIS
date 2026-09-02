#pragma once
#include <string>
#include <vector>
#include "data/gdal_datasource.h"

struct LayerInfo {
    std::string name;
    std::string sourceCrs;   // 文本形式, 如 "EPSG:4326" 或 "unknown"
    long long featureCount = 0;
    bool visible = true;
};

// 一个已加载的图层: 元信息 + 源-CRS 几何(用于重投影)
struct MapLayer {
    LayerInfo info;
    VectorData data;          // 源-CRS 顶点 + srcEpsg + 范围
    std::string sourcePath;   // 来源文件路径(用于同文件去重/重开替换)
    int sourceLayerIdx = 0;   // 源文件中的图层索引(属性识别用)
    float color[4] = {0.3f, 0.8f, 0.9f, 0.35f}; // RGBA 渲染色; 第4分量=面填充不透明度
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
