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

namespace peekg::vt {
class RawRegionStream;   // 超 Lmax 直读源数据流(vt_build.h, 头文件只透前向声明)
}

class VtRenderer {
public:
    struct GpuTile {
        unsigned vao = 0, vbo = 0;
        long long vcount = 0, pcount = 0, fcount = 0;   // 线 / 点 / 面填充(依次拼接)
        long long lastUse = 0;
        long long bytes = 0;                            // 顶点缓冲字节数(内存淘汰用)
        double originX = 0, originY = 0, cell = 1;      // 净区左下角(显示CRS) + 格距(scissor 用)
        int tileSize = peekg::vt::TILE_SIZE;            // 该层净区格数(最深层 1024, 其余 512)
    };
    struct Layer {
        std::shared_ptr<peekg::vt::VtCache> cache;
        std::string path;
        std::string srcPath;          // 原始矢量源文件(超 Lmax 直读用; 空 = 不直读)
        int sceneLayerIdx = -1;
        int srcEpsg = 0, dstEpsg = 0;     // dstEpsg = 缓存烘焙所在 CRS
        int renderEpsg = 0;               // 当前 VBO 实际所在 CRS(显示CRS)
        int maxLevel = 0, curLevel = -1;
        double originX = 0, originY = 0, tileW0 = 1;
        std::unordered_map<uint64_t, GpuTile> tiles;
        size_t tileLimit = 512;
        bool building = false;            // 构建中: 只渲染显式投递的瓦片(边建边看)
        long long bytes = 0;              // 已驻留顶点字节
        bool hasBbox = false;             // 构建期占位框(数据范围)
        double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        // 构建期"已建覆盖"粗网格: 把建好的瓦片涂成图层色, 随构建长出来
        std::vector<uint8_t> cov;
        int covN = 0;
        bool covDirty = false;
        unsigned covVao = 0, covVbo = 0;
        long long covVerts = 0;

        // ---- 超 Lmax 直读状态 ----
        bool rawEnabled = true;           // 配置: 允许直读源
        double rawBudgetMs = 150.0;       // 配置: 单遍可见区读盘预算
        bool rawDisabled = false;         // 判死: 一次超预算即停用直读(app 打开会话内不重试)
        bool rawActive = false;           // 当前处于直读流模式
        bool rawBusy = false;             // 一块直读 chunk 在途(防重入)
        bool rawDone = false;             // 本遍(该区域)已读完
        int rawLevel = -1;                // 直读层(>maxLevel)
        uint64_t rawGen = 0;              // 直读代际: 区域变更/退出时 ++, 丢弃在途旧结果
        double rawEma = -1.0;             // EWMA 毫秒/要素(自适应读盘效率)
        long long rawScanned = 0;         // 本遍累计扫描要素
        double rawMs = 0;                 // 本遍累计耗时(ms)
        std::shared_ptr<peekg::vt::RawRegionStream> raw;   // 本遍直读流(in-flight job 持拷贝, 替换安全)
        int rawRgX0 = 0, rawRgY0 = 0, rawRgX1 = -1, rawRgY1 = -1;  // 本遍区域(rawLevel 瓦片下标)
    };

    VtRenderer() = default;
    ~VtRenderer();
    VtRenderer(const VtRenderer&) = delete;
    VtRenderer& operator=(const VtRenderer&) = delete;

    int addLayer(const std::string& cachePath, int sceneLayerIdx, int srcEpsg, int dstEpsg,
                 const std::string& srcPath = "");
    void removeLayer(int idx);
    void clear();
    bool hasLayer(int idx) const;
    int layerCount() const { return (int)layers_.size(); }
    // 某缓存文件是否正被渲染层持有(删除缓存前判断: Windows 下开着删不掉)
    bool holdsPath(const std::string& path) const;

    // 场景删除某下标图层时调用: 移除对应 vt 层, 其余 sceneLayerIdx 前移(保证颜色/可见性不错位)
    void onSceneLayerRemoved(int sceneIdx);

    // 构建中模式: 只渲染显式投递的瓦片(边建边看), 不做视口选层; 结束恢复视口模式
    void setBuilding(int idx, bool b);
    void requestTile(int idx, int level, int tx, int ty);   // 主线程: 投递单瓦片读取任务
    // 构建期: 点亮某瓦片对应的覆盖框(256 粗网格), 不读缓存。构建线程实时上报进度用。
    void markBuildTile(int idx, int level, int tx, int ty);
    // 构建期占位框(显示 CRS 数据范围): 建缓存时地图上至少能看到范围
    void setPlaceholderBbox(int idx, double x0, double y0, double x1, double y1);

    // 每帧: 选层/可见瓦片, 缺片投递后台任务, 收结果上传(逐帧预算), LRU 淘汰
    void sync(const MapScene& scene, int texW, int texH, long long frameNo);

