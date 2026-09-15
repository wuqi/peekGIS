#pragma once
#include <string>
#include <vector>
#include <deque>
#include <array>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <cstdint>

#include "map/map_scene.h"
#include "render/gl_backend.h"
#include "config/app_config.h"
#include "app/ui.h"
#include "data/async_loader.h"

struct GLFWwindow;

// 应用主体: 持有场景/渲染后端/异步加载器/UI 状态, 以及各处后台 worker 的共享态
// (栅格底图、细节块、属性表、识别、元数据预读), 每帧由 frame() 统一驱动。
// 原来散落在 main.cpp 的文件级静态与脱离主线程的闭包全部收进本类,
// 后台线程归属字段 bgThreads_ 在 shutdown() 时统一 join, 避免析构后仍写成员。
class App {
public:
    explicit App(AppConfig& cfg);
    ~App();

    void frame(GLFWwindow* window);   // 一帧: 轮询事件 + 消费请求/后台结果 + 渲染 + 交换
    void shutdown();                  // 停 worker + join 后台线程(可与 dtor 重复调用)
    void deferOpen(const std::string& p);   // --after: 前一个文件加载完成后再打开

    UIState ui;

private:
    struct IdentifyTarget {
        std::string path;
        int layerIdx = 0;
        int srcEpsg = 0;
        bool isRaster = false;
        std::string openSpec;       // 栅格: 打开名(subdataset 或含 GDAL 子名)
        double geo[6] = {0,0,0,0,0,0};
        int width = 0, height = 0;
    };

    struct RasterLoadJob {
        std::string spec;           // 打开名(文件或 subdataset)
        RasterRenderOptions opts;   // 渲染选项(rebase 重渲染用)
        int rebaseHandle = -1;      // 设置变更重渲染: 复用该图层 handle
    };

    struct RasterBlockJob {
        std::string openSpec;
        peekg::data::RasterRgbAssemble as;
        int gen = 0;
        int x0=0, y0=0, w0=0, h0=0, dw=256, dh=256;
        int span=0, sw=0, sh=0;
        int tx=0, ty=0;
        int rasterHandle = -1;
    };

    struct RasterBlockResult {
        int rasterHandle;
        int gen = 0;
        int span, tx, ty;
        int sw=0, sh=0;
        std::vector<unsigned char> rgba;
        int w=0, h=0;
    };

    struct AttrTaskSpec {
        std::string path;
        int layerIdx = 0;
        int page = 0;
        int rowsPerPage = 20;
        int enc = 0;
        int gen = 0;
        bool needCount = false;
    };

    // ---- 后台 worker 循环 ----
    void rasterLoaderWorker();
    void rasterBlockWorker();
    void pumpRasterDetail(const MapScene& scene, GLBackend& backend);
    void refreshRasterQueue();
    bool launchAttrTask(AttrTaskSpec spec);
    void applyAttrPage(UIState& ui, const AttrLayerInfo& info, AttrPageData pd);
    void applyLoaderEvents();   // 消费 AsyncLoader 的完成/块事件, 应用到 scene/backend
    int queueVector(const std::string& path, const std::vector<LayerMeta>& meta,
                    const std::vector<int>& layerIndices);   // 去重 + 建占位图层 + 入队
    bool openVtFile(const std::string& path, const std::string& displayName = "", const std::string& srcPath = "");   // v2 矢量瓦片缓存(.vtk)直接打开
    bool tryOpenVtForSource(const std::string& path);   // 打开源文件时自动发现已建的 v2 缓存
    bool tryAutoVtBuild(const std::string& path);       // 大文件无缓存: 后台生成 v2 缓存

    static bool isRasterExt(const std::string& ext);
    static std::string baseName(const std::string& p);

    AppConfig& cfg;

