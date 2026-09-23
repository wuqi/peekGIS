#pragma once
// 瓦片内 arc 拓扑: 建拓扑 -> 小面并入邻面 -> 按 arc 抽稀 -> 还原。
// 移植 mapshaper buildPathTopology / ArcIndex / dissolve / simplify 的思路。
// 接缝: 相邻瓦片间无共享 arc(各自独立), 故把净区边界上的点全部钉住(x/y ∈ {0,netSize})。
// 只依赖 VtTile, 无 GL/GEOS, 可单测。
#include "vt/vt_types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <vector>

namespace peekg::vt {

namespace topo_detail {

inline int64_t triArea2(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t cx, int64_t cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}
inline uint64_t ckey(int64_t x, int64_t y) {
    return ((uint64_t)(uint32_t)(int32_t)x << 32) | (uint32_t)(int32_t)y;
}

inline void vwThresholds(const std::vector<int64_t>& xs, const std::vector<int64_t>& ys,
                         const std::vector<uint8_t>& pin, std::vector<double>& kk) {
    const int n = (int)xs.size();
    const double INF = std::numeric_limits<double>::infinity();
    kk.assign((size_t)(n > 0 ? n : 0), INF);
    if (n < 3) return;
    std::vector<int> prv((size_t)n), nxt((size_t)n);
    std::vector<char> alive((size_t)n, 1);
    using Item = std::pair<double, int>;
    std::vector<Item> heap; heap.reserve((size_t)n);
    auto area = [&](int a, int b, int c) {
        return (double)std::llabs(triArea2(xs[(size_t)a], ys[(size_t)a], xs[(size_t)b], ys[(size_t)b],
                                           xs[(size_t)c], ys[(size_t)c])) * 0.5;
    };
    for (int i = 0; i < n; ++i) {
        prv[(size_t)i] = i - 1; nxt[(size_t)i] = i + 1;
        double v = (i == 0 || i == n - 1 || pin[(size_t)i]) ? INF : area(i - 1, i, i + 1);
        kk[(size_t)i] = v; heap.push_back({v, i});
    }
    std::make_heap(heap.begin(), heap.end(), std::greater<Item>());
    double maxVal = -INF;
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), std::greater<Item>());
        Item it = heap.back(); heap.pop_back();
        const int c = it.second;
        if (!alive[(size_t)c] || it.first != kk[(size_t)c]) continue;
        if (it.first == INF) break;
        if (it.first < maxVal) kk[(size_t)c] = maxVal; else maxVal = it.first;
        const int b = prv[(size_t)c], d = nxt[(size_t)c];
        alive[(size_t)c] = 0; nxt[(size_t)b] = d; prv[(size_t)d] = b;
        if (b > 0 && !pin[(size_t)b]) { double nv = area(prv[(size_t)b], b, d); kk[(size_t)b] = nv; heap.push_back({nv, b}); std::push_heap(heap.begin(), heap.end(), std::greater<Item>()); }
        if (d < n - 1 && !pin[(size_t)d]) { double nv = area(b, d, nxt[(size_t)d]); kk[(size_t)d] = nv; heap.push_back({nv, d}); std::push_heap(heap.begin(), heap.end(), std::greater<Item>()); }
    }
    for (int i = 1; i < n - 1; ++i) if (kk[(size_t)i] < INF) kk[(size_t)i] = std::sqrt(kk[(size_t)i]) * 0.65;
}

}  // namespace topo_detail

struct TileTopo {
    struct Arc { std::vector<int64_t> x, y; };
    std::vector<int64_t> xs, ys;              // 全部点
    std::vector<uint32_t> ringStart, ringLen; // 环在点数组中的位置
    std::vector<uint8_t> ringType, ringHole;
    std::vector<uint32_t> ringPoly;
    std::vector<Arc> arcs;
    std::vector<std::vector<int>> ringArcs;   // 环 -> 有符号 arc id(<0: ~id 反向)

    double ringArea(int ri) const {
        double a = 0; uint32_t s = ringStart[(size_t)ri], n = ringLen[(size_t)ri];
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t j = (i + 1) % n;
            a += (double)xs[s + i] * ys[s + j] - (double)xs[s + j] * ys[s + i];
        }
        return std::fabs(a) * 0.5;
    }
};

