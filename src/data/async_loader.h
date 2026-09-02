#pragma once
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "data/gdal_datasource.h"
#include "config/app_config.h"
#include "map/map_scene.h"
#include "render/gl_backend.h"
#include "app/ui.h"
#include <gdal.h>

// 多文件并行矢量加载器:
//  主线程 enqueue(): 同路径去重 + 建"占位图层"(名字/数量立刻可见) + 分配全局图层索引 + 入队
//  N 个工作线程并发取任务(每任务内部再按层/FID 并行读 GDAL), 产出"块事件"流回主线程
//  主线程 update(): 每帧增量 append 到 VBO —— 不再整层快照、不再每 40ms 整体重传
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
    ~AsyncLoader();

    // 主线程: 入队一个文件(meta 来自 readLayerMetadata, layerIndices 为源图层索引, 空=全部)。
    // 立即在 scene/backend 上完成同路径去重 + 建占位图层, 返回第一个全局图层索引。
    int enqueue(const std::string& path, const AppConfig& cfg, MapScene& scene, GLBackend& backend,
                const std::vector<LayerMeta>& meta, const std::vector<int>& layerIndices);

    void cancel();                                        // 放弃排队任务 + 停掉进行中任务
    void update(MapScene& scene, GLBackend& backend, UIState& ui);  // 每帧消费块事件
    LoadStats stats() const;

private:
    struct Task {
        int id = 0;
        int gen = 0;
        std::string path;
        AppConfig cfg;
        std::vector<int> layerIndices;   // 源图层索引(显式)
        int globalBase = 0;              // 本任务全局图层基址
        std::vector<std::string> names;  // 图层名(占位)
        std::vector<long long> counts;   // 要素数(占位/进度)
        std::atomic<long long> processed{0};
        std::atomic<long long> total{0};
        std::atomic<bool> done{false};
        std::atomic<bool> failed{false};
        std::string resultMsg;
    };

    static const int kChunkFeature = 20000;  // GDAL 顺序读一块的要素数
    static const int kMaxConcurrent = 4;     // 文件级并行度(每任务内部再并行)

    // 块事件(worker -> 主线程): 元信息每块都携带(轻量), 首次到达即配置图层 CRS
    struct ChunkEvent {
        int globalIdx = 0;
        int srcEpsg = 0;
        std::string name, crs;
        std::vector<float> verts, pts, tris;   // 源 CRS(线边界/点/面填充三角形)
    };

    void runTask(std::shared_ptr<Task> t);
    void stopWorkers();
    void keepAttrDataset(const std::string& path, int gen);
    void finishOk(std::shared_ptr<Task> t, const std::string& msg);
    void finishFail(std::shared_ptr<Task> t, const std::string& msg);
    void pushFullLayer(int gi, const VectorData& vd);

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

    bool m_firstDataRefit = false;     // 本批加载是否已做过首次视图适配
};