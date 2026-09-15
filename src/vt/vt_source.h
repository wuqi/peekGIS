#pragma once
// v2 要素环流: 用 GDAL/OGR 顺序读源图层, 逐环回调(点/线/面), 可选重投影到显示 CRS。
// 不复用 AsyncLoader(那条线输出已耳切三角形, 与瓦片环模型不同)。
#include "vt/vt_types.h"

#include <functional>
#include <string>
#include <vector>

namespace peekg::vt {

struct LayerInfo {
    std::string name;
    long long featureCount = 0;
    int srcEpsg = 0;
    int geomType = 0;            // OGRwkbGeometryType
    bool hasExtent = false;
    double minx = 0, miny = 0, maxx = 0, maxy = 0;
};

// 读图层元数据(不读几何)。失败返回 false。
bool readVtLayerInfo(const std::string& path, int layerIdx, LayerInfo& out);

// 源环(显示 CRS 坐标, double; xy 交替)
struct SourceRing {
    uint8_t  type = RING_LINE;
    uint8_t  hole = 0;
    uint32_t polyGroup = 0;
    long long featureIdx = 0;   // 所属要素序号(进度用)
    std::vector<double> xy;
};

// 顺序读取要素并逐环回调。dstEpsg>0 且与源不同则重投影。返回处理要素数; <0 打开失败。
long long streamVtRings(const std::string& path, int layerIdx, int dstEpsg,
                        const std::function<void(const SourceRing&)>& sink);

// 快速估算源顶点总数: 顺序取前 sampleK 个要素的平均点数 × 要素数。
// 用于 v1.0/v2 路由; 按存储顺序取样可能低估(空间自相关), 仅作数量级判据。失败返回 -1。
long long estimateSourceVerts(const std::string& path, int layerIdx, long long sampleK = 50000);

}  // namespace peekg::vt
