// vt_bench -- 按层统计 v2 矢量瓦片的几何量与解码耗时(渲染前的主要 CPU 成本)。
// 用法: vt_bench <cache.vtk> [每层采样瓦片数=200]
// 输出每层: 有效片数 / 均顶点每片 / 总顶点(按有效片估算) / 均环数 / 单片解码耗时 / 解码吞吐
#include "vt/vt_cache.h"
#include "vt/vt_types.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace peekg::vt;

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) { printf("用法: vt_bench <cache.vtk> [每层采样数=200]\n"); return 1; }
    std::string path = argv[1];
    int sampleN = (argc > 2) ? std::atoi(argv[2]) : 200;

    std::ifstream f(path, std::ios::binary);
    if (!f) { printf("打开失败: %s\n", path.c_str()); return 1; }
    VtFileHeader h{};
    f.read((char*)&h, sizeof(h));
    if (std::memcmp(h.magic, VT_MAGIC, 8) != 0) { printf("magic 不符\n"); return 1; }
    printf("cache=%s\n  maxLevel=%u tileW0=%.8f fullyBuilt=0x%x srcEpsg=%d dstEpsg=%d\n",
           path.c_str(), h.maxLevel, h.tileW0, h.fullyBuiltLevels, h.srcEpsg, h.dstEpsg);

    VtCache c;
    if (!c.open(path)) { printf("VtCache open 失败\n"); return 1; }

    // 稀疏槽表布局: [header][data][sparse slots][row dir]
    // 每 (level,ty) 一条 VtRowDir, 描述该行 [txLo, txLo+count) 用了哪些槽;
    // 槽在槽区里的绝对下标 = r.slotStart + (tx - r.txLo)。
    // 旧版是「按层连续排布的 4^L 槽表」+ 单一 slotTableOffset 起点, 已被行稀疏化取代。
    std::vector<VtRowDir> rows;
    if (h.rowDirOffset == 0 || h.rowDirCount == 0) {
        printf("槽区/行目录为空(缓存未 finalize)\n");
        return 1;
    }
    f.clear();
    f.seekg((std::streamoff)h.rowDirOffset);
    rows.resize(h.rowDirCount);
    f.read((char*)rows.data(), (std::streamsize)(rows.size() * sizeof(VtRowDir)));
    if ((size_t)f.gcount() != rows.size() * sizeof(VtRowDir)) {
        printf("行目录读取不完整(要 %u 条, 实得 %zu)\n", h.rowDirCount, (size_t)f.gcount() / sizeof(VtRowDir));
        return 1;
    }

    // 一次把整个稀疏槽区读进来, 避免每片单独 seek(每片 16 字节, 随机 seek 极慢)
    std::vector<VtSlot> slotArea;
    {
        // 槽区从 slotAreaOffset 到 rowDirOffset (行目录紧接槽区之后)
        uint64_t n = (h.rowDirOffset > h.slotAreaOffset)
                         ? (h.rowDirOffset - h.slotAreaOffset) / sizeof(VtSlot) : 0;
        slotArea.resize((size_t)n);
        f.clear();
        f.seekg((std::streamoff)h.slotAreaOffset);
        if (!slotArea.empty())
            f.read((char*)slotArea.data(), (std::streamsize)(n * sizeof(VtSlot)));
        if ((uint64_t)f.gcount() != n * sizeof(VtSlot)) {
            printf("槽区读取不完整(要 %llu 槽, 实得 %llu)\n",
                   (unsigned long long)n, (unsigned long long)(f.gcount() / (std::streamsize)sizeof(VtSlot)));
            return 1;
        }
    }

    printf("\n%-3s %8s %8s %13s %13s %10s %11s %10s\n",
           "L", "有效片", "满额槽", "均顶点/片", "总顶点(估)", "均环/片", "解码us/片", "解码MB/s");

    for (int L = 0; L <= (int)h.maxLevel; ++L) {
        // 收集本层的有效片: (tx, ty, 压缩字节数)
        struct Hit { int tx, ty; uint32_t bytes; };
        std::vector<Hit> valid;
        for (const VtRowDir& r : rows) {
            if ((int)r.level != L) continue;
            for (uint32_t i = 0; i < (uint32_t)r.count; ++i) {
                uint64_t si = (uint64_t)r.slotStart + i;
                if (si >= slotArea.size()) continue;          // 槽区截断: 跳过
                const VtSlot& s = slotArea[(size_t)si];
                if (!s.valid) continue;
                valid.push_back({(int)r.txLo + (int)i, (int)r.ty, s.size});
            }
        }
        long long sv = 0, sr = 0, sbytes = 0;
        int cnt = 0;
        double ms = 0;
        uint64_t step = valid.empty() ? 1 : std::max<uint64_t>(1, (uint64_t)valid.size() / (uint64_t)sampleN);
        for (uint64_t k = 0; k < valid.size() && cnt < sampleN; k += step) {
            const Hit& hv = valid[k];
            VtTile t;
            auto t0 = std::chrono::steady_clock::now();
            bool ok = c.readTile(L, hv.tx, hv.ty, t);
            auto t1 = std::chrono::steady_clock::now();
            if (!ok) continue;
            ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            sv += t.vertexCount();
            sr += (long long)t.rings.size();
            sbytes += (long long)hv.bytes;
            ++cnt;
        }
        double avgV = cnt ? (double)sv / cnt : 0;
        double avgR = cnt ? (double)sr / cnt : 0;
        double usPer = cnt ? ms * 1000.0 / cnt : 0;
        double mbps = ms > 0 ? (sbytes / 1048576.0) / (ms / 1000.0) : 0;
        printf("%-3d %8llu %8llu %13.1f %13.0f %10.1f %11.1f %10.1f\n",
               L, (unsigned long long)valid.size(), (unsigned long long)slotCount(L),
               avgV, avgV * (double)valid.size(), avgR, usPer, mbps);
    }
    return 0;
}
