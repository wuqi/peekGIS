#pragma once
// v2 矢量瓦片渲染: 视口选层 + 可见瓦片后台读取/(可选)重投影/earcut + 主线程上传 + LRU。
// 瓦片烘焙在 cache 的 dstEpsg; 当显示 CRS 不同时在 worker 里整块重投影到显示 CRS(方案b)。
#include "map/map_scene.h"
#include "vt/vt_cache.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class VtRenderer {
public:
    struct GpuTile {
        unsigned vao = 0, vbo = 0;
        long long vcount = 0, pcount = 0, fcount = 0;   // 线 / 点 / 面填充(依次拼接)
        long long lastUse = 0;
        long long bytes = 0;                            // 顶点缓冲字节数(内存淘汰用)
        double originX = 0, originY = 0, cell = 1;      // 净区左下角(显示CRS) + 格距(scissor 用)
    };
    struct Layer {
        std::shared_ptr<peekg::vt::VtCache> cache;
        std::string path;
        int sceneLayerIdx = -1;
        int srcEpsg = 0, dstEpsg = 0;     // dstEpsg = 缓存烘焙所在 CRS
        int renderEpsg = 0;               // 当前 VBO 实际所在 CRS(显示CRS)
        int maxLevel = 0, curLevel = -1;
        double originX = 0, originY = 0, tileW0 = 1;
        std::unordered_map<uint64_t, GpuTile> tiles;
        size_t tileLimit = 512;
        bool building = false;            // 构建中: 只渲染显式投递的瓦片(边建边看)
        long long bytes = 0;              // 已驻留顶点字节
    };

    VtRenderer() = default;
    ~VtRenderer();
    VtRenderer(const VtRenderer&) = delete;
    VtRenderer& operator=(const VtRenderer&) = delete;

    int addLayer(const std::string& cachePath, int sceneLayerIdx, int srcEpsg, int dstEpsg);
    void removeLayer(int idx);
    void clear();
    bool hasLayer(int idx) const;
    int layerCount() const { return (int)layers_.size(); }

    // 场景删除某下标图层时调用: 移除对应 vt 层, 其余 sceneLayerIdx 前移(保证颜色/可见性不错位)
    void onSceneLayerRemoved(int sceneIdx);

    // 构建中模式: 只渲染显式投递的瓦片(边建边看), 不做视口选层; 结束恢复视口模式
    void setBuilding(int idx, bool b);
    void requestTile(int idx, int level, int tx, int ty);   // 主线程: 投递单瓦片读取任务

    // 每帧: 选层/可见瓦片, 缺片投递后台任务, 收结果上传(逐帧预算), LRU 淘汰
    void sync(const MapScene& scene, int texW, int texH, long long frameNo);
    // 绘制(调用方需已 glUseProgram 矢量 program)
    void drawFill(unsigned program, int locColor, int locAlpha, const MapScene& scene);
    void drawLines(unsigned program, int locColor, int locAlpha, const MapScene& scene);

    size_t residentTiles() const;
    size_t pendingJobs() const;
    long long drawnVerts() const { return drawnVerts_; }

private:
    struct Job {
        std::shared_ptr<peekg::vt::VtCache> cache;
        int layer = -1, level = 0, tx = 0, ty = 0;
        double cell = 1.0;
        int fromEpsg = 0, toEpsg = 0;
        uint64_t gen = 0;
    };
    struct Result {
        int layer = -1, level = 0, tx = 0, ty = 0;
        long long vcount = 0, pcount = 0, fcount = 0;
        std::vector<float> data;
        uint64_t gen = 0;
    };

    void ensureWorker();
    void stopWorker();
    void workerLoop();
    void releaseTile(GpuTile& g, long long& bytes);
    void bumpGen();   // 结构变化: 作废在途任务结果

    std::vector<Layer> layers_;
    long long frame_ = 0;
    long long drawnVerts_ = 0;
    uint64_t gen_ = 1;
    long long buildByteBudget_ = 256LL << 20;   // 构建中渲染的显存/内存上限(256MB)
    int texW_ = 0, texH_ = 0;                   // 最近一帧 FBO 尺寸(scissor 映射用)

    std::thread worker_;
    std::atomic<bool> stop_{false};
    bool workerStarted_ = false;
    std::mutex jobMtx_;
    std::condition_variable jobCv_;
    std::deque<Job> jobs_;
    std::mutex resMtx_;
    std::deque<Result> results_;
    std::set<uint64_t> inflight_;   // 主线程独占; key = layer<<56 | tileKey
};
