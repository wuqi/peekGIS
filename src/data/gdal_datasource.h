#pragma once
#include <string>
#include <vector>
#include "data/encoding.h"

// 矢量数据集(已抽取为源 CRS 的线段顶点, 便于 VBO 直绘与动态重投影)
struct VectorData {
    std::string name;        // 图层名(文件名)
    std::string sourceCrs;   // 文本形式, 如 "EPSG:4326" 或 "unknown"
    int srcEpsg = 0;         // 源 EPSG; 0 表示未知
    long long featureCount = 0;
    double minx = 0, miny = 0, maxx = 0, maxy = 0;
    std::vector<float> vertices;  // 线段端点对(x,y,x,y,...), 源 CRS 坐标
    std::vector<float> points;    // 点坐标(x,y,x,y,...), 源 CRS 坐标
    std::vector<float> triangles; // 面填充三角形顶点(x,y,x,y,...), 源 CRS 坐标(半透明面用)
};

// 用 GDAL/OGR 打开矢量文件(shp/gdb/gpkg 等), 抽取线/面边界为源 CRS 线段顶点。
// 成功返回 true, 数据写入 out(仅第 0 层)。
bool loadVectorFile(const std::string& path, VectorData& out);

// 同上, 但加载所有图层(如 gpkg 多表), 依次写入 out。
bool loadVectorFileAll(const std::string& path, std::vector<VectorData>& out);

// 图层元数据(用于多图层选择对话框)
struct LayerMeta {
    std::string name;
    long long featureCount = 0;
    bool selected = true;     // 默认全选
};
// 快速读取图层元数据(不读几何, 仅 GDALOpen + 遍历图层名/要素数, 通常 <100ms)
bool readLayerMetadata(const std::string& path, std::vector<LayerMeta>& out);

// 单个属性的显示数据: 名称 + 值(UTF-8)。字符串字段额外保存原始字节以支持编码切换。
struct IdentifyAttr {
    std::vector<unsigned char> rawName;       // 字段名原始字节(未转码)
    std::string name;                         // 字段名(UTF-8, 已按编码转码)
    std::string value;                        // 字段值(UTF-8 显示, 字符串字段按所选编码转码)
    bool isString = false;                    // 是否字符串字段(需按编码转码)
    std::vector<unsigned char> raw;           // 字符串字段的原始字节(未转码); 非字符串为空
};

// 属性识别点击查询结果(每图层一个命中要素)
struct IdentifyHit {
    std::string layerName;                     // 图层名
    std::string geomType;                      // "POLYGON" / "LINESTRING" / "POINT" 等
    std::vector<IdentifyAttr> attrs;           // 字段名 -> 值(含原始字节, 支持编码切换)
    int srcEpsg = 0;                           // 几何所在源 CRS(高亮重投影用)
    std::vector<float> outline;                // 描边线段(x,y 交替, 源 CRS)
    std::vector<float> fillTris;               // 面填充三角形顶点(源 CRS, 无面时为空)
    std::vector<float> points;                 // 点坐标(源 CRS, 非点时为空)

    // 按所选编码重转所有字符串字段的 value(不重读文件)。失败的非字符串保持原样。
    void applyEncoding(TextEncoding enc);
    // 当前字符串值使用的编码(默认 UTF-8)
    TextEncoding encoding = TextEncoding::Utf8;
};

// 在 (dx, dy)(显示坐标) + 容差 dtol(显示单位) 范围内查询指定文件图层的要素属性并写入 out。
// displayEpsg==0 或与 srcEpsg 相同时直接使用坐标; 否则两点重投影换算容差。
// 命中(有要素距离 <= dtol)返回 true, 取距离最近要素的属性。
bool identifyFeatures(const std::string& path, int layerIdx,
                      double dx, double dy, double dtol,
                      int displayEpsg, int srcEpsg,
                      IdentifyHit& out);

// 解析单个 WKT 字符串(Point/LineString/Polygon/Multi*/GeometryCollection)为 VectorData。
// name 为图层名; srcCrs 形如 "EPSG:4326"; srcEpsg 为对应数值(>=0)。
// 解析失败或没有任何可绘制几何时返回 false。
bool loadWktToVectorData(const std::string& name, const std::string& wkt,
                         const std::string& srcCrs, int srcEpsg, VectorData& out);
