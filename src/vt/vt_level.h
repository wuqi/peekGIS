#pragma once
// 视口缩放 -> 选层 + 可见瓦片范围。纯函数(无 GL), 可单测。
// 选层目标: 每片约 512 屏幕像素; 只在已建层里选(fullyBuiltLevels)。
#include <algorithm>
#include <cmath>

namespace peekg::vt {

struct TileRange {
    int tx0 = 0, ty0 = 0, tx1 = -1, ty1 = -1;   // tx1<tx0 表示空
};

// 选最深层 L: tileW0/(512*2^L) ≈ scale; 限制在 [0,maxLevel] 且该层已建(bit 置位)。
inline int chooseVtLevel(double scale, double tileW0, int maxLevel, uint32_t builtMask) {
    if (!(scale > 0) || tileW0 <= 0 || maxLevel < 0) return 0;
    int Ld = (int)std::lround(std::log2(tileW0 / (512.0 * scale)));
    if (Ld < 0) Ld = 0;
    if (Ld > maxLevel) Ld = maxLevel;
    while (Ld > 0 && !(builtMask & (1u << Ld))) --Ld;
    return Ld;
}

// 纯期望层(不封顶到 maxLevel、不查已建 mask): 用于判断视口已超出缓存最深层。
// cap 为绝对上限(防 1<<L 越界)。渲染端据此进入"原始数据直读"模式。
inline int chooseVtLevelWanted(double scale, double tileW0, int cap) {
    if (!(scale > 0) || tileW0 <= 0 || cap < 0) return 0;
    int L = (int)std::lround(std::log2(tileW0 / (512.0 * scale)));
    if (L < 0) L = 0;
    if (L > cap) L = cap;
    return L;
}

// 视口(中心+每像素世界单位+像素尺寸)在 level 层命中的瓦片下标范围(闭区间, 已 clamp)。
inline TileRange visibleTileRange(double cx, double cy, double scale, int texW, int texH,
                                  double originX, double originY, double tileW0, int level) {
    TileRange r;
    if (!(scale > 0) || texW <= 0 || texH <= 0 || tileW0 <= 0 || level < 0) return r;
    int n = 1 << level;
    double tileW = tileW0 / (double)n;
    if (tileW <= 0) return r;
    double hw = scale * texW * 0.5, hh = scale * texH * 0.5;
    double vx0 = cx - hw, vx1 = cx + hw, vy0 = cy - hh, vy1 = cy + hh;
    // 视口与整层范围无交 -> 空
    if (vx1 < originX || vx0 > originX + tileW0 || vy1 < originY || vy0 > originY + tileW0)
        return r;
    int tx0 = (int)std::floor((vx0 - originX) / tileW);
    int tx1 = (int)std::floor((vx1 - originX) / tileW);
    int ty0 = (int)std::floor((vy0 - originY) / tileW);
    int ty1 = (int)std::floor((vy1 - originY) / tileW);
    r.tx0 = std::max(0, std::min(n - 1, tx0));
    r.tx1 = std::max(0, std::min(n - 1, tx1));
    r.ty0 = std::max(0, std::min(n - 1, ty0));
    r.ty1 = std::max(0, std::min(n - 1, ty1));
    return r;
}

}  // namespace peekg::vt
