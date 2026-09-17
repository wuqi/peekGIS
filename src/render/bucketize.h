// 矢量图层 显示CRS网格 纯CPU分块(无 GL 依赖)。gl_backend 分块与单测共用同一几何核心。
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "data/reproject.h"

namespace vbucket {

using peekg::data::reprojectVertices;
using peekg::data::computeExtent;

inline constexpr long long kTargetVertsPerBucket = 262144;  // 目标块大小 ~256K 顶点
inline constexpr size_t kMaxGridN = 252;

// 纯 CPU 分区结果: 每格原始坐标数组 + 格->块映射 + 网格参数(无任何 GL 对象)。
struct CpuPartition {
    std::vector<std::vector<float>> cellV;   // 每格线段顶点
    std::vector<std::vector<float>> cellP;   // 每格点集
    std::vector<std::vector<float>> cellF;   // 每格填充三角形
    std::vector<int> cellToBucket;           // 格下标 -> buckets 索引(-1=空)
    std::vector<size_t> bucketCell;          // buckets 索引 -> 格下标(与 cellToBucket 对齐)
    // 每格 bbox(供桶渲染裁剪用)
    std::vector<double> cellMinx, cellMiny, cellMaxx, cellMaxy;
    size_t gridN = 1;                        // 网格边数(格数 = gridN^2)
    double minx = 0, miny = 0, maxx = 0, maxy = 0;
    double cellW = 1, cellH = 1;
    long long totalVerts = 0;
};

// 点 (x,y) 所在格下标(行列 clamp 到 [0,nc))
inline size_t vecCellIndex(double x, double y, double minx, double miny,
                           double cellW, double cellH, size_t nc) {
    size_t cx = (size_t)std::floor((x - minx) / cellW);
    size_t cy = (size_t)std::floor((y - miny) / cellH);
    if (cx >= nc) cx = nc - 1;
    if (cy >= nc) cy = nc - 1;
    return cy * nc + cx;
}

// 分块: V/P/F 为源CRS坐标, srcEpsg != targetEpsg 时先整体重投影(失败回退源坐标)。
// 图元完整归入一格(线段按中点/点按自身/三角形按重心), 防跨格撕裂。
inline bool partitionLayer(const std::vector<float>& V, const std::vector<float>& P,
                           const std::vector<float>& F, int srcEpsg, int targetEpsg,
                           CpuPartition& out) {
    out = CpuPartition{};
    out.gridN = 1;
    if (V.empty() && P.empty() && F.empty()) return true;   // 空层: 空结果

    // 重投影(仅 src != target), 失败回退源坐标
    std::vector<float> Rv, Rp, Rf;
    const std::vector<float> *pV = &V, *pP = &P, *pF = &F;
    if (srcEpsg != targetEpsg) {
        std::vector<float> tmp;
        if (reprojectVertices(V, srcEpsg, targetEpsg, tmp)) { Rv.swap(tmp); pV = &Rv; }
        if (reprojectVertices(P, srcEpsg, targetEpsg, tmp)) { Rp.swap(tmp); pP = &Rp; }
        if (reprojectVertices(F, srcEpsg, targetEpsg, tmp)) { Rf.swap(tmp); pF = &Rf; }
    }

    auto bounds = [&](const std::vector<float>& s, bool& any,
                      double& mnx, double& mny, double& mxx, double& mxy) {
        if (s.empty()) return;
        double a, b, c, d;
        computeExtent(s, a, b, c, d);
        if (!any) { mnx = a; mny = b; mxx = c; mxy = d; any = true; }
        else {
            mnx = std::min(mnx, a); mny = std::min(mny, b);
            mxx = std::max(mxx, c); mxy = std::max(mxy, d);
        }
    };
    bool anyGeom = false;
    double mnx = 0, mny = 0, mxx = 0, mxy = 0;
    bounds(*pV, anyGeom, mnx, mny, mxx, mxy);
    bounds(*pP, anyGeom, mnx, mny, mxx, mxy);
    bounds(*pF, anyGeom, mnx, mny, mxx, mxy);
    if (!anyGeom) return true;

    long long totalVerts = (long long)(pV->size() / 2 + pP->size() / 2 + pF->size() / 2);
    long long s = (long long)std::ceil(std::sqrt((double)std::max<long long>(totalVerts, 1) /
                                                 (double)kTargetVertsPerBucket));
    if (s < 1) s = 1;
    if (s > (long long)kMaxGridN) s = (long long)kMaxGridN;
    size_t nc = (size_t)s;

    out.gridN = nc;
    out.minx = mnx; out.miny = mny; out.maxx = mxx; out.maxy = mxy;
    out.cellW = (mxx - mnx) / (double)nc; if (out.cellW <= 0) out.cellW = 1.0;
    out.cellH = (mxy - mny) / (double)nc; if (out.cellH <= 0) out.cellH = 1.0;
    out.totalVerts = totalVerts;

    size_t ncell = nc * nc;
    out.cellV.assign(ncell, {});
    out.cellP.assign(ncell, {});
    out.cellF.assign(ncell, {});
    out.cellMinx.assign(ncell, 1e300);
    out.cellMiny.assign(ncell, 1e300);
    out.cellMaxx.assign(ncell, -1e300);
    out.cellMaxy.assign(ncell, -1e300);
    auto growCell = [&](size_t ci, float x, float y) {
        out.cellMinx[ci] = std::min(out.cellMinx[ci], (double)x);
        out.cellMiny[ci] = std::min(out.cellMiny[ci], (double)y);
        out.cellMaxx[ci] = std::max(out.cellMaxx[ci], (double)x);
        out.cellMaxy[ci] = std::max(out.cellMaxy[ci], (double)y);
    };

    for (size_t i = 0; i + 3 < pV->size(); i += 4) {
        float ax = (*pV)[i], ay = (*pV)[i + 1], bx = (*pV)[i + 2], by = (*pV)[i + 3];
        size_t ci = vecCellIndex((ax + bx) * 0.5f, (ay + by) * 0.5f, mnx, mny, out.cellW, out.cellH, nc);
        out.cellV[ci].insert(out.cellV[ci].end(), pV->begin() + i, pV->begin() + i + 4);
        growCell(ci, ax, ay); growCell(ci, bx, by);
    }
    for (size_t i = 0; i + 1 < pP->size(); i += 2) {
        size_t ci = vecCellIndex((*pP)[i], (*pP)[i + 1], mnx, mny, out.cellW, out.cellH, nc);
        out.cellP[ci].insert(out.cellP[ci].end(), pP->begin() + i, pP->begin() + i + 2);
        growCell(ci, (*pP)[i], (*pP)[i + 1]);
    }
    for (size_t i = 0; i + 5 < pF->size(); i += 6) {
        float ax = (*pF)[i], ay = (*pF)[i + 1], bx = (*pF)[i + 2], by = (*pF)[i + 3];
        float cx = (*pF)[i + 4], cy = (*pF)[i + 5];
        size_t ci = vecCellIndex((ax + bx + cx) / 3.0f, (ay + by + cy) / 3.0f, mnx, mny, out.cellW, out.cellH, nc);
        out.cellF[ci].insert(out.cellF[ci].end(), pF->begin() + i, pF->begin() + i + 6);
        growCell(ci, ax, ay); growCell(ci, bx, by); growCell(ci, cx, cy);
    }

    // 建成 格->块 映射(每格一个块, 空格跳过)
    out.cellToBucket.assign(ncell, -1);
    for (size_t ci = 0; ci < ncell; ci++) {
        if (out.cellV[ci].empty() && out.cellP[ci].empty() && out.cellF[ci].empty()) continue;
        out.cellToBucket[ci] = (int)out.bucketCell.size();
        out.bucketCell.push_back(ci);
    }
    return true;
}

}  // namespace vbucket