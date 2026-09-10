#pragma once
#include <ogr_api.h>
#include <vector>
#include <utility>
#include "earcut.hpp"   // mapbox::earcut 单头文件(thirdparty/earcut)

// 递归抽取线/面边界为源 CRS 线段顶点(x,y 交替); pts 非空时同时收集点(供 GL_POINTS 绘制)
namespace peekg::data {
inline void addGeometry(OGRGeometryH g, std::vector<float>& v, std::vector<float>* pts = nullptr) {
    if (!g) return;
    OGRwkbGeometryType t = wkbFlatten(OGR_G_GetGeometryType(g));
    switch (t) {
        case wkbPoint: {
            if (pts) { pts->push_back((float)OGR_G_GetX(g, 0)); pts->push_back((float)OGR_G_GetY(g, 0)); }
            break;
        }
        case wkbMultiPoint: {
            int np = OGR_G_GetGeometryCount(g);
            for (int i = 0; i < np; i++) addGeometry(OGR_G_GetGeometryRef(g, i), v, pts);
            break;
        }
        case wkbLineString:
        case wkbLinearRing: {
            int n = OGR_G_GetPointCount(g);
            for (int i = 0; i + 1 < n; i++) {
                v.push_back((float)OGR_G_GetX(g, i));
                v.push_back((float)OGR_G_GetY(g, i));
                v.push_back((float)OGR_G_GetX(g, i + 1));
                v.push_back((float)OGR_G_GetY(g, i + 1));
            }
            break;
        }
        case wkbPolygon: {
            int nr = OGR_G_GetGeometryCount(g);
            for (int r = 0; r < nr; r++) addGeometry(OGR_G_GetGeometryRef(g, r), v, pts);
            break;
        }
        case wkbMultiLineString:
        case wkbMultiPolygon:
        case wkbGeometryCollection: {
            int ng = OGR_G_GetGeometryCount(g);
            for (int i = 0; i < ng; i++) addGeometry(OGR_G_GetGeometryRef(g, i), v, pts);
            break;
        }
        default:
            break;  // 点/Curve 等类型暂不绘制
    }
}

// 抽一环的顶点坐标(去掉闭合重复点), 供三角剖分
inline void collectRingCoords(OGRGeometryH ring, std::vector<std::pair<float, float>>& out) {
    int n = OGR_G_GetPointCount(ring);
    if (n < 3) return;
    bool closed = OGR_G_GetX(ring, 0) == OGR_G_GetX(ring, n - 1) &&
                  OGR_G_GetY(ring, 0) == OGR_G_GetY(ring, n - 1);
    int m = closed ? n - 1 : n;
    for (int i = 0; i < m; i++)
        out.emplace_back((float)OGR_G_GetX(ring, i), (float)OGR_G_GetY(ring, i));
}

// 对一个 Polygon(含洞)做 ear-clip 剖分, 三角形顶点(x,y 交替)追加到 tris(源 CRS)
inline void tessellatePolygon(OGRGeometryH pg, std::vector<float>& tris) {
    int nr = OGR_G_GetGeometryCount(pg);
    std::vector<std::vector<std::pair<float, float>>> rings;  // rings[0]=外环, 其余=洞
    std::vector<float> flat;
    for (int r = 0; r < nr; r++) {
        std::vector<std::pair<float, float>> rc;
        collectRingCoords(OGR_G_GetGeometryRef(pg, r), rc);
        if (rc.size() < 3) continue;
        rings.push_back(std::move(rc));
    }
    if (rings.empty()) return;
    for (auto& rc : rings)
        for (auto& p : rc) { flat.push_back(p.first); flat.push_back(p.second); }
    std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(rings);
    if (idx.empty()) return;
    idx.resize(idx.size() - idx.size() % 3);   // 冗余/异常时对齐到整三角形
    tris.reserve(tris.size() + idx.size() * 2);
    for (uint32_t i : idx) {
        size_t k = (size_t)i * 2;
        if (k + 1 < flat.size()) { tris.push_back(flat[k]); tris.push_back(flat[k + 1]); }
    }
}

// 同 addGeometry, 额外把面几何三角化为填充顶点:
//   line=线/面边界线段(描边), tris=面填充三角形(半透明填充), pts=点(非空时收集)
inline void addFilledGeometry(OGRGeometryH g, std::vector<float>& line, std::vector<float>& tris,
                              std::vector<float>* pts = nullptr) {
    if (!g) return;
    OGRwkbGeometryType t = wkbFlatten(OGR_G_GetGeometryType(g));
    switch (t) {
        case wkbPoint: {
            if (pts) { pts->push_back((float)OGR_G_GetX(g, 0)); pts->push_back((float)OGR_G_GetY(g, 0)); }
            break;
        }
        case wkbMultiPoint: {
            int np = OGR_G_GetGeometryCount(g);
            for (int i = 0; i < np; i++) addFilledGeometry(OGR_G_GetGeometryRef(g, i), line, tris, pts);
            break;
        }
        case wkbLineString:
        case wkbLinearRing: {
            int n = OGR_G_GetPointCount(g);
            for (int i = 0; i + 1 < n; i++) {
                line.push_back((float)OGR_G_GetX(g, i));
                line.push_back((float)OGR_G_GetY(g, i));
                line.push_back((float)OGR_G_GetX(g, i + 1));
                line.push_back((float)OGR_G_GetY(g, i + 1));
            }
            break;
        }
        case wkbPolygon: {
            int nr = OGR_G_GetGeometryCount(g);
            for (int r = 0; r < nr; r++)
                addFilledGeometry(OGR_G_GetGeometryRef(g, r), line, tris, pts);   // 描边
            tessellatePolygon(g, tris);                                           // 填充
            break;
        }
        case wkbMultiLineString:
        case wkbMultiPolygon:
        case wkbGeometryCollection: {
            int ng = OGR_G_GetGeometryCount(g);
            for (int i = 0; i < ng; i++) addFilledGeometry(OGR_G_GetGeometryRef(g, i), line, tris, pts);
            break;
        }
        default:
            break;
    }
}

}  // namespace peekg::data