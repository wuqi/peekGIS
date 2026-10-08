#pragma once
// v2 构建管道: 阶段A(最深层裁剪/量化/去重/LRU 落盘) + 阶段B(4x4 合并到 L0)。
// 只服务渲染, 不维护拓扑。
#include "vt/vt_types.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace peekg::vt {

struct SourceRing;   // vt_source.h(直读流路由用它, 头文件不透出源)

struct VtBuildConfig {
    int layerIdx = 0;
    int dstEpsg = 0;            // 0 = 用源 EPSG
    // 最深层选层(见 pickVtLevel): errorFactor > 0 = 误差驱动(主策略);
    // = 0 = 退回旧的顶点数驱动(每瓦片顶点≈targetVerts)。
    // 选层判据: 最深层格距 ≈ 数据的典型结构尺度(相邻点间距 p50)。
    // errorFactor 是相对该尺度的粗细倍率: 1.0 = 贴住典型尺度(推荐);
    // >1 更浅省空间; <1 更深。0 = 退回旧的"每瓦片顶点数≈targetVerts"策略。
    double errorFactor = 1.0;
    int targetVerts = 2048;     // 每瓦片顶点数: 仅旧策略用(误差模式下不用单片上限)
    // 体积安全阀: 双条件取更浅的那档(见 pickVtLevel 注释)
    long long maxTotalVerts = 0;          // 保留层总顶点预算(落盘体积; 0=不限制)
    long long maxVertsPerTile = 0;         // 单片顶点数上限(渲染帧耗时; 0=不限制)
    int maxLevelCap = 12;
    // 最深层**上限**: -1 = 完全自动(按典型尺度 + 体积预算估算); >=0 = 照常自动估算,
    // 只是结果不许比它更深。选层是观感/性能权衡, 不同数据结论完全不同, 所以由自动估算定,
    // 配置只负责"不许超过我给的深度"。
    // (早期把 >=0 当成"直接钉死", 于是所有数据都被拉到同一层, 小数据集白白多建十几倍:
    //  tl_2025_06_tabblock20 因此从 30MB 涨到 178MB。)
    int levels = -1;
    // 强制精确层数: >=0 时完全跳过估算, 就建这一层。只给测试和"我就是要这个深度"的
    // 特殊场景用 —— 它不参与体积安全阀。与 levels(上限) 是两件不同的事。
    int forceLevel = -1;
    double lruVerts = 1e8;      // LRU 上限(顶点数), ~8B/顶点 -> 1e8 约 750MB; 勿超 1.2e8(约1GB)
    bool simplify = true;       // 各层按格距做近共线抽稀(显著降低粗层体积)
    double simplifyFactor = 1.0;  // 抽稀容差 = 该层格距 * factor
    double minFeatureCells = 1.0; // 线: 源 bbox 对角 < 该值(格)则在该层丢弃
    int levelStep = 2;          // 隔层构建: 每 step 层保留一层(从 L0 起)+最深层; 1=每层都建
    bool verbose = false;
};

struct VtBuildStats {
    long long features = 0;
    long long rings = 0;
    long long srcVerts = 0;
    long long tilesWritten = 0;
    long long storedVerts = 0;
    uint64_t dataBytes = 0;
    int maxLevel = 0;
    double seconds = 0;
};

// 建缓存: srcPath 读, cachePath 写。失败返回 false。
// onProgress: 0..100(阶段A 0..50, 阶段B 50..100)。
// onCover(level,tx,ty): 构建线程每首次触及某瓦片时回调(用于实时显示进度覆盖框, 与落盘无关)。
// onMerge(level,tx,ty): 拓扑后处理(小面并入邻面 + 抽稀, 即"合并/糊化")每完成一片时回调,
//                        用于让覆盖框区分"已建"与"已合并"两种状态(不同颜色)。
bool buildVtCache(const std::string& srcPath, int layerIdx, const std::string& cachePath,
                  const VtBuildConfig& cfg, VtBuildStats& stats,
                  const std::function<void(int, int, int)>& onTile = nullptr,
                  const std::function<void(int)>& onProgress = nullptr,
                  const std::function<void(int, int, int)>& onCover = nullptr,
                  const std::function<void(int, int, int)>& onMerge = nullptr);

// 选层结果(诊断/日志用; 决定性字段只有 level)。errorMode=false 表示
// 目标格距不可用(源无几何/采样不足)而退回顶点数驱动。
struct VtLevelPick {
    int level = 0;
    double nativeStep = 0;        // 源相邻点间距 p50 分位数(= 选层用的典型结构尺度)
    double keepRatio = 0;         // 该层作为最深层时保留的源顶点比例(仅日志/诊断)
    double srcVerts = 0;          // 源顶点总数(估计)
    double spanUsed = 0;          // 稳健跨度(仅供诊断日志)
    double cellAt = 0;            // 选中层的实际格距
    double vertsPerTile = 0;      // 选中层估计每瓦片顶点数(含校准)
    double totalVerts = 0;        // 保留层总存储顶点估计(含校准)
    bool clampedByVerts = false;  // 是否被体积安全阀从更深层压上来
    bool errorMode = false;
};

