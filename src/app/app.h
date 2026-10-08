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
#include "vt/vt_source.h"

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

    // 要素查询任务: fid<0 表示只要字段定义(绑定图层时), 不查具体要素
    struct QueryTaskSpec {
        std::string path;
        int layerIdx = 0;
        long long fid = -1;
        int enc = 0;
        int gen = 0;
    };

    // ---- 后台 worker 循环 ----
    void rasterLoaderWorker();
    void rasterBlockWorker();
    void pumpRasterDetail(const MapScene& scene, GLBackend& backend);
    void refreshRasterQueue();
    bool launchAttrTask(AttrTaskSpec spec);
    bool launchQueryTask(QueryTaskSpec spec);
    void applyAttrPage(UIState& ui, const AttrLayerInfo& info, AttrPageData pd);
    void applyLoaderEvents();   // 消费 AsyncLoader 的完成/块事件, 应用到 scene/backend
    int queueVector(const std::string& path, const std::vector<LayerMeta>& meta,
                    const std::vector<int>& layerIndices);   // 去重 + 建占位图层 + 入队
    bool openVtFile(const std::string& path, const std::string& displayName = "", const std::string& srcPath = "");   // v2 矢量瓦片缓存(.vtk)直接打开
    bool tryOpenVtForSource(const std::string& path);   // 打开源文件时自动发现已建的 v2 缓存
    // 大文件无缓存: 后台生成 v2 缓存。srcPath/estVerts/li 由后台探测算好传入(UI 线程不做 GDAL 调用)
    bool tryAutoVtBuild(const std::string& path, const std::string& srcPath, long long estVerts,
                        const peekg::vt::LayerInfo& li);

    void loadRecent();                                  // 从 <cache>/recent.txt 载入最近打开
    void saveRecent();                                  // 落盘最近打开(最多 5 条)
    void addRecent(const std::string& source);          // 去重置顶(PG 串脱敏后存)

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

    // ---- 打开探测(后台): 决定"走缓存/走 v2/走 v1.0", 不占 UI 线程 ----
    // 之前这些决策(找缓存、全表 COUNT、采样 5 万要素)是**同步跑在 UI 线程**上的,
    // 大表/压缩包能冻住整个窗口(Windows 判无响应), 而且按钮也点不到 —— 界面卡死时
    // 没有任何办法打断。改成后台做, 主线程只应用结论。
    struct OpenProbeResult {
        bool done = false;
        bool cancelled = false;
        bool openFailed = false;
        std::string srcPath;        // 实际可打开的源路径(zip 时是 /vsizip/... 形式)
        bool haveVtCache = false;   // 已有匹配的 v2 缓存 -> 直接开缓存
        std::string vtCachePath;
        bool shouldAutoBuild = false;   // 大文件且无缓存 -> 走 v2 后台建缓存
        long long estVerts = -1;
        peekg::vt::LayerInfo li;
    };

    void startOpenProbe(const std::string& path);
    void applyOpenProbe();   // 主线程: 只做 scene/backend 变更, 不碰 GDAL

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
    std::atomic<bool> metaCancel_{false};   // 用户点"取消" -> 预读线程见到就丢弃结果, 不再弹图层选择框
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

    // ---- 打开探测共享态(后台线程做 GDAL 决策, 主线程每帧取回结论) ----
    std::thread probeThread_;
    std::mutex probeMtx_;
    std::atomic<bool> probeRunning_{false};
    std::atomic<bool> probeCancel_{false};     // 用户点"取消" -> 置位
    OpenProbeResult probeResult_;
    std::string probePath_;                    // 正在探测的路径(状态栏显示)
    // vt 构建也能取消(与探测分开: 探测是"决定", 构建是"干活")
    std::atomic<bool> vtStop_{false};

    // ---- 要素查询(按 FID 回源) ----
    // 与属性表共用 per-path keeper 锁(attrFetchByFid 内部), 所以这里是**单飞**的:
    // 同一时刻只允许一个查询在途, 否则两次回源会互相等那把锁, 表现为界面"卡一下"。
    std::mutex qryMtx;
    bool qryBusy = false;
    bool qryReady = false;
    int qryResultGen = -1;
    peekg::data::AttrQueryResult qryRes = peekg::data::AttrQueryResult::Error;
    peekg::data::AttrRow qryRow;
    peekg::data::AttrLayerInfo qryInfo;
    std::atomic<int> qryGen{0};

    // ---- v2 后台建缓存 + 边建边看 ----
    std::thread vtThread_;
    std::atomic<bool> vtBuilding_{false};
    std::atomic<int> vtPct_{0};
    std::atomic<bool> vtDone_{false};
    std::atomic<bool> vtOk_{false};
    std::mutex vtMtx_;
    std::string vtOut_, vtName_, vtSrcPath_;
    int vtSceneIdx_ = -1;        // 构建期占位场景图层下标
    int vtHandle_ = -1;          // backend vt 层 handle
    int vtSrcEpsg_ = 0;
    double vtBbox_[4] = {0, 0, 0, 0};   // 构建期占位框范围
    std::atomic<int> vtDisplayLevel_{-1};   // 构建线程: 最近落盘的层(仅状态栏显示用)
    bool vtLayerReady_ = false;  // backend vt 层是否已挂上
    std::mutex vtReadyMtx_;
    std::vector<std::array<int, 3>> vtCover_;   // 构建线程触及+落盘的瓦片(实时覆盖框, 不等落盘)
    std::vector<std::array<int, 3>> vtMerge_;   // 拓扑后处理(合并/糊化)完成的瓦片(覆盖框换色用)
};
