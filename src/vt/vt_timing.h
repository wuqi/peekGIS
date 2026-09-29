#pragma once
// 构建耗时埋点: PEEK_VT_TIMING=1 时累计各阶段耗时, 构建结束统一打印。
// 阶段B 的拓扑后处理是多线程的, 故累加器用 relaxed 原子(关闭时不做原子操作,
// 只走一次 bool 判断, 无原子开销)。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace peekg::vt {

struct VtTimeAcc {
    bool on = false;
    // 阶段A: 读源+建各保留层(单线程)
    std::atomic<double> streamTotal{0};   // streamVtRings 整段(读源+collectRing+sink)
    std::atomic<double> route{0};   std::atomic<long long> nRoute{0};
    std::atomic<double> clip{0};    std::atomic<long long> nClip{0};
    std::atomic<double> append{0};  std::atomic<long long> nAppend{0};
    std::atomic<double> flush{0};   std::atomic<long long> nFlush{0};
    // 阶段B: 拓扑后处理(每片; 多线程 -> 原子累加)
    std::atomic<double> topoRead{0}, topoBuild{0}, topoMerge{0}, topoSimp{0},
                         topoRebuild{0}, topoHoles{0}, topoWrite{0};
    std::atomic<long long> nTopo{0};
    std::atomic<long long> nClipFast{0};   // 走"环完全在瓦片内"快路径的次数
};

// 阶段B 线程数上限: 留 2 核给主线程/IO, 免得构建时界面卡。
inline unsigned vtTopoThreadCap() {
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 4;
    return hc > 2 ? hc - 2 : 1;
}

inline VtTimeAcc& vtTime() {
    static VtTimeAcc a;
    static bool init = false;
    if (!init) { init = true; a.on = std::getenv("PEEK_VT_TIMING") != nullptr; }
    return a;
}

inline void vtAdd(std::atomic<double>& acc, double v) {
    acc.store(acc.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}
inline void vtAdd(std::atomic<long long>& cnt, long long v) {
    cnt.store(cnt.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}
inline double vtGet(const std::atomic<double>& a) { return a.load(std::memory_order_relaxed); }
inline long long vtGet(const std::atomic<long long>& a) { return a.load(std::memory_order_relaxed); }

// 作用域计时: 累加毫秒 + 计数。关闭时只走一次 bool 判断, 不调 chrono、不做原子。
struct VtScope {
    std::atomic<double>& acc;
    std::atomic<long long>& cnt;
    std::chrono::steady_clock::time_point t0;
    bool on;
    VtScope(std::atomic<double>& a, std::atomic<long long>& c)
        : acc(a), cnt(c), on(vtTime().on), t0(std::chrono::steady_clock::now()) {}
    ~VtScope() {
        if (!on) return;
        vtAdd(acc, std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count());
        vtAdd(cnt, 1);
    }
};

// 只计时间不计数(如读片/写片这种按外部循环计数的)
struct VtScopeNoCnt {
    std::atomic<double>& acc;
    std::chrono::steady_clock::time_point t0;
    bool on;
    VtScopeNoCnt(std::atomic<double>& a)
        : acc(a), on(vtTime().on), t0(std::chrono::steady_clock::now()) {}
    ~VtScopeNoCnt() {
        if (!on) return;
        vtAdd(acc, std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count());
    }
};

}  // namespace peekg::vt
