#include "map/map_scene.h"
#include "data/reproject.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>

using namespace peekg::data;

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

void MapScene::addRasterLayer(const RasterData& rd) {
    MapLayer l;
    l.kind = LayerKind::Raster;
    l.info.name = rd.name;
    l.info.sourceCrs = rd.sourceCrs;
    l.info.visible = true;
    l.raster = rd;
    // 默认渲染模式按波段数/类型
    if (rd.bands.empty()) {
        l.rastOpts.mode = RasterRenderMode::StretchGray;
    } else if (rd.bandCount >= 3) {
        l.rastOpts.mode = RasterRenderMode::RGB;
    } else {
        l.rastOpts.mode = RasterRenderMode::StretchGray;
    }
    layers.push_back(std::move(l));
    // 栅格范围不在此处并入全局 bbox(等 reprojectRasterExtent 后用 displayCRS 范围并入)
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
        if (l.kind == LayerKind::Raster) {
            const RasterData& rd = l.raster;
            if (rd.minx == 0 && rd.miny == 0 && rd.maxx == 0 && rd.maxy == 0) continue;
            mnx = std::min(mnx, rd.minx); mny = std::min(mny, rd.miny);
            mxx = std::max(mxx, rd.maxx); mxy = std::max(mxy, rd.maxy);
            any = true;
            continue;
        }
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

void MapScene::fitToView(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!hasExtent) {
        // 无数据: 显示参考网格范围
        bboxMinX = -10; bboxMinY = -10; bboxMaxX = 10; bboxMaxY = 10;
    }
    view.vpW = w; view.vpH = h;
    double wWorld = bboxMaxX - bboxMinX;
    double hWorld = bboxMaxY - bboxMinY;
    static const bool dbgFit = getenv("PEEK_DEBUG_VEC") != nullptr;
    if (dbgFit) {
        spdlog::info("[FIT] hasExtent={} vp={}x{} bbox=({:.5f},{:.5f},{:.5f},{:.5f})",
                     (int)hasExtent, w, h, bboxMinX, bboxMinY, bboxMaxX, bboxMaxY);
    }
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
    const MapLayer& l = layers[idx];
    if (getenv("PEEK_DEBUG_VEC")) {
        spdlog::info("[ZTL] idx={} name={} kind={} srcEpsg={} disp={}",
                     idx, l.info.name, (l.kind == LayerKind::Raster ? "R" : "V"),
                     (l.kind == LayerKind::Raster ? l.raster.srcEpsg : l.data.srcEpsg),
                     displayEpsg);
    }
    if (l.kind == LayerKind::Raster) {
        const RasterData& rd = l.raster;
        if (rd.hasDispExtent) {
            bboxMinX = rd.dispMinx; bboxMinY = rd.dispMiny;
            bboxMaxX = rd.dispMaxx; bboxMaxY = rd.dispMaxy;
        } else {
            if (rd.minx == 0 && rd.miny == 0 && rd.maxx == 0 && rd.maxy == 0) return;
            bboxMinX = rd.minx; bboxMinY = rd.miny;
            bboxMaxX = rd.maxx; bboxMaxY = rd.maxy;
        }
        hasExtent = true;
        fitToView(view.vpW, view.vpH);
        return;
    }
    const VectorData& vd = l.data;
    if (vd.minx > vd.maxx) return;
    double mnx = vd.minx, mny = vd.miny, mxx = vd.maxx, mxy = vd.maxy;
    // 动态投影: 源CRS范围要先转到显示CRS, 否则会把视口跳到源坐标位置(图层"飞不见")
    if (displayEpsg != 0 && vd.srcEpsg != 0 && vd.srcEpsg != displayEpsg) {
        const double cx[4] = {vd.minx, vd.maxx, vd.minx, vd.maxx};
        const double cy[4] = {vd.miny, vd.miny, vd.maxy, vd.maxy};
        double rx[4], ry[4];
        bool ok = true;
        for (int i = 0; i < 4; i++)
            if (!reprojectPoint(cx[i], cy[i], vd.srcEpsg, displayEpsg, rx[i], ry[i])) { ok = false; break; }
        if (ok) {
            mnx = *std::min_element(rx, rx + 4);
            mxx = *std::max_element(rx, rx + 4);
            mny = *std::min_element(ry, ry + 4);
            mxy = *std::max_element(ry, ry + 4);
        } else {
            // 重投影失败: 不移动镜头(避免把视口拽到按源坐标硬塞的"远方"位置)
            return;
        }
    }
    bboxMinX = mnx; bboxMinY = mny; bboxMaxX = mxx; bboxMaxY = mxy;
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