inline bool buildTileTopo(const VtTile& t, TileTopo& tp) {
    const int nRing = (int)t.rings.size();
    if (nRing < 1) return false;
    const int16_t* V = t.verts.data();
    tp.xs.clear(); tp.ys.clear(); tp.ringStart.assign((size_t)nRing, 0); tp.ringLen.assign((size_t)nRing, 0);
    std::vector<int> pathOf;
    for (int ri = 0; ri < nRing; ++ri) {
        const VtRing& r = t.rings[(size_t)ri];
        tp.ringStart[(size_t)ri] = (uint32_t)tp.xs.size();
        tp.ringLen[(size_t)ri] = r.vertexCount;
        for (uint32_t k = 0; k < r.vertexCount; ++k) {
            tp.xs.push_back(V[(size_t)(r.firstVertex + k) * 2]);
            tp.ys.push_back(V[(size_t)(r.firstVertex + k) * 2 + 1]);
            pathOf.push_back(ri);
        }
    }
    const int N = (int)tp.xs.size();
    if (N < 3) return false;
    auto& xs = tp.xs; auto& ys = tp.ys;

    std::unordered_map<uint64_t, std::vector<int>> byCoord; byCoord.reserve((size_t)N * 2);
    for (int i = 0; i < N; ++i) byCoord[topo_detail::ckey(xs[(size_t)i], ys[(size_t)i])].push_back(i);
    std::vector<int> chainIds((size_t)N);
    for (int i = 0; i < N; ++i) chainIds[(size_t)i] = i;
    for (auto& kv : byCoord) { auto& v = kv.second; for (size_t k = 0; k < v.size(); ++k) chainIds[(size_t)v[k]] = v[(k + 1) % v.size()]; }

    auto nextPoint = [&](int id) { int ri = pathOf[(size_t)id]; uint32_t s = tp.ringStart[(size_t)ri], len = tp.ringLen[(size_t)ri]; return (uint32_t)(id - (int)s) + 1 < len ? id + 1 : (int)s; };
    auto prevPoint = [&](int id) { int ri = pathOf[(size_t)id]; uint32_t s = tp.ringStart[(size_t)ri], len = tp.ringLen[(size_t)ri]; return id > (int)s ? id - 1 : (int)(s + len - 1); };
    auto brokenEdge = [&](int ap, int an, int bp, int bn) {
        return !((xs[(size_t)ap] == xs[(size_t)bp] && xs[(size_t)an] == xs[(size_t)bn] && ys[(size_t)ap] == ys[(size_t)bp] && ys[(size_t)an] == ys[(size_t)bn]) ||
                 (xs[(size_t)ap] == xs[(size_t)bn] && xs[(size_t)an] == xs[(size_t)bp] && ys[(size_t)ap] == ys[(size_t)bn] && ys[(size_t)an] == ys[(size_t)bp]));
    };
    auto isEndpoint = [&](int id) {
        int id2 = chainIds[(size_t)id], prev = prevPoint(id), next = nextPoint(id);
        while (id != id2) { int p2 = prevPoint(id2), n2 = nextPoint(id2); if (brokenEdge(prev, next, p2, n2)) return true; id2 = chainIds[(size_t)id2]; }
        return false;
    };

    tp.arcs.clear();
    tp.ringArcs.assign((size_t)nRing, {});
    std::unordered_map<uint64_t, std::vector<int>> arcByStart;
    auto findDupRevIds = [&](const std::vector<int>& ids) -> int {
        auto it = arcByStart.find(topo_detail::ckey(xs[(size_t)ids.back()], ys[(size_t)ids.back()]));
        if (it == arcByStart.end()) return INT_MIN;   // 注意: ~arcId 是负数, 不能用 -1 当哨兵
        for (int aid : it->second) {
            const auto& a = tp.arcs[(size_t)aid];
            if (a.x.size() != ids.size()) continue;
            bool ok = true;
            for (size_t t = 0; t < ids.size(); ++t) {
                int r = ids[ids.size() - 1 - t];
                if (a.x[(size_t)t] != xs[(size_t)r] || a.y[(size_t)t] != ys[(size_t)r]) { ok = false; break; }
            }
            if (ok) return ~aid;
        }
        return INT_MIN;
    };
    auto addArcIds = [&](const std::vector<int>& ids) -> int {
        TileTopo::Arc a; a.x.reserve(ids.size()); a.y.reserve(ids.size());
        for (int id : ids) { a.x.push_back(xs[(size_t)id]); a.y.push_back(ys[(size_t)id]); }
        int aid = (int)tp.arcs.size();
        arcByStart[topo_detail::ckey(a.x.front(), a.y.front())].push_back(aid);
        tp.arcs.push_back(std::move(a));
        return aid;
    };
    // 环上从点 a 前进(可绕回)到点 b 的点序列(含两端)
    auto arcPointIds = [&](int a, int b) {
        std::vector<int> ids;
        int cur = a, guard = 0;
        while (guard++ < N + 2) { ids.push_back(cur); if (cur == b) break; cur = nextPoint(cur); }
        return ids;
    };
    for (int ri = 0; ri < nRing; ++ri) {
        uint32_t s = tp.ringStart[(size_t)ri], len = tp.ringLen[(size_t)ri];
        if (len < 2) continue;
        std::vector<int> nodes;
        for (uint32_t k = 0; k < len; ++k) if (isEndpoint((int)(s + k))) nodes.push_back((int)(s + k));
        if (nodes.empty()) {
            std::vector<int> ids;
            for (uint32_t k = 0; k < len; ++k) ids.push_back((int)(s + k));
            int dup = findDupRevIds(ids);
            tp.ringArcs[(size_t)ri].push_back(dup != INT_MIN ? dup : addArcIds(ids));
        } else {
            for (size_t j = 0; j < nodes.size(); ++j) {
                int a = nodes[j], b = nodes[(j + 1) % nodes.size()];
                std::vector<int> ids = arcPointIds(a, b);
                int dup = findDupRevIds(ids);
                tp.ringArcs[(size_t)ri].push_back(dup != INT_MIN ? dup : addArcIds(ids));
            }
        }
    }
    tp.ringType.resize((size_t)nRing); tp.ringHole.resize((size_t)nRing); tp.ringPoly.resize((size_t)nRing);
    for (int ri = 0; ri < nRing; ++ri) {
        tp.ringType[(size_t)ri] = t.rings[(size_t)ri].type;
        tp.ringHole[(size_t)ri] = t.rings[(size_t)ri].hole;
        tp.ringPoly[(size_t)ri] = t.rings[(size_t)ri].polyGroup;
    }
    return true;
}

