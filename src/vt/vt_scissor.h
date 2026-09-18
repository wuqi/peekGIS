#pragma once
// 瓦片净区 -> 屏幕像素 scissor 矩形。纯函数(无 GL), 可单测。
// 关键: 相邻瓦片共用的那条边界世界坐标映射到同一像素值并同样取整 ->
// 严格共享边界像素(无缝、无重叠)。y 为 OpenGL 下原点(从下往上)。
#include <algorithm>
#include <cmath>

namespace peekg::vt {

struct ScissorRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// originX/originY: 瓦片净区左下角(显示CRS); cell: 格距; 净区固定 512 格。
// centerX/centerY/scale: 视图中心与每像素世界单位; texW/texH: 视口像素尺寸。
inline ScissorRect tileScissorRect(double originX, double originY, double cell,
                                   double centerX, double centerY, double scale,
                                   int texW, int texH, int tileSize = TILE_SIZE) {
    ScissorRect r;
    if (!(scale > 0) || texW <= 0 || texH <= 0) {
        r = {0, 0, texW, texH};
        return r;
    }
    double sx0 = (originX - centerX) / scale + texW * 0.5;
    double sx1 = (originX + (double)tileSize * cell - centerX) / scale + texW * 0.5;
    double sy0 = (originY - centerY) / scale + texH * 0.5;
    double sy1 = (originY + (double)tileSize * cell - centerY) / scale + texH * 0.5;
    long l = std::lround(sx0), rr = std::lround(sx1);
    long b = std::lround(sy0), t = std::lround(sy1);
    if (l < 0) l = 0;
    if (b < 0) b = 0;
    if (rr > texW) rr = texW;
    if (t > texH) t = texH;
    long w = rr - l, h = t - b;
    if (w < 1) w = 1;   // 亚像素瓦片(构建进度)也留 1px
    if (h < 1) h = 1;
    r.x = (int)l; r.y = (int)b; r.w = (int)w; r.h = (int)h;
    return r;
}

}  // namespace peekg::vt
