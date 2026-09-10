#pragma once
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "data/vector_reader.h"
#include "config/app_config.h"

namespace peekg::data {
// 多文件并行矢量加载器(纯数据生产, 不接触场景/渲染/UI):
//  调用方 enqueue(): 自己负责去重 + 建"占位图层"(名字/数量立刻可见) + 分配全局图层索引,
//                    然后把 meta(图层名/要素数)与全局基址交给本类
//  N 个工作线程并发取任务(每任务内部再按层/FID 并行读 GDAL), 产出"块事件"流回主线程
//  主线程 poll(): 每帧取回完成事件 + 块事件, 由调用方应用到场景/后端
struct LoadStats {
    int activeFiles = 0;      // 排队+进行中
    int doneFiles = 0;        // 已完成
    int totalFiles = 0;       // 本次会话累计入队
    long long processed = 0;  // 累计处理要素
    long long total = 0;      // 累计总要素
    double fraction() const { return total > 0 ? (double)processed / (double)total : -1.0; }
};

class AsyncLoader {
public:
    // 完成事件(poll 取回, 供调用方收尾: 状态提示/重建/范围适配/空图层补偿)
    struct LoadEvent {
        std::string path;
        std::string msg;
        bool failed = false;
        int globalBase = 0;
        std::vector<int> layerIndices;
    };

    // 块事件(worker -> 主线程): 元信息每块都携带(轻量), 首次到达即配置图层 CRS
    struct ChunkEvent {
        int globalIdx = 0;
        int srcEpsg = 0;
        std::string name, crs;
        std::vector<float> verts, pts, tris;   // 源 CRS(线边界/点/面填充三角形)
        bool full = false;                     // 整层几何单块(旧缓存命中/兜底补读)
        bool cacheChunk = false;               // 缓存命中逐块: 每块=缓存写库块, 直接成桶(不整层累积)
        int rebuildEpsg = 0;                   // >0: 块桶层 CRS 重建, 目标 CRS(=块重投影目标)
        double minx = 0, miny = 0, maxx = 0, maxy = 0;  // 整层的源CRS范围(仅 full / cacheChunk)
    };

    ~AsyncLoader();

    // 主线程: 入队一个文件。调用方已建好占位图层(去重+分配全局索引), 此处只负责任务本身。
    // meta 来自 readLayerMetadata(图层名/要素数), layerIndices 为源图层索引(空=全部),
    // globalBase 为占位图层的全局索引基址。返回 globalBase(入队失败返回 -1)。
    int enqueue(const std::string& path, const AppConfig& cfg, const std::vector<LayerMeta>& meta,
                const std::vector<int>& layerIndices, int globalBase);

    // 块桶层 CRS 重建: 从缓存逐块重读几何(源CRS) -> 推回主线程按 targetEpsg 重投影成新桶。
    // world sourceLayerIdx 为源文件中该层层号; 结果块事件带 rebuildEpsg=targetEpsg。
    void enqueueRebuild(const std::string& path, int globalIdx, int sourceLayerIdx, int targetEpsg);

    void cancel();                                        // 放弃排队任务 + 停掉进行中任务
    void poll(std::vector<LoadEvent>& done, std::vector<ChunkEvent>& outChunks);  // 每帧取回事件
    LoadStats stats() const;

private:
    struct Task {
        int gen = 0;
        std::string path;
        AppConfig cfg;
        std::vector<int> layerIndices;   // 源图层索引(显式)
        int globalBase = 0;              // 本任务全局图层基址
        std::vector<std::string> names;  // 图层名(占位)
        std::vector<long long> counts;   // 要素数(占位/进度)
        std::atomic<long long> total{0};
        std::atomic<bool> done{false};
        std::atomic<bool> failed{false};
        std::string resultMsg;
        bool rebuild = false;            // 块桶层 CRS 重建任务(从缓存重读)
        int rebuildEpsg = 0;             // 重建目标 CRS
    };

    static const int kMaxConcurrent = 4;     // 文件级并行度(每任务内部再并行)

    void runTask(std::shared_ptr<Task> t);
    void runRebuild(std::shared_ptr<Task> t);   // 块桶层 CRS 重建: 缓存逐块重读
    void stopWorkers();
    void keepAttrDataset(const std::string& path, int gen);
    void finishOk(std::shared_ptr<Task> t, const std::string& msg);
    void finishFail(std::shared_ptr<Task> t, const std::string& msg);
    void pushFullLayer(int gi, VectorData&& vd);   // 整层几何单块(缓存命中): 移动语义零拷贝

    mutable std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Task>> tasks;    // 排队(未被 worker 取走)
    std::deque<std::shared_ptr<Task>> finished; // 已完成(供主线程消费状态/计数)
    std::vector<std::thread> workers;
    std::atomic<int> m_gen{1};
    bool stopping = false;

    std::deque<ChunkEvent> chunks;              // 待主线程消费
    std::atomic<long long> m_aggProcessed{0};
    std::atomic<long long> m_aggTotal{0};
    int m_totalTasks = 0;       // 会话累计入队(受 mtx 保护)
    int m_doneTasks = 0;        // 会话累计完成(受 mtx 保护)
};
}  // namespace peekg::data