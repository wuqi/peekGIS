#pragma once
// 把一片 VtTile 转成显示 CRS 的 float 几何(供 GL 直绘): lines(线/面描边), points, fill(面填充三角形)。
// 与 GL 无关, 可单测。
// 关键: 同一 polyGroup 可能有多个"外环"(跨瓦片被切成多段) —— 每个外环各自成一个面,
// 孔只并入包含它的外环; 否则会把别的外环当成孔, 挖出一堆洞。
#include "vt/vt_types.h"
#include "earcut.hpp"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace peekg::vt {

// 点是否在环内(射线法)
inline bool pointInRing(const std::pair<float, float>& p,
                        const std::vector<std::pair<float, float>>& r) {
    bool in = false;
    size_t n = r.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        float xi = r[i].first, yi = r[i].second;
        float xj = r[j].first, yj = r[j].second;
        bool inter = ((yi > p.second) != (yj > p.second)) &&
                     (p.first < (xj - xi) * (p.second - yi) / (yj - yi) + xi);
        if (inter) in = !in;
    }
    return in;
}

// strokeFaces: 是否给面描边。minFillCells: 面积小于该值(格²)的面只描边不填充(亚像素省 earcut)。
inline void buildTileGeometry(const VtTile& t, double cell, bool strokeFaces,
                              std::vector<float>& lines,
                              std::vector<float>& points,
                              std::vector<float>& fill,
                              double minFillCells = 0) {
    auto X = [&](int16_t g) { return (float)(t.originX + (double)g * cell); };
    auto Y = [&](int16_t g) { return (float)(t.originY + (double)g * cell); };
    auto seg = [&](const int16_t* v, uint32_t n) {
        for (uint32_t i = 0; i + 1 < n; ++i) {
            lines.push_back(X(v[2*i]));   lines.push_back(Y(v[2*i+1]));
            lines.push_back(X(v[2*i+2])); lines.push_back(Y(v[2*i+3]));
        }
    };
    auto ringCoords = [&](const VtRing* r, std::vector<std::pair<float, float>>& c) {
        const int16_t* v = t.verts.data() + (size_t)r->firstVertex * 2;
        c.clear();
        c.reserve(r->vertexCount);
        for (uint32_t i = 0; i < r->vertexCount; ++i) c.emplace_back(X(v[2*i]), Y(v[2*i+1]));
        if (c.size() >= 2 && c.front() == c.back()) c.pop_back();   // 去显式闭合点
    };

    std::unordered_map<uint32_t, std::vector<const VtRing*>> groups;
    for (const VtRing& r : t.rings) {
        const int16_t* v = t.verts.data() + (size_t)r.firstVertex * 2;
        if (r.type == RING_POINT) {
            if (r.vertexCount >= 1) { points.push_back(X(v[0])); points.push_back(Y(v[1])); }
        } else if (r.type == RING_LINE) {
            seg(v, r.vertexCount);
        } else {   // FACE
            if (strokeFaces) seg(v, r.vertexCount);
            groups[r.polyGroup].push_back(&r);
        }
    }

    for (auto& kv : groups) {
        std::vector<const VtRing*> outers, holes;
        for (const VtRing* r : kv.second) {
            if (r->vertexCount < 3) continue;
            if (r->hole == 0) outers.push_back(r); else holes.push_back(r);
        }
        for (const VtRing* outer : outers) {
            std::vector<std::pair<float, float>> oc;
            ringCoords(outer, oc);
            if (oc.size() < 3) continue;
            if (minFillCells > 0) {   // 亚像素小面: 只描边不填充
                long long a2 = 0;
                const int16_t* gv = t.verts.data() + (size_t)outer->firstVertex * 2;
                for (uint32_t i = 0; i < outer->vertexCount; ++i) {
                    uint32_t j = (i + 1) % outer->vertexCount;
                    a2 += (long long)gv[2*i] * gv[2*j+1] - (long long)gv[2*j] * gv[2*i+1];
                }
                if (a2 < 0) a2 = -a2;
                if ((double)a2 < 2.0 * minFillCells) continue;
            }

            std::vector<std::vector<std::pair<float, float>>> rings;
            std::vector<float> flat;
            auto push = [&](const std::vector<std::pair<float, float>>& c) {
                for (auto& p : c) { flat.push_back(p.first); flat.push_back(p.second); }
                rings.push_back(c);   // 拷贝(oc 后面还要用于孔包含判定)
            };
            push(oc);
            // 孔只并入包含它的外环(点代表测试)
            for (const VtRing* h : holes) {
                std::vector<std::pair<float, float>> hc;
                ringCoords(h, hc);
                if (hc.size() < 3) continue;
                if (pointInRing(hc[0], oc)) push(hc);
            }
            std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(rings);
            idx.resize(idx.size() - idx.size() % 3);
            for (uint32_t i : idx) {
                size_t k = (size_t)i * 2;
                if (k + 1 < flat.size()) { fill.push_back(flat[k]); fill.push_back(flat[k + 1]); }
            }
        }
    }
}

}  // namespace peekg::vt
