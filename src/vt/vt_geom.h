#pragma once
// 把一片 VtTile 转成显示 CRS 的 float 几何(供 GL 直绘): lines(线/面描边), points, fill(面填充三角形)。
// 与 GL 无关, 可单测。面按 polyGroup 聚合外环+孔后 earcut。
#include "vt/vt_types.h"
#include "earcut.hpp"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace peekg::vt {

// cell = 该瓦片所属层的格距(显示 CRS 单位); 顶点 = origin + int16 * cell。
inline void buildTileGeometry(const VtTile& t, double cell,
                              std::vector<float>& lines,
                              std::vector<float>& points,
                              std::vector<float>& fill) {
    auto X = [&](int16_t g) { return (float)(t.originX + (double)g * cell); };
    auto Y = [&](int16_t g) { return (float)(t.originY + (double)g * cell); };
    auto seg = [&](const int16_t* v, uint32_t n) {
        for (uint32_t i = 0; i + 1 < n; ++i) {
            lines.push_back(X(v[2*i]));   lines.push_back(Y(v[2*i+1]));
            lines.push_back(X(v[2*i+2])); lines.push_back(Y(v[2*i+3]));
        }
    };

    std::unordered_map<uint32_t, std::vector<const VtRing*>> groups;
    for (const VtRing& r : t.rings) {
        const int16_t* v = t.verts.data() + (size_t)r.firstVertex * 2;
        if (r.type == RING_POINT) {
            if (r.vertexCount >= 1) { points.push_back(X(v[0])); points.push_back(Y(v[1])); }
        } else if (r.type == RING_LINE) {
            seg(v, r.vertexCount);
        } else {   // FACE: 描边 + 收集到分组
            seg(v, r.vertexCount);
            groups[r.polyGroup].push_back(&r);
        }
    }

    for (auto& kv : groups) {
        const std::vector<const VtRing*>& rs = kv.second;
        const VtRing* outer = nullptr;
        std::vector<const VtRing*> holes;
        for (const VtRing* r : rs) {
            if (r->hole == 0 && !outer) outer = r;
            else holes.push_back(r);
        }
        if (!outer || outer->vertexCount < 3) continue;

        std::vector<std::vector<std::pair<float, float>>> rings;
        std::vector<float> flat;
        auto pushRing = [&](const VtRing* r) {
            const int16_t* v = t.verts.data() + (size_t)r->firstVertex * 2;
            std::vector<std::pair<float, float>> c;
            c.reserve(r->vertexCount);
            for (uint32_t i = 0; i < r->vertexCount; ++i) c.emplace_back(X(v[2*i]), Y(v[2*i+1]));
            if (c.size() >= 2 && c.front() == c.back()) c.pop_back();   // 去显式闭合点
            if (c.size() < 3) return;
            for (auto& p : c) { flat.push_back(p.first); flat.push_back(p.second); }
            rings.push_back(std::move(c));
        };
        pushRing(outer);
        for (const VtRing* h : holes) if (h->vertexCount >= 3) pushRing(h);
        if (rings.empty()) continue;

        std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(rings);
        idx.resize(idx.size() - idx.size() % 3);
        for (uint32_t i : idx) {
            size_t k = (size_t)i * 2;
            if (k + 1 < flat.size()) { fill.push_back(flat[k]); fill.push_back(flat[k+1]); }
        }
    }
}

}  // namespace peekg::vt