// 估算最深层。errorFactor > 0 时按「典型结构尺度」选: 让最深层(1024 细格)的格距
// ≈ 源相邻点间距的 p50 / errorFactor。errorFactor = 0 时退化为旧的顶点数驱动
// (|P(L)/4^L − targetVerts| 最小)。失败返回 level<0。
// maxTotalVerts: 保留层总顶点预算; maxVertsPerTile: 单片顶点数上限(两者取更浅);
// levelStep: 隔层构建步长(决定哪些层计入)。
// 想要限制最深层请用 VtBuildConfig::levels —— 它是**上限**: 估算照常跑, 只是结果不许比它更深。
// (早期把 levels 当成"强制层数", 会让所有数据都建到同一层, 白白多出十几倍体积。)
VtLevelPick pickVtLevel(const std::string& srcPath, int layerIdx, int dstEpsg,
                        double errorFactor, int targetVerts, int cap,
                        long long maxTotalVerts, long long maxVertsPerTile, int levelStep);

// 兼容入口: 旧顶点数策略(等价 pickVtLevel(..., errorFactor=0, ...).level)
int estimateMaxLevel(const std::string& srcPath, int layerIdx, int dstEpsg,
                     int targetVerts, int cap);

// ---- 原始数据直读流(超 Lmax 时用) ----
// 打开源文件, 对"可见区域(源CRS)"设 OGR 空间过滤(有空间索引如 ship .qix 时只顺序读命中要素),
// 分块读取并把几何路由到指定直读层(level>maxLevel)的内存瓦片, 不写缓存、不做抽稀。
// scanned/featureCount/EWMA 均按"可见区命中子集"度量 —— 子集小则读盘快, 深层直读才成立;
// chunk() 限制每块要素数/毫秒, 渲染端据此实测效率决定是否继续/回退(避免无索引源整表扫万)。
// OGR 类型以 void* 存, 避免把头文件拖入 gdal 依赖(头文件保持纯类型 + 少量 std)。
class RawRegionStream {
public:
    RawRegionStream();
    ~RawRegionStream();
    RawRegionStream(const RawRegionStream&) = delete;
    RawRegionStream& operator=(const RawRegionStream&) = delete;

    // srcPath/layerIdx: 源; dstEpsg: 显示 CRS(路由/瓦片 epsg 用; 0 = 源 CRS);
    // level: 路由到的直读层(>maxLevel); maxLevel: 缓存最深层(决定 tileSizeAt 的 1024 细格);
    // originX/originY/S: 全局网格原点与 L0 边长(与缓存同源, 保证与缓存片对齐);
    // rx0..ry1: 可见区域(世界坐标, dst CRS)。打开即测 featureCount(命中子集)。失败返回 false。
    bool open(const std::string& srcPath, int layerIdx, int dstEpsg,
              int level, int maxLevel, double originX, double originY, double S,
              double rx0, double ry0, double rx1, double ry1);

    // 读一块: 最多 maxFeatures 个要素或到 EOF, 并尽量不超过 maxMs 毫秒。输出本块扫描要素数/耗时(ms);
    // done=true 表示本遍已读尽(区域一次读完)。内部读出即路由到瓦片。
    void chunk(long long maxFeatures, double maxMs,
               long long& scanned, double& ms, bool& done);

    // 取走全部累积瓦片(内部缓冲清空)。key = tileKey(level, tx, ty)。
    void takeTiles(std::vector<std::pair<uint64_t, VtTile>>& out);

    long long featureCount() const { return featureCount_; }
    bool isOpen() const { return ds_ != nullptr; }
    void close();

private:
    void addFeatureGeom(void* g, uint32_t& polyCounter, long long featureIdx);
    void routeRing(const SourceRing& sr, double cell);

    void* ds_ = nullptr;       // GDALDatasetH
    void* lyr_ = nullptr;      // OGRLayerH
    void* ct_ = nullptr;       // OGRCoordinateTransformationH (src->dstEpsg)
    void* geosCtx_ = nullptr;  // GEOSContextHandle_t (面裁剪用)
    long long featureCount_ = 0;
    std::unordered_map<uint64_t, VtTile> tiles_;
    int level_ = 0, maxLevel_ = 0;
    double originX_ = 0, originY_ = 0, S_ = 1;
    double cell_ = 1, tileW_ = 1;
    int epsg_ = 0;
    // 可见区域 -> 源 CRS 坐标(整要素快速跳过用; 未投影时为 0 标志)
    double sbx0_ = 0, sby0_ = 0, sbx1_ = 0, sby1_ = 0;
    // 可见区域瓦片下标范围(路由只输出可见区, 避免在深层巨大网格里到处建片)。tx1<tx0 = 空
    int vtx0_ = 0, vty0_ = 0, vtx1_ = -1, vty1_ = -1;
};

}  // namespace peekg::vt