// 弧序列(环)的面积(格²): 拼接弧点后 shoelace。
inline double ringArcArea(const TileTopo& tp, const std::vector<int>& arcs) {
    if (arcs.empty()) return 0.0;
    double a = 0; int64_t px = 0, py = 0; bool first = true;
    int64_t x0 = 0, y0 = 0;
    for (size_t si = 0; si < arcs.size(); ++si) {
        int id = arcs[si], aid = id >= 0 ? id : ~id;
        const auto& arc = tp.arcs[(size_t)aid];
        int m = (int)arc.x.size();
        for (int k = (si == 0 ? 0 : 1); k < m; ++k) {
            int kk = id >= 0 ? k : (m - 1 - k);
            int64_t x = arc.x[(size_t)kk], y = arc.y[(size_t)kk];
            if (first) { x0 = px = x; y0 = py = y; first = false; continue; }
            a += (double)px * y - (double)x * py;
            px = x; py = y;
        }
    }
    a += (double)px * y0 - (double)x0 * py;
    return std::fabs(a) * 0.5;
}

// F ∪ G 的边界(两环共享任意条弧): 沿环切换(walk-and-switch)。
// 从 F 的非公共弧出发沿 F 走; 遇公共弧切到 G, 从对应公共弧的下一条沿 G 走; 再遇公共弧切回 F;
// 直到回到起点。剩余未用的非公共弧再开新环(并集可能是多个环)。失败返回 false。
inline bool mergeTwoRings(const TileTopo& tp, const std::vector<int>& FA, const std::vector<int>& GA,
                          std::vector<std::vector<int>>& outRings) {
    outRings.clear();
    if (FA.empty() || GA.empty()) return false;
    auto absi = [](int id) { return id >= 0 ? id : ~id; };
    std::unordered_map<int, std::pair<int, int>> sharedMap;   // abs -> (iF, iG)
    for (int i = 0; i < (int)FA.size(); ++i)
        for (int j = 0; j < (int)GA.size(); ++j) {
            int a = FA[(size_t)i], b = GA[(size_t)j];
            if (a == b || a == ~b) sharedMap[absi(a)] = {i, j};
        }
    if (sharedMap.empty()) return false;
    std::vector<char> usedF(FA.size(), 0), usedG(GA.size(), 0);
    const int lim = (int)(FA.size() + GA.size()) * 4 + 16;
    while (true) {
        int start = -1; bool onF = true;
        for (int i = 0; i < (int)FA.size(); ++i)
            if (!usedF[(size_t)i] && !sharedMap.count(absi(FA[(size_t)i]))) { start = i; break; }
        if (start < 0) {
            onF = false;
            for (int j = 0; j < (int)GA.size(); ++j)
                if (!usedG[(size_t)j] && !sharedMap.count(absi(GA[(size_t)j]))) { start = j; break; }
        }
        if (start < 0) break;
        std::vector<int> ring;
        int iF = onF ? start : 0, iG = onF ? 0 : start;
        bool curF = onF, started = false, closed = false;
        int guard = 0;
        while (guard++ < lim) {
            if (curF) {
                if (started && onF && iF == start) { closed = true; break; }
                started = true;
                int a = FA[(size_t)iF];
                auto it = sharedMap.find(absi(a));
                if (it == sharedMap.end()) { ring.push_back(a); usedF[(size_t)iF] = 1; iF = (iF + 1) % (int)FA.size(); }
                else { iG = (it->second.second + 1) % (int)GA.size(); curF = false; }
            } else {
                if (started && !onF && iG == start) { closed = true; break; }
                started = true;
                int b = GA[(size_t)iG];
                auto it = sharedMap.find(absi(b));
                if (it == sharedMap.end()) { ring.push_back(b); usedG[(size_t)iG] = 1; iG = (iG + 1) % (int)GA.size(); }
                else { iF = (it->second.first + 1) % (int)FA.size(); curF = true; }
            }
        }
        if (!closed || ring.empty()) return false;
        outRings.push_back(std::move(ring));
    }
    return !outRings.empty();
}

