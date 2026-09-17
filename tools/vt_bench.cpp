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

    printf("\n%-3s %8s %8s %13s %13s %10s %11s %10s\n",
           "L", "有效片", "槽数", "均顶点/片", "总顶点(估)", "均环/片", "解码us/片", "解码MB/s");

    uint64_t slotOff = h.slotTableOffset;
    for (int L = 0; L <= (int)h.maxLevel; ++L) {
        uint64_t sc = slotCount(L);
        std::vector<VtSlot> slots((size_t)sc);
        f.clear();
        f.seekg((std::streamoff)slotOff);
        f.read((char*)slots.data(), (std::streamsize)(sc * sizeof(VtSlot)));
        slotOff += sc * sizeof(VtSlot);

        std::vector<uint64_t> valid;
        valid.reserve(1024);
        for (uint64_t i = 0; i < sc; ++i)
            if (slots[i].valid) valid.push_back(i);
        int n = 1 << L;
        long long sv = 0, sr = 0, sbytes = 0;
        int cnt = 0;
        double ms = 0;
        uint64_t step = valid.empty() ? 1 : std::max<uint64_t>(1, (uint64_t)valid.size() / (uint64_t)sampleN);
        for (uint64_t k = 0; k < valid.size() && cnt < sampleN; k += step) {
            uint64_t i = valid[k];
            int tx = (int)(i % (uint64_t)n), ty = (int)(i / (uint64_t)n);
            VtTile t;
            auto t0 = std::chrono::steady_clock::now();
            bool ok = c.readTile(L, tx, ty, t);
            auto t1 = std::chrono::steady_clock::now();
            if (!ok) continue;
            ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            sv += t.vertexCount();
            sr += (long long)t.rings.size();
            sbytes += (long long)slots[i].size;
            ++cnt;
        }
        double avgV = cnt ? (double)sv / cnt : 0;
        double avgR = cnt ? (double)sr / cnt : 0;
        double usPer = cnt ? ms * 1000.0 / cnt : 0;
        double mbps = ms > 0 ? (sbytes / 1048576.0) / (ms / 1000.0) : 0;
        printf("%-3d %8llu %8llu %13.1f %13.0f %10.1f %11.1f %10.1f\n",
               L, (unsigned long long)valid.size(), (unsigned long long)sc,
               avgV, avgV * (double)valid.size(), avgR, usPer, mbps);
    }
    return 0;
}