    // 超 Lmax 直读开关与预算(app_config [vt] raw_over_max / raw_budget_ms 透传)
    void setRawConfig(bool enabled, double budgetMs);
    // 绘制(调用方需已 glUseProgram 矢量 program)
    void drawFill(unsigned program, int locColor, int locAlpha, const MapScene& scene);
    void drawLines(unsigned program, int locColor, int locAlpha, const MapScene& scene);

    size_t residentTiles() const;
    size_t pendingJobs() const;
    long long drawnVerts() const { return drawnVerts_; }
    int displayLevel() const { return layers_.empty() ? -1 : layers_[0].curLevel; }   // 状态栏显示当前层
    // 当前是否处于超 Lmax 直读原始数据(状态栏据此显示"原始数据"而非缓存层)
    bool rawReading() const {
        for (const auto& L : layers_)
            if (L.rawActive) return true;
        return false;
    }
    int rawReadLevel() const {
        for (const auto& L : layers_)
            if (L.rawActive) return L.rawLevel;
        return -1;
    }

private:
    struct Job {
        std::shared_ptr<peekg::vt::VtCache> cache;
        int layer = -1, level = 0, tx = 0, ty = 0;
        double cell = 1.0;
        double scale = 0;   // 请求时视图 scale(算亚像素填充阈值)
        int fromEpsg = 0, toEpsg = 0;
        uint64_t gen = 0;
        // 超 Lmax 直读 chunk: raw 非空时按直读流分块读(rather than cache), 产出 rawStat 结果+瓦片
        std::shared_ptr<peekg::vt::RawRegionStream> raw;
        long long rawMaxFeat = 0;
        double rawMaxMs = 0;
        uint64_t rawGen = 0;
    };
    struct Result {
        int layer = -1, level = 0, tx = 0, ty = 0;
        long long vcount = 0, pcount = 0, fcount = 0;
        std::vector<float> data;
        uint64_t gen = 0;
        // 超 Lmax 直读统计(非瓦片几何; rawDone 表示该遍读到 EOF)
        bool rawStat = false;
        bool rawDone = false;
        long long rawScanned = 0;
        double rawMs = 0;
        uint64_t rawGen = 0;
    };

    void ensureWorker();
    void stopWorker();
    void workerLoop();
    void releaseTile(GpuTile& g, long long& bytes);
    void releaseCoverage(Layer& L);   // 释放构建期覆盖网格/占位框
    void bumpGen();   // 结构变化: 作废在途任务结果
    void drawRect(float x0, float y0, float x1, float y1, bool filled);   // 占位框(复用 phVao_)
    void uploadResults();             // 收后台结果并上传 VBO(主线程 GL)
    void evictBuildingTiles(Layer& L);   // 构建期按内存预算淘汰
    void updateViewportTiles(Layer& L, size_t li, const MapScene& scene, bool& queued);   // 视口选层+投递
    void dispatchRawChunk(Layer& L, size_t li, const MapScene& scene, bool& queued);      // 投递一块直读
    void enterRaw(Layer& L, size_t li, const MapScene& scene, bool& queued, double scale);
    void disableRaw(Layer& L);   // 回退 Lmax 缓存: 关流/清直读片/永久判死(会话内不重试)
    void exitRaw(Layer& L);      // 退出直读但保留能力(缩回缓存层/层隐藏): 只清直读状态与直读片
    int wantedRawLevel(double scale, const Layer& L) const;   // 期望直读层(封顶)

    std::vector<Layer> layers_;
    long long frame_ = 0;
    long long drawnVerts_ = 0;
    uint64_t gen_ = 1;
    bool rawCfgEnabled_ = true;             // 直读总开关(配置透传; 新层采用)
    double rawCfgBudgetMs_ = 150.0;         // 单遍可见区读盘预算(ms)
    long long buildByteBudget_ = 256LL << 20;   // 构建中渲染的显存/内存上限(256MB)
    int texW_ = 0, texH_ = 0;                   // 最近一帧 FBO 尺寸(scissor 映射用)
    unsigned phVao_ = 0, phVbo_ = 0;            // 占位框动态 VBO

    std::vector<std::thread> workers_;   // 多线程并行构建瓦片几何(读+earcut+描边)
    std::atomic<bool> stop_{false};
    bool workerStarted_ = false;
    mutable std::mutex jobMtx_;   // pendingJobs() 等 const 查询需要
    std::condition_variable jobCv_;
    std::deque<Job> jobs_;
    mutable std::mutex resMtx_;
    std::deque<Result> results_;
    std::set<uint64_t> inflight_;   // 主线程独占; key = layer<<56 | tileKey
};