// 小面并入邻面(B: 局部拼接)。面积 < minAreaCells 的面与共享边界最多的邻面合并。
inline int mergeSmallFacesLocal(TileTopo& tp, double minAreaCells) {
    const int nRing = (int)tp.ringArcs.size();
    if (nRing < 2) return nRing;
    std::vector<char> removed((size_t)nRing, 0);
    std::unordered_map<int, std::vector<int>> arcUsers;
    for (int ri = 0; ri < nRing; ++ri)
        for (int id : tp.ringArcs[(size_t)ri]) arcUsers[id >= 0 ? id : ~id].push_back(ri);
    std::vector<double> ar((size_t)nRing);
    for (int i = 0; i < nRing; ++i) ar[(size_t)i] = tp.ringArea(i);

    for (int fi = 0; fi < nRing; ++fi) {
        if (tp.ringType[(size_t)fi] != RING_FACE || removed[(size_t)fi]) continue;
        if (ar[(size_t)fi] >= minAreaCells) continue;
        std::unordered_map<int, int> shared;
        for (int id : tp.ringArcs[(size_t)fi])
            for (int nb : arcUsers[id >= 0 ? id : ~id])
                if (nb != fi && !removed[(size_t)nb] && tp.ringType[(size_t)nb] == RING_FACE) shared[nb]++;
        int best = -1, bestCnt = 0, bestBig = -1;
        for (auto& kv : shared) {
            int big = (ar[(size_t)kv.first] >= minAreaCells) ? 1 : 0;
            if (big > bestBig || (big == bestBig && kv.second > bestCnt)) { best = kv.first; bestCnt = kv.second; bestBig = big; }
        }
        if (best < 0) continue;
        // 只处理"恰好一条公共弧"(稳定, 不造洞); 多弧的 walk 会在部分 case 拼出无效环 -> 暂不用
        int sIdxF = -1, sIdxG = -1, nShared = 0;
        for (size_t i = 0; i < tp.ringArcs[(size_t)fi].size(); ++i)
            for (size_t j = 0; j < tp.ringArcs[(size_t)best].size(); ++j) {
                int a = tp.ringArcs[(size_t)fi][i], b = tp.ringArcs[(size_t)best][j];
                if (a == b || a == ~b) { ++nShared; sIdxF = (int)i; sIdxG = (int)j; }
            }
        if (nShared != 1) continue;
        std::vector<int> merged;
        merged.reserve(tp.ringArcs[(size_t)best].size() + tp.ringArcs[(size_t)fi].size());
        for (int k = 0; k < sIdxG; ++k) merged.push_back(tp.ringArcs[(size_t)best][(size_t)k]);
        int m = (int)tp.ringArcs[(size_t)fi].size();
        for (int k = 1; k < m; ++k) merged.push_back(tp.ringArcs[(size_t)fi][(size_t)((sIdxF + k) % m)]);
        for (size_t k = (size_t)sIdxG + 1; k < tp.ringArcs[(size_t)best].size(); ++k) merged.push_back(tp.ringArcs[(size_t)best][k]);
        tp.ringArcs[(size_t)best] = std::move(merged);
        removed[(size_t)fi] = 1;
    }

    // 压实: 去掉被并入的环
    std::vector<std::vector<int>> nArcs;
    std::vector<uint8_t> nType, nHole;
    std::vector<uint32_t> nPoly;
    for (int i = 0; i < nRing; ++i) {
        if (removed[(size_t)i] || tp.ringArcs[(size_t)i].empty()) continue;
        nArcs.push_back(std::move(tp.ringArcs[(size_t)i]));
        nType.push_back(tp.ringType[(size_t)i]);
        nHole.push_back(tp.ringHole[(size_t)i]);
        nPoly.push_back(tp.ringPoly[(size_t)i]);
    }
    if (nArcs.empty()) return nRing;
    tp.ringArcs = std::move(nArcs);
    tp.ringType = std::move(nType);
    tp.ringHole = std::move(nHole);
    tp.ringPoly = std::move(nPoly);
    return (int)tp.ringArcs.size();
}

