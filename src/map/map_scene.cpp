#include "map/map_scene.h"
#include <cmath>
#include <algorithm>

void MapScene::addLayer(const VectorData& vd) {
    MapLayer l;
    l.info.name = vd.name;
    l.info.sourceCrs = vd.sourceCrs;
    l.info.featureCount = vd.featureCount;
    l.info.visible = true;
    l.data = vd;
    layers.push_back(std::move(l));

    // 仅当有几何时才并入范围; 空图层(无几何)的哨兵/0范围会撑大 bbox 使要素缩成一点
    if (!vd.vertices.empty() || !vd.points.empty()) {
        if (!hasExtent) {
            bboxMinX = vd.minx; bboxMinY = vd.miny;
            bboxMaxX = vd.maxx; bboxMaxY = vd.maxy;
            hasExtent = true;
        } else {
            bboxMinX = std::min(bboxMinX, vd.minx);
            bboxMinY = std::min(bboxMinY, vd.miny);
            bboxMaxX = std::max(bboxMaxX, vd.maxx);
            bboxMaxY = std::max(bboxMaxY, vd.maxy);
        }
    }
    needRefit = true;
}

void MapScene::expandExtent(double minx, double miny, double maxx, double maxy) {
    if (!hasExtent) {
        bboxMinX = minx; bboxMinY = miny;
        bboxMaxX = maxx; bboxMaxY = maxy;
        hasExtent = true;
    } else {
        bboxMinX = std::min(bboxMinX, minx);
        bboxMinY = std::min(bboxMinY, miny);
        bboxMaxX = std::max(bboxMaxX, maxx);
        bboxMaxY = std::max(bboxMaxY, maxy);
    }
    needRefit = true;
}

void MapScene::clearLayers() {
    layers.clear();
    hasExtent = false;
    needRefit = true;
}

void MapScene::removeLayer(int idx) {
    if (idx < 0 || idx >= (int)layers.size()) return;
    layers.erase(layers.begin() + idx);
    if (layers.empty()) { hasExtent = false; needRefit = true; return; }

    // 用剩余图层(源CRS)的有效范围重建 bbox; 保持当前视图不跳变
    double mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300;
    bool any = false;
    for (const auto& l : layers) {
        const VectorData& vd = l.data;
        if (vd.minx > vd.maxx) continue;
        mnx = std::min(mnx, vd.minx); mny = std::min(mny, vd.miny);
        mxx = std::max(mxx, vd.maxx); mxy = std::max(mxy, vd.maxy);
        any = true;
    }
    hasExtent = any;
    if (any) { bboxMinX = mnx; bboxMinY = mny; bboxMaxX = mxx; bboxMaxY = mxy; }
    needRefit = false;   // 卸载不自动缩放, 保持当前视野
}

void MapScene::setExtent(double minx, double miny, double maxx, double maxy) {
    bboxMinX = minx; bboxMinY = miny; bboxMaxX = maxx; bboxMaxY = maxy;
    hasExtent = true;
    needRefit = true;
}

void MapScene::fitToView(int w, int h) {    if (w <= 0 || h <= 0) return;
    if (!hasExtent) {
        // 无数据: 显示参考网格范围
        bboxMinX = -10; bboxMinY = -10; bboxMaxX = 10; bboxMaxY = 10;
    }
    view.vpW = w; view.vpH = h;
    double wWorld = bboxMaxX - bboxMinX;
    double hWorld = bboxMaxY - bboxMinY;
    if (wWorld <= 0) wWorld = 1;
    if (hWorld <= 0) hWorld = 1;
    double pad = 1.1; // 留边
    double sx = (wWorld * pad) / (double)w;
    double sy = (hWorld * pad) / (double)h;
    view.scale = std::max(sx, sy);
    view.centerX = (bboxMinX + bboxMaxX) * 0.5;
    view.centerY = (bboxMinY + bboxMaxY) * 0.5;
}

void MapScene::zoomToLayer(int idx) {
    if (idx < 0 || idx >= (int)layers.size()) return;
    const VectorData& vd = layers[idx].data;
    if (vd.minx > vd.maxx) return;  // 空/无效范围
    bboxMinX = vd.minx; bboxMinY = vd.miny;
    bboxMaxX = vd.maxx; bboxMaxY = vd.maxy;
    hasExtent = true;
    fitToView(view.vpW, view.vpH);
}

void MapScene::pan(double dxPixels, double dyPixels) {
    view.centerX -= dxPixels * view.scale;   // 屏幕x向右 => 世界中心左移
    view.centerY += dyPixels * view.scale;   // 屏幕y向下 => 世界中心上移
}

void MapScene::zoomAt(double factor, double mx, double my) {
    double wx, wy;
    screenToWorld(mx, my, wx, wy);
    view.scale *= factor;
    view.centerX = wx - (mx - view.vpW * 0.5) * view.scale;
    view.centerY = wy + (my - view.vpH * 0.5) * view.scale;
}

void MapScene::screenToWorld(double sx, double sy, double& wx, double& wy) const {
    wx = (sx - view.vpW * 0.5) * view.scale + view.centerX;
    wy = view.centerY - (sy - view.vpH * 0.5) * view.scale;
}