    MapScene scene;
    GLBackend backend;
    peekg::data::AsyncLoader loader;
    bool loaderFirstDataRefit = false;   // 本会话加载是否已做过首次视图适配
    int openSeqCounter_ = 0;             // 图层打开顺序计数器(自动定位归属判定)
    int fitOwnerSeq_ = -1;               // 当前自动定位归属的打开序号(-1=尚未自动定位)
    std::vector<int> rebuildQueued;       // 块桶层 CRS 重建已入队目标(下标=图层 index, 0=未入队)

    // ---- CLI --after 顺序加载 ----
    std::vector<std::string> deferredOpen;
    std::deque<std::string> pendingOpen;
    bool deferredArmed = false;

    // ---- 异步属性识别共享态(双击抓拍目标清单, 后台线程逐层查询, 主线程每帧取回) ----
    std::mutex identifyMtx;
    std::atomic<uint64_t> identifyGen{0};
    std::vector<IdentifyHit> identifyGeo;
    bool identifyGeoReady = false;
    int identifyRemaining = 0;
    bool identifyDone = false;

    // ---- 后台图层元数据预读(non-shp 多图层判断) ----
    std::mutex metaMtx;
    std::string metaPath;
    std::vector<LayerMeta> metaResult;
    bool metaOk = false;
    std::atomic<bool> metaDone{false};
    std::atomic<bool> sdsFlag{false};

    // ---- 后台栅格底图读取(队列模型, 支持多 subdataset) ----
    std::mutex rasterMtx;
    std::deque<RasterLoadJob> pendingRasterSpecs;
    std::vector<RasterData> rasterMeta;
    std::vector<std::vector<unsigned char>> rasterRgba;
    std::vector<std::array<int,2>> rasterDims;
    std::vector<int> rasterBandW;
    std::vector<int> rasterRebase;
    std::atomic<bool> rasterDone{false};
    std::atomic<bool> rasterPing{false};
    std::condition_variable rasterCv;
    std::thread rasterWorker;
    std::atomic<bool> rasterWorkerOn{false};
    std::atomic<bool> rasterStop{false};
    std::atomic<bool> rebasePending{false};

    // ---- 细节块后台读取 ----
    std::mutex blockMtx;
    std::condition_variable blockCv;
    std::deque<RasterBlockJob> blockJobs;
    std::deque<RasterBlockResult> blockResults;
    std::set<long long> blockInflight;
    std::atomic<int> blockGen{0};
    std::atomic<bool> blockStop{false};
    std::thread blockWorker;
    std::atomic<bool> blockStarted{false};

    // ---- 后台属性表读取 ----
    std::mutex attrMtx;
    bool attrBusy = false;
    bool attrInfoReady = false;
    AttrLayerInfo attrFetchedInfo;
    bool attrPageReady = false;
    AttrPageData attrFetchedPage;
    bool attrCountReady = false;
    long long attrFetchedCount = -1;
    bool attrNeedCount = false;
    int attrResultGen = -1;
    std::atomic<int> attrGen{0};
    int attrOpenRetries = 0;

    std::vector<std::thread> bgThreads_;   // 识别/元数据/属性等临时线程, 析构前 join

    // ---- v2 后台建缓存 + 边建边看 ----
    std::thread vtThread_;
    std::atomic<bool> vtBuilding_{false};
    std::atomic<int> vtPct_{0};
    std::atomic<bool> vtDone_{false};
    std::atomic<bool> vtOk_{false};
    std::mutex vtMtx_;
    std::string vtOut_, vtName_;
    int vtSceneIdx_ = -1;        // 构建期占位场景图层下标
    int vtHandle_ = -1;          // backend vt 层 handle
    int vtSrcEpsg_ = 0;
    double vtBbox_[4] = {0, 0, 0, 0};   // 构建期占位框范围
    int vtBuildLevel_ = -1;      // 构建期渲染器当前显示的层
    std::atomic<int> vtDisplayLevel_{-1};   // 构建线程: 当前正在产出的层(阶段B 逐层下降)
    bool vtLayerReady_ = false;  // backend vt 层是否已挂上
    std::mutex vtReadyMtx_;
    std::vector<std::array<int, 3>> vtReady_;   // 构建线程产出 (level,tx,ty)
};