inline void simplifyTileArcs(TileTopo& tp, double tol, int netSize) {
    for (auto& a : tp.arcs) {
        int m = (int)a.x.size();
        if (m < 3) continue;
        std::vector<uint8_t> pin((size_t)m, 0);
        for (int k = 0; k < m; ++k) {
            int64_t x = a.x[(size_t)k], y = a.y[(size_t)k];
            if (x == 0 || x == netSize || y == 0 || y == netSize) pin[(size_t)k] = 1;
        }
        std::vector<double> kk; topo_detail::vwThresholds(a.x, a.y, pin, kk);
        TileTopo::Arc b;
        for (int k = 0; k < m; ++k) if (kk[(size_t)k] >= tol) { b.x.push_back(a.x[(size_t)k]); b.y.push_back(a.y[(size_t)k]); }
        if (b.x.size() >= 2) a = std::move(b);
    }
}

// 由拓扑还原 VtTile(点已是 int16 范围)。返回 false 表示无有效环。
inline bool rebuildTile(const TileTopo& tp, VtTile& t) {
    std::vector<int16_t> nv;
    std::vector<VtRing> nr;
    const int nRing = (int)tp.ringArcs.size();
    for (int ri = 0; ri < nRing; ++ri) {
        const auto& seq = tp.ringArcs[(size_t)ri];
        if (seq.empty()) continue;
        std::vector<int16_t> q;
        auto appendPts = [&](int id, bool skipFirst) {
            int a = id >= 0 ? id : ~id;
            const auto& arc = tp.arcs[(size_t)a];
            int m = (int)arc.x.size();
            for (int k = skipFirst ? 1 : 0; k < m; ++k) {
                int kk = (id >= 0) ? k : (m - 1 - k);
                q.push_back((int16_t)arc.x[(size_t)kk]);
                q.push_back((int16_t)arc.y[(size_t)kk]);
            }
        };
        for (size_t si = 0; si < seq.size(); ++si) appendPts(seq[si], si != 0);
        while (q.size() >= 4 && q[0] == q[q.size() - 2] && q[1] == q[q.size() - 1]) q.resize(q.size() - 2);
        uint8_t type = tp.ringType[(size_t)ri];
        uint32_t minv = (type == RING_FACE) ? 3 : (type == RING_POINT ? 1 : 2);
        if (q.size() / 2 < minv) continue;
        VtRing r;
        r.type = type; r.hole = tp.ringHole[(size_t)ri]; r.polyGroup = tp.ringPoly[(size_t)ri];
        r.firstVertex = (uint32_t)(nv.size() / 2);
        r.vertexCount = (uint32_t)(q.size() / 2);
        nv.insert(nv.end(), q.begin(), q.end());
        nr.push_back(r);
    }
    if (nr.empty()) return false;
    t.verts.swap(nv);
    t.rings.swap(nr);
    return true;
}

