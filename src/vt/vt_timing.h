#pragma once
// 构建耗时埋点: PEEK_VT_TIMING=1 时累计各阶段耗时, 构建结束统一打印。
// 单线程构建, 用 inline 全局累加器即可(不需原子); 关闭时 chrono 采样被跳过分支。
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace peekg::vt {

struct VtTimeAcc {
    bool on = false;
    // 阶段A: 读源+建各保留层
    double streamTotal = 0;   // streamVtRings 整段(读源+collectRing+sink)
    double route      = 0;   long long nRoute = 0;   // sink 内 routeRing 总计
    double clip       = 0;   long long nClip  = 0;   // geosClipRings (GEOS 交)
    double append     = 0;   long long nAppend= 0;   // 量化+共线压缩+退化判定
    double flush      = 0;   long long nFlush = 0;   // 淘汰落盘(读回+小面合并+写)
    // 阶段B: 拓扑后处理(每片)
    double topoRead = 0, topoBuild = 0, topoMerge = 0, topoSimp = 0, topoRebuild = 0, topoHoles = 0, topoWrite = 0;
    long long nTopo = 0;
};

inline VtTimeAcc& vtTime() {
    static VtTimeAcc a;
    static bool init = false;
    if (!init) { init = true; a.on = std::getenv("PEEK_VT_TIMING") != nullptr; }
    return a;
}

// 作用域计时: 累加毫秒。关闭时只做计数, 不调 chrono。
struct VtScope {
    double& acc;
    long long& cnt;
    std::chrono::steady_clock::time_point t0;
    bool on;
    VtScope(double& a, long long& c)
        : acc(a), cnt(c), on(vtTime().on), t0(std::chrono::steady_clock::now()) {}
    ~VtScope() {
        ++cnt;
        if (on) acc += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    }
};

// 拓扑后处理各步(已有 VtTimeAcc::topo* 字段, 这里单列以便复用 VtScope 的写法)
struct VtScopeNoCnt {
    double& acc;
    std::chrono::steady_clock::time_point t0;
    bool on;
    VtScopeNoCnt(double& a) : acc(a), on(vtTime().on), t0(std::chrono::steady_clock::now()) {}
    ~VtScopeNoCnt() {
        if (on) acc += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    }
};

}  // namespace peekg::vt
