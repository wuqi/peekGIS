#pragma once
// v2 构建管道: 阶段A(最深层裁剪/量化/去重/LRU 落盘) + 阶段B(4x4 合并到 L0)。
// 只服务渲染, 不维护拓扑。
#include "vt/vt_types.h"

#include <functional>
#include <string>

namespace peekg::vt {

struct VtBuildConfig {
    int layerIdx = 0;
    int dstEpsg = 0;            // 0 = 用源 EPSG
    int targetVerts = 2048;     // 目标每瓦片顶点数(用于估算最深层)
    int maxLevelCap = 12;
    int levels = -1;            // >=0 强制最深层; -1 自动估算
    double lruVerts = 1e8;      // LRU 上限(顶点数), ~8B/顶点 -> 1e8 约 750MB; 勿超 1.2e8(约1GB)
    bool simplify = true;       // 各层按格距做近共线抽稀(显著降低粗层体积)
    double simplifyFactor = 1.0;  // 抽稀容差 = 该层格距 * factor
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
bool buildVtCache(const std::string& srcPath, int layerIdx, const std::string& cachePath,
                  const VtBuildConfig& cfg, VtBuildStats& stats,
                  const std::function<void(int, int, int)>& onTile = nullptr,
                  const std::function<void(int)>& onProgress = nullptr);

// 估算最深层(顺序步进采样 + P(L)/4^L 最接近 target)。失败返回 -1。
int estimateMaxLevel(const std::string& srcPath, int layerIdx, int dstEpsg,
                     int targetVerts, int cap);

}  // namespace peekg::vt