// 重建后按"嵌套深度"重新定孔(奇=孔)。孔只需与同 polyGroup 的环比较(孔与外环同组),
// 避免 O(环数²) 的全量两两判包含(实测 L4 占后处理 76%)。
inline void assignHolesByNesting(VtTile& t) {
    const int n = (int)t.rings.size();
    std::unordered_map<uint32_t, std::vector<int>> byPoly;
    byPoly.reserve((size_t)n * 2);
    for (int i = 0; i < n; ++i)
        if (t.rings[i].type == RING_FACE && t.rings[i].vertexCount >= 3)
            byPoly[t.rings[i].polyGroup].push_back(i);
    for (auto& kv : byPoly) {
        const std::vector<int>& ids = kv.second;
        const size_t m = ids.size();
        for (size_t a = 0; a < m; ++a) {
            const VtRing& r = t.rings[(size_t)ids[a]];
            double px = t.verts[(size_t)r.firstVertex * 2];
            double py = t.verts[(size_t)r.firstVertex * 2 + 1];
            int depth = 0;
            for (size_t b = 0; b < m; ++b) {
                if (a == b) continue;
                const VtRing& rr = t.rings[(size_t)ids[b]];
                bool in = false;
                for (uint32_t p = 0, q = rr.vertexCount - 1; p < rr.vertexCount; q = p++) {
                    double xi = t.verts[(size_t)(rr.firstVertex + p) * 2], yi = t.verts[(size_t)(rr.firstVertex + p) * 2 + 1];
                    double xj = t.verts[(size_t)(rr.firstVertex + q) * 2], yj = t.verts[(size_t)(rr.firstVertex + q) * 2 + 1];
                    if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) in = !in;
                }
                if (in) ++depth;
            }
            t.rings[(size_t)ids[a]].hole = (depth & 1) ? 1 : 0;
        }
    }
}

// 对一张瓦片做"拓扑抽稀 + 小面并入邻面"。tol<=0 跳过抽稀; minAreaCells<=0 跳过小面合并。
inline bool processTileTopology(VtTile& t, double tol, int netSize, double minAreaCells) {
    if (t.rings.size() < 2) return false;
    const bool tm = std::getenv("PEEK_VT_TOPO_TIME") != nullptr;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const size_t nR = t.rings.size(), nV = t.vertexCount();
    auto t0 = now();
    TileTopo tp;
    if (!buildTileTopo(t, tp)) return false;
    auto t1 = now();
    if (minAreaCells > 0.0) mergeSmallFacesLocal(tp, minAreaCells);
    auto t2 = now();
    if (tol > 0.0) simplifyTileArcs(tp, tol, netSize);
    auto t3 = now();
    if (!rebuildTile(tp, t)) return false;
    auto t4 = now();
    assignHolesByNesting(t);
    auto t5 = now();
    if (tm)
        fprintf(stderr, "[topo-time] 环=%zu 点=%zu arc=%zu | build=%.1f dis=%.1f simp=%.1f rebuild=%.1f holes=%.1f 合计=%.1f ms\n",
                nR, nV, tp.arcs.size(), ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), ms(t4, t5), ms(t0, t5));
    return true;
}

}  // namespace peekg::vt
