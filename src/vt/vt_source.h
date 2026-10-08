#pragma once
// v2 要素环流: 用 GDAL/OGR 顺序读源图层, 逐环回调(点/线/面), 可选重投影到显示 CRS。
// 不复用 AsyncLoader(那条线输出已耳切三角形, 与瓦片环模型不同)。
#include "vt/vt_types.h"

#include <atomic>
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

// 探测预算/取消: 长耗时的打开阶段动作(全表 COUNT、顺序采样)必须能被叫停。
// budgetSec <= 0 表示不限时; cancel 非空且被置位则立刻返回。
struct ProbeBudget {
    double budgetSec = 0.0;
    const std::atomic<bool>* cancel = nullptr;
    bool expired(const void* startClock) const;   // 内部用, 供实现文件
    bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }
};

// 读图层元数据(不读几何)。失败返回 false。
// withCount=false 跳过要素计数(大表 COUNT(*) 冷缓存可达十秒级; 只要 epsg/范围时别数)。
// withCount=true 时若带预算且超预算, 返回 true 但 featureCount 置 -1(=未知), 不硬等。
// 结果按 (path, layerIdx) 会话级缓存: 同一源只 COUNT 一次, 后续调用直接返回。
bool readVtLayerInfo(const std::string& path, int layerIdx, LayerInfo& out, bool withCount = true,
                     const ProbeBudget* budget = nullptr);

// 源环(显示 CRS 坐标, double; xy 交替)
struct SourceRing {
    uint8_t  type = RING_LINE;
    uint8_t  hole = 0;
    uint32_t polyGroup = 0;
    long long featureIdx = 0;   // 所属要素序号(进度用)
    std::vector<double> xy;
};

// 顺序读取要素并逐环回调。dstEpsg>0 且与源不同则重投影。返回处理要素数; <0 打开失败。
// 带预算时超预算/被取消会提前收尾并返回已处理数(>=0), 调用方据此判断"只估了部分"。
long long streamVtRings(const std::string& path, int layerIdx, int dstEpsg,
                        const std::function<void(const SourceRing&)>& sink,
                        const ProbeBudget* budget = nullptr);

// 快速估算源顶点总数: 顺序取前 sampleK 个要素的平均点数 × 要素数。
// 用于 v1.0/v2 路由; 按存储顺序取样可能低估(空间自相关), 仅作数量级判据。失败返回 -1。
// 带预算时只按已采样部分给估算值(标记 *outPartial=true), 不再硬读满 sampleK。
long long estimateSourceVerts(const std::string& path, int layerIdx, long long sampleK = 50000,
                              const ProbeBudget* budget = nullptr, bool* outPartial = nullptr);

}  // namespace peekg::vt
