#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // 必须先于 GLFW/glfw3.h, 避免 APIENTRY 宏重定义(C4005)
#endif
#include "app/app.h"
#include "data/gdal_common.h"
#include "data/geom_util.h"
#include "data/vector_reader.h"
#include "data/raster_reader.h"
#include "data/geoloc.h"
#include "data/attr_table.h"
#include "data/reproject.h"
#include "util/logger.h"
#include "vt/vt_cache.h"
#include "vt/vt_source.h"
#include "vt/vt_build.h"
#include "platform/exe_path.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <cstdio>
#include <cmath>
#include <random>
#include <chrono>
#include <memory>
#include <algorithm>
#include <cctype>
#include <filesystem>

using namespace peekg::data;

// 16 色视觉区分色板(避免相邻图层颜色相近); 占位图层配色由 App 负责(原 AsyncLoader 内)
static const float kPalette[16][3] = {
    {0.22f, 0.49f, 0.73f}, {0.90f, 0.30f, 0.24f}, {0.16f, 0.68f, 0.37f}, {0.95f, 0.61f, 0.07f},
    {0.58f, 0.40f, 0.74f}, {0.17f, 0.63f, 0.68f}, {0.85f, 0.75f, 0.28f}, {0.80f, 0.36f, 0.53f},
    {0.36f, 0.55f, 0.20f}, {0.60f, 0.35f, 0.18f}, {0.45f, 0.45f, 0.45f}, {0.55f, 0.75f, 0.25f},
    {0.30f, 0.30f, 0.70f}, {0.90f, 0.50f, 0.60f}, {0.20f, 0.80f, 0.60f}, {0.70f, 0.55f, 0.85f},
};
static int s_layerColorIdx = 0;

std::string App::baseName(const std::string& p) {
    size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

bool App::isRasterExt(const std::string& ext) {
    return ext == ".tif" || ext == ".tiff" || ext == ".img" || ext == ".png" ||
           ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".nc" ||
           ext == ".vrt" || ext == ".hdf" || ext == ".h5" || ext == ".hdf5";
}

App::App(AppConfig& c) : cfg(c) {
    backend.init();   // 需在 GL 上下文就绪后(由 main 保证)
}

App::~App() { shutdown(); }

void App::deferOpen(const std::string& p) { deferredOpen.push_back(p); }

void App::shutdown() {
    if (blockStarted.load()) {
        blockStop.store(true);
        blockCv.notify_all();
        if (blockWorker.joinable()) blockWorker.join();
        blockStarted.store(false);
    }
    if (rasterWorkerOn.load()) {
        rasterStop.store(true);
        rasterCv.notify_all();
        if (rasterWorker.joinable()) rasterWorker.join();
        rasterWorkerOn.store(false);
    }
    for (auto& t : bgThreads_) if (t.joinable()) t.join();
    bgThreads_.clear();
    if (vtThread_.joinable()) vtThread_.join();
}

// 后台 worker: 不断取 spec, 读元数据 + 组装底图, 结果入 rasterMeta...
void App::rasterLoaderWorker() {
    for (;;) {
        RasterLoadJob job;
        {
            std::unique_lock<std::mutex> lk(rasterMtx);
            rasterCv.wait(lk, [&]() { return rasterStop.load() || !pendingRasterSpecs.empty(); });
            if (rasterStop.load() && pendingRasterSpecs.empty()) return;
            if (pendingRasterSpecs.empty()) continue;
            job = pendingRasterSpecs.front();
            pendingRasterSpecs.pop_front();
        }
        const std::string& spec = job.spec;
        RasterData rd;
        bool ok = readRasterMetadata(spec, rd);
        std::vector<unsigned char> rgba; int w = 0, h = 0; int mode = 0;
        if (ok) {
            double maxd = std::max((long long)rd.width, (long long)rd.height);
            double scl = std::min(1.0, 1024.0 / (double)maxd);
            int dw = std::max(1, (int)std::lround(rd.width * scl));
            int dh = std::max(1, (int)std::lround(rd.height * scl));
            RasterRgbAssemble as;
            const RasterRenderOptions& o = job.opts;
            if (o.mode == RasterRenderMode::RGB) { as.mode = RasterRgbAssemble::Mode::RGB; mode = 1; }
            else if (o.mode == RasterRenderMode::Pseudocolor) { as.mode = RasterRgbAssemble::Mode::Pseudocolor; mode = 2; }
            else { as.mode = RasterRgbAssemble::Mode::Gray; mode = 0; }
            as.b1 = (o.mode == RasterRenderMode::RGB) ? o.rBand : o.grayBand;
            as.b2 = o.gBand; as.b3 = o.bBand;
            as.colorMap = o.colorMap;
            as.mn = o.useAutoMinMax && !rd.bands.empty() && rd.bands[0].hasMinMax
                    ? rd.bands[0].min : o.minRaw;
            as.mx = o.useAutoMinMax && !rd.bands.empty() && rd.bands[0].hasMinMax
                    ? rd.bands[0].max : o.maxRaw;
            rgba = assembleRasterRgba(rd.openSpec, dw, dh, as, w, h);
            if (rgba.empty()) ok = false;
        }
        {
            std::lock_guard<std::mutex> lk(rasterMtx);
            if (ok) {
                rasterMeta.push_back(std::move(rd));
                rasterRgba.push_back(std::move(rgba));
                rasterDims.push_back(std::array<int,2>{w, h});
                rasterBandW.push_back(mode);
                rasterRebase.push_back(job.rebaseHandle);
                rasterDone.store(true);
            }
        }
    }
}

// 主线程按需唤醒栅格后台 worker(首次调用启动线程)
void App::refreshRasterQueue() {
    if (!rasterWorkerOn.load()) {
        rasterWorkerOn.store(true);
        rasterWorker = std::thread([this] { rasterLoaderWorker(); });
    }
    rasterCv.notify_all();
}

void App::rasterBlockWorker() {
    for (;;) {
        RasterBlockJob job;
        {
            std::unique_lock<std::mutex> lk(blockMtx);
            blockCv.wait(lk, [&]() { return blockStop.load() || !blockJobs.empty(); });
            if (blockStop.load()) return;
            job = blockJobs.front();
            blockJobs.pop_front();
        }
        int w=0, h=0;
        std::vector<unsigned char> rgba;
        if (job.dw <= 0) {
            rgba = assembleRasterWindowRgba(job.openSpec, job.as, job.x0, job.y0,
                                            job.w0, job.h0, 0, 0, w, h);
        } else {
            rgba = assembleRasterWindowRgba(job.openSpec, job.as, job.x0, job.y0,
                                            job.w0, job.h0, job.dw, job.dh, w, h);
        }
        if (rgba.empty()) continue;
        {
            std::lock_guard<std::mutex> lk(blockMtx);
            RasterBlockResult r;
            r.rasterHandle = job.rasterHandle;
            r.gen = job.gen;
            r.span = job.span; r.tx = job.tx; r.ty = job.ty;
            r.sw = job.sw; r.sh = job.sh;
            r.rgba = std::move(rgba); r.w = w; r.h = h;
            blockResults.push_back(std::move(r));
        }
    }
}

// 主线程: 每帧驱动。回传结果 + 枚举可见块发起读取(限速)。
void App::pumpRasterDetail(const MapScene& scene, GLBackend& backend) {
    if (!blockStarted.load()) { blockStarted.store(true); blockWorker = std::thread([this] { rasterBlockWorker(); }); }
    std::deque<RasterBlockResult> results;
    {
        std::lock_guard<std::mutex> lk(blockMtx);
        results.swap(blockResults);
    }
    for (auto& res : results)  {
        if (res.gen != blockGen.load()) continue;
        for (size_t i = 0; i < scene.layers.size(); i++)
            if (scene.layers[i].kind == LayerKind::Raster && scene.layers[i].rasterHandle == res.rasterHandle) {
                backend.setRasterTile(res.rasterHandle, res.span, res.tx, res.ty, res.sw, res.sh, res.rgba, res.w, res.h);
                break;
            }
    }
    results.clear();

    if (rebasePending.load()) return;

    const int kMaxNewJobs = 6;
    int newJobs = 0;
    for (size_t i = 0; i < scene.layers.size(); i++) {
        if (scene.layers[i].kind != LayerKind::Raster) continue;
        const MapLayer& l = scene.layers[i];
        if (!l.info.visible) continue;
        if (l.rasterHandle < 0 || l.rasterHandle >= (int)backend.rasters.size()) continue;
        const RasterData& r = l.raster;
        if (r.maxDim <= 2048) continue;
        double pxWorld = std::max(std::fabs(r.geo[1]), std::fabs(r.geo[5]));
        if (pxWorld <= 0) continue;
        if (scene.view.scale <= 0) continue;
        double spxPerScreen = pxWorld / scene.view.scale;
        double baseTexelScreen = (double)r.maxDim / 1024.0 * spxPerScreen;
        if (baseTexelScreen <= 1.5) continue;
        int span = (int)std::ceil(256.0 / spxPerScreen);
        if (span < 1) span = 1;
        if (span >= r.maxDim) continue;
        double hw = scene.view.scale * scene.view.vpW * 0.5;
        double hh = scene.view.scale * scene.view.vpH * 0.5;
        double viewX0 = scene.view.centerX - hw, viewX1 = scene.view.centerX + hw;
        double viewY0 = scene.view.centerY - hh, viewY1 = scene.view.centerY + hh;
        if (r.hasDispExtent) {
            double dx0,dy0,dx1,dy1,dx2,dy2,dx3,dy3;
            if (reprojectPoint(viewX0, viewY0, scene.displayEpsg, r.srcEpsg, dx0, dy0) &&
                reprojectPoint(viewX1, viewY0, scene.displayEpsg, r.srcEpsg, dx1, dy1) &&
                reprojectPoint(viewX1, viewY1, scene.displayEpsg, r.srcEpsg, dx2, dy2) &&
                reprojectPoint(viewX0, viewY1, scene.displayEpsg, r.srcEpsg, dx3, dy3)) {
                viewX0 = std::min({dx0,dx1,dx2,dx3});
                viewX1 = std::max({dx0,dx1,dx2,dx3});
                viewY0 = std::min({dy0,dy1,dy2,dy3});
                viewY1 = std::max({dy0,dy1,dy2,dy3});
            } else {
                continue;
            }
        }
        double sx0 = (viewX0 - r.geo[0]) / r.geo[1];
        double sx1 = (viewX1 - r.geo[0]) / r.geo[1];
        double sy0 = (viewY0 - r.geo[3]) / r.geo[5];
        double sy1 = (viewY1 - r.geo[3]) / r.geo[5];
        if (sx1 < sx0) std::swap(sx0, sx1);
        if (sy1 < sy0) std::swap(sy0, sy1);
        long long tx0 = (long long)std::floor(sx0 / span);
        long long tx1 = (long long)std::floor(sx1 / span);
        long long ty0 = (long long)std::floor(sy0 / span);
        long long ty1 = (long long)std::floor(sy1 / span);
        long long maxTX = (r.width + span - 1) / span;
        long long maxTY = (r.height + span - 1) / span;
        if (tx0 < 0) tx0 = 0;
        if (ty0 < 0) ty0 = 0;
        if (tx1 > maxTX - 1) tx1 = maxTX - 1;
        if (ty1 > maxTY - 1) ty1 = maxTY - 1;
        for (long long tx = tx0; tx <= tx1 && newJobs < kMaxNewJobs; tx++) {
            for (long long ty = ty0; ty <= ty1 && newJobs < kMaxNewJobs; ty++) {
                if (tx < 0 || ty < 0) continue;
                long long key = ((long long)l.rasterHandle << 48) | ((long long)span << 32)
                              | ((long long)tx << 16) | ((long long)ty & 0xFFFF);
                if (backend.hasRasterTile(l.rasterHandle, span, (int)tx, (int)ty)) continue;
                if (blockInflight.count(key)) continue;
                blockInflight.insert(key);
                RasterBlockJob j;
                j.openSpec = l.raster.openSpec;
                const RasterRenderOptions& o = l.rastOpts;
                j.as.mode = (o.mode == RasterRenderMode::RGB) ? RasterRgbAssemble::Mode::RGB
                       : (o.mode == RasterRenderMode::Pseudocolor) ? RasterRgbAssemble::Mode::Pseudocolor
                       : RasterRgbAssemble::Mode::Gray;
                j.as.b1 = (o.mode == RasterRenderMode::RGB) ? o.rBand : o.grayBand;
                j.as.b2 = o.gBand;
                j.as.b3 = o.bBand;
                j.as.colorMap = o.colorMap;
                j.as.mn = o.useAutoMinMax && !l.raster.bands.empty() && l.raster.bands[0].hasMinMax
                          ? l.raster.bands[0].min : o.minRaw;
                j.as.mx = o.useAutoMinMax && !l.raster.bands.empty() && l.raster.bands[0].hasMinMax
                          ? l.raster.bands[0].max : o.maxRaw;
                j.x0 = (int)(tx * span); j.y0 = (int)(ty * span);
                j.w0 = (int)std::min((long long)span, (long long)r.width - j.x0);
                j.h0 = (int)std::min((long long)span, (long long)r.height - j.y0);
                if (j.w0 <= 0 || j.h0 <= 0) continue;
                j.dw = std::max(1, (int)std::lround(256.0 * j.w0 / span));
                j.dh = std::max(1, (int)std::lround(256.0 * j.h0 / span));
                j.span = span; j.sw = j.w0; j.sh = j.h0;
                j.tx = (int)tx; j.ty = (int)ty;
                j.rasterHandle = l.rasterHandle;
                j.gen = blockGen.load();
                {
                    std::lock_guard<std::mutex> lk(blockMtx);
                    blockJobs.push_back(std::move(j));
                }
                newJobs++;
            }
        }
    }
    blockCv.notify_one();
}

// 发布一个新的后台属性表任务(已带锁检查过未在忙)。return false=因忙/无法发布, 调用方应回滚 pending 状态。
// 顺序: 1)打开(字段定义) 2)拉一页 3)取总数(可能慢, 放最后, 不阻塞行渲染)
bool App::launchAttrTask(AttrTaskSpec spec) {
    spec.gen = attrGen.load();
    {
        std::lock_guard<std::mutex> lk(attrMtx);
        if (attrBusy) return false;
        attrBusy = true;
        attrInfoReady = false;
        attrPageReady = false;
        attrCountReady = false;
        attrNeedCount = false;
        attrResultGen = -1;
    }
    bgThreads_.push_back(std::thread([this, spec]() {
        AttrLayerInfo info;
        bool infoOk = attrOpenLayer(spec.path, spec.layerIdx, info);
        {
            std::lock_guard<std::mutex> lk(attrMtx);
            attrInfoReady = true;
            attrFetchedInfo = infoOk ? info : AttrLayerInfo{};
            attrResultGen = spec.gen;
        }
        bool pok = false;
        if (infoOk) {
            AttrPageData pd;
            pok = attrFetchPage(spec.path, spec.layerIdx, spec.page, spec.rowsPerPage,
                                (TextEncoding)spec.enc, info, pd);
            {
                std::lock_guard<std::mutex> lk(attrMtx);
                attrPageReady = pok;
                attrFetchedPage = std::move(pd);
                attrNeedCount = spec.needCount;
                if (!spec.needCount) {
                    attrCountReady = true;
                    attrFetchedCount = info.total;
                    attrBusy = false;
                }
            }
        }
        if (infoOk && spec.needCount) {
            long long n = attrFeatureCount(spec.path, spec.layerIdx);
            {
                std::lock_guard<std::mutex> lk(attrMtx);
                attrCountReady = true;
                attrFetchedCount = n;
                attrBusy = false;
            }
        } else if (!infoOk) {
            std::lock_guard<std::mutex> lk(attrMtx);
            attrCountReady = true;
            attrFetchedCount = -1;
            attrBusy = false;
        }
    }));
    return true;
}

// 将后台完成的一页合并进 ui(保留 attrInfo, 缓存页替换/追加)
void App::applyAttrPage(UIState& ui, const AttrLayerInfo& info, AttrPageData pd) {
    if (!info.ok) return;
    if (info.layerName != ui.attr.info.layerName) {
        ui.attr.info = info;
    }
    bool replaced = false;
    for (auto& p : ui.attr.pages)
        if (p.page == pd.page) { p = std::move(pd); replaced = true; break; }
    if (!replaced) ui.attr.pages.push_back(std::move(pd));
    while ((int)ui.attr.pages.size() > 5) {
        int victim = -1;
        for (size_t i = 0; i < ui.attr.pages.size(); i++)
            if (ui.attr.pages[i].page != ui.attr.currentPage) { victim = (int)i; break; }
        if (victim < 0) break;
        ui.attr.pages.erase(ui.attr.pages.begin() + victim);
    }
}

// 矢量文件加载入口: 同路径去重 + 建占位图层(名字/要素数立刻可见, 数据块到达后填充) + 入队
int App::queueVector(const std::string& path, const std::vector<LayerMeta>& meta,
                     const std::vector<int>& layerIndices) {
    for (int i = (int)scene.layers.size() - 1; i >= 0; i--)
        if (scene.layers[i].sourcePath == path) {
            backend.removeLayer(i);
            backend.onVtSceneLayerRemoved(i);
            scene.layers.erase(scene.layers.begin() + i);
        }

    // v2: 大文件无缓存 → 后台生成瓦片缓存(完成后加载), 本次不再走 v1.0
    if (tryAutoVtBuild(path)) return -1;

    std::vector<int> idxs = layerIndices;
    if (idxs.empty())
        for (size_t i = 0; i < meta.size(); i++) idxs.push_back((int)i);
    if (idxs.empty()) return -1;

    int globalBase = (int)scene.layers.size();
    for (int li : idxs) {
        MapLayer L;
        if (li >= 0 && li < (int)meta.size()) {
            L.info.name = meta[li].name;
            L.info.featureCount = meta[li].featureCount;
        } else {
            L.info.name = "layer" + std::to_string(li);
        }
        L.info.sourceCrs = "";
        L.info.visible = true;
        L.data.name = L.info.name;
        L.data.featureCount = L.info.featureCount;
        L.data.srcEpsg = 0;
        L.data.minx = L.data.miny = 1e300;   // 哨兵: 首个块到达时写入真实范围
        L.data.maxx = L.data.maxy = -1e300;
        L.sourcePath = path;
        L.sourceLayerIdx = li;
        L.openSeq = ++openSeqCounter_;
        const float* c = kPalette[(s_layerColorIdx++) % 16];
        L.color[0] = c[0]; L.color[1] = c[1]; L.color[2] = c[2];
        scene.layers.push_back(std::move(L));
        backend.addLayerPlaceholder();
    }

    return loader.enqueue(path, cfg, meta, idxs, globalBase);
}

// v2 矢量瓦片缓存(.vtk)直接打开: 建场景图层 + backend vt 层(占位 geoms 保持索引对齐)
bool App::openVtFile(const std::string& path, const std::string& displayName, const std::string& srcPath) {
    peekg::vt::VtCache probe;
    if (!probe.open(path)) {
        ui.status = "无法打开矢量瓦片缓存: " + baseName(path);
        ui.statusErr = true;
        return false;
    }
    peekg::vt::VtFileHeader h = probe.header();
    probe.close();

    int gi = (int)scene.layers.size();
    MapLayer L;
    L.info.name = displayName.empty() ? baseName(path) : displayName;
    L.info.sourceCrs = h.dstEpsg ? ("EPSG:" + std::to_string(h.dstEpsg)) : "unknown";
    L.info.featureCount = 0;
    L.kind = LayerKind::Vector;
    L.data.name = L.info.name;
    L.data.srcEpsg = h.dstEpsg;
    L.data.minx = h.minx; L.data.miny = h.miny;
    L.data.maxx = h.maxx; L.data.maxy = h.maxy;
    L.sourcePath = srcPath.empty() ? path : srcPath;   // 属性表/识别查原始源文件(缓存无属性)
    L.sourceLayerIdx = 0;
    L.openSeq = ++openSeqCounter_;
    const float* c = kPalette[(s_layerColorIdx++) % 16];
    L.color[0] = c[0]; L.color[1] = c[1]; L.color[2] = c[2]; L.color[3] = 0.35f;

    backend.addLayerPlaceholder();   // 占位: 保持 geoms 与 scene.layers 索引一致
    int vh = backend.addVtLayer(path, gi, h.srcEpsg, h.dstEpsg);
    if (vh < 0) {
        backend.removeLayer(gi);
        ui.status = "矢量瓦片缓存打开失败";
        ui.statusErr = true;
        return false;
    }
    L.vtHandle = vh;
    scene.layers.push_back(std::move(L));

    if (scene.displayEpsg == 0) scene.displayEpsg = h.dstEpsg;
    // 场景范围用显示 CRS(缓存 CRS 不同则四角重投影); 图层自身 data 保持缓存 CRS 供 applyDisplayCrs 用
    double ex0 = h.minx, ey0 = h.miny, ex1 = h.maxx, ey1 = h.maxy;
    if (scene.displayEpsg != 0 && h.dstEpsg != 0 && scene.displayEpsg != h.dstEpsg) {
        const double cx[4] = {h.minx, h.maxx, h.minx, h.maxx};
        const double cy[4] = {h.miny, h.miny, h.maxy, h.maxy};
        double rx[4], ry[4];
        bool ok = true;
        for (int q = 0; q < 4; q++)
            if (!peekg::data::reprojectPoint(cx[q], cy[q], h.dstEpsg, scene.displayEpsg, rx[q], ry[q])) { ok = false; break; }
        if (ok) {
            ex0 = *std::min_element(rx, rx + 4); ex1 = *std::max_element(rx, rx + 4);
            ey0 = *std::min_element(ry, ry + 4); ey1 = *std::max_element(ry, ry + 4);
        }
    }
    scene.expandExtent(ex0, ey0, ex1, ey1);
    if (scene.view.vpW > 0 && scene.view.vpH > 0)
        scene.fitToView(scene.view.vpW, scene.view.vpH);
    else
        scene.needRefit = true;
    ui.status = "已加载矢量瓦片: " + baseName(path);
    ui.statusErr = false;
    return true;
}

// v2 缓存目录: 相对路径按 exe 目录解析(与 v1.0 几何缓存一致), 不受启动工作目录影响。
static std::string vtCacheDir(const AppConfig& cfg) {
    std::string d = cfg.cache_dir.empty() ? "cache" : cfg.cache_dir;
    std::filesystem::path p(d);
    if (!p.is_absolute()) {
        std::string e = exeDir();
        if (!e.empty()) d = e + "/" + d;
    }
    return d;
}

// 打开源文件时自动发现已建的 v2 缓存。优先命中与当前显示 CRS 一致的缓存;
// 否则命中源 CRS 的缓存(渲染时后台重投影到显示 CRS, 方案b)。
bool App::tryOpenVtForSource(const std::string& path) {
    peekg::vt::LayerInfo li;
    if (!peekg::vt::readVtLayerInfo(path, 0, li)) return false;
    int srcEpsg = li.srcEpsg;
    std::vector<int> cands;
    if (scene.displayEpsg > 0) cands.push_back(scene.displayEpsg);
    if (srcEpsg > 0) cands.push_back(srcEpsg);
    for (int d : cands) {
        std::string vp = peekg::vt::vtCachePath(vtCacheDir(cfg), path, srcEpsg, d);
        std::error_code ec;
        if (std::filesystem::exists(vp, ec)) {
            spdlog::info("[vt] 发现缓存 {} -> {}", path, vp);
            return openVtFile(vp, baseName(path), path);
        }
    }
    return false;
}

// 大文件且无 v2 缓存: 后台生成瓦片缓存(用源 CRS 建, 渲染时按需重投影到显示 CRS)。
// 构建期边建边看: 每建好一片就通知主线程渲染(内存 LRU 淘汰)。
bool App::tryAutoVtBuild(const std::string& path) {
    if (!cfg.vt_auto_build) return false;
    if (vtBuilding_.load()) return false;   // 已有构建在跑(串行)
    long long N = peekg::vt::estimateSourceVerts(path, 0, 50000);
    if (N < 0) return false;
    if (N < cfg.vt_threshold_verts) return false;
    peekg::vt::LayerInfo li;
    if (!peekg::vt::readVtLayerInfo(path, 0, li)) return false;
    int buildDst = li.srcEpsg;
    std::string out = peekg::vt::vtCachePath(vtCacheDir(cfg), path, li.srcEpsg, buildDst);
    std::string nm = baseName(path);

    // 建占位场景图层(范围取源图层): 相机可立即适配, 构建期边建边画
    int gi = (int)scene.layers.size();
    MapLayer L;
    L.info.name = nm;
    L.info.sourceCrs = li.srcEpsg ? ("EPSG:" + std::to_string(li.srcEpsg)) : "unknown";
    L.info.featureCount = li.featureCount;
    L.kind = LayerKind::Vector;
    L.data.name = nm;
    L.data.srcEpsg = li.srcEpsg;
    L.data.minx = li.minx; L.data.miny = li.miny;
    L.data.maxx = li.maxx; L.data.maxy = li.maxy;
    L.sourcePath = path;
    L.sourceLayerIdx = 0;
    L.openSeq = ++openSeqCounter_;
    const float* col = kPalette[(s_layerColorIdx++) % 16];
    L.color[0] = col[0]; L.color[1] = col[1]; L.color[2] = col[2]; L.color[3] = 0.35f;
    backend.addLayerPlaceholder();
    scene.layers.push_back(std::move(L));
    if (scene.displayEpsg == 0 && li.srcEpsg) scene.displayEpsg = li.srcEpsg;
    scene.expandExtent(li.minx, li.miny, li.maxx, li.maxy);
    if (scene.view.vpW > 0 && scene.view.vpH > 0) scene.fitToView(scene.view.vpW, scene.view.vpH);

    vtSceneIdx_ = gi;
    vtHandle_ = -1;
    vtSrcEpsg_ = li.srcEpsg;
    vtBbox_[0] = li.minx; vtBbox_[1] = li.miny; vtBbox_[2] = li.maxx; vtBbox_[3] = li.maxy;
    vtBuildLevel_ = -1;
    vtDisplayLevel_.store(-1);
    vtLayerReady_ = false;
    {
        std::lock_guard<std::mutex> lk(vtReadyMtx_);
        vtReady_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(vtMtx_);
        vtOut_ = out;
        vtName_ = nm;
    }
    vtPct_.store(0);
    vtDone_.store(false);
    vtOk_.store(false);
    vtBuilding_.store(true);
    if (vtThread_.joinable()) vtThread_.join();
    std::string src = path;
    int dst = buildDst;
    spdlog::info("[vt] 数据量大(≈{}M 顶点), 后台生成缓存 -> {}", N / 1000000, out);
    vtThread_ = std::thread([this, src, dst, out]() {
        peekg::vt::VtBuildConfig bc;
        bc.dstEpsg = dst;
        bc.levelStep = cfg.vt_level_step;
        peekg::vt::VtBuildStats st;
        bool ok = peekg::vt::buildVtCache(src, 0, out, bc, st,
            [this](int level, int tx, int ty) {
                vtDisplayLevel_.store(level);   // 阶段B 逐层下降: 通知主线程切换显示层
                std::lock_guard<std::mutex> lk(vtReadyMtx_);
                if (vtReady_.size() < 200000)
                    vtReady_.push_back({level, tx, ty});
            },
            [this](int pct) {
                if (pct < 0) pct = 0; if (pct > 100) pct = 100;
                vtPct_.store(pct);
                static thread_local int last = -1;
                if (pct >= last + 10 || pct == 100) {
                    last = pct;
                    spdlog::info("[vt] 建缓存 {}%", pct);
                }
            });
        vtOk_.store(ok);
        vtDone_.store(true);
    });
    ui.status = "数据量大 (≈" + std::to_string(N / 1000000) + "M 顶点), 正在后台生成瓦片缓存…";
    ui.statusErr = false;
    return true;
}

// 每帧消费 AsyncLoader 的完成/块事件, 应用到 scene/backend(原 AsyncLoader::update 主体)
void App::applyLoaderEvents() {
    std::vector<peekg::data::AsyncLoader::LoadEvent> doneList;
    std::vector<peekg::data::AsyncLoader::ChunkEvent> ev;
    loader.poll(doneList, ev);

    auto unionScene = [&](double minx, double miny, double maxx, double maxy) {
        if (!scene.hasExtent) {
            scene.bboxMinX = minx; scene.bboxMinY = miny;
            scene.bboxMaxX = maxx; scene.bboxMaxY = maxy;
            scene.hasExtent = true;
        } else {
            scene.bboxMinX = std::min(scene.bboxMinX, minx);
            scene.bboxMinY = std::min(scene.bboxMinY, miny);
            scene.bboxMaxX = std::max(scene.bboxMaxX, maxx);
            scene.bboxMaxY = std::max(scene.bboxMaxY, maxy);
        }
    };

    // 自动定位归属判定: 只允许"打开序号最新"的图层拖动镜头, 旧任务(较早文件仍在流式/完成、
    // 或已完成却被后来新开覆盖)不得抢镜, 否则两个文件交错流式时会来回跳。
    // 同一图层首次数据与完成(序号相等)重复定位无害(范围一致)。用户手动动过视图则取消自动定位。
    auto autoFit = [&](int gi) {
        if (ui.viewTouched) return;
        if (gi < 0 || gi >= (int)scene.layers.size()) return;
        if (scene.layers[gi].openSeq < fitOwnerSeq_) return;
        scene.zoomToLayer(gi);
        fitOwnerSeq_ = scene.layers[gi].openSeq;
    };

    for (auto& c : ev) {
        int gi = c.globalIdx;
        if (gi < 0 || gi >= (int)scene.layers.size()) continue;   // 图层已被清理/替换
        MapLayer& L = scene.layers[gi];
        if (!c.name.empty()) L.info.name = c.name;
        if (!c.crs.empty()) L.info.sourceCrs = c.crs;
        else L.info.sourceCrs = (c.srcEpsg ? "EPSG:" + std::to_string(c.srcEpsg) : "unknown");
        L.data.srcEpsg = c.srcEpsg;
        spdlog::debug("[consume] gi={} name={} chunk srcEpsg={} displayEpsg={} verts={} pts={} tris={}",
                      gi, L.info.name, c.srcEpsg, scene.displayEpsg,
                      c.verts.size(), c.pts.size(), c.tris.size());

        // 缓存命中整层单块: 几何移入 L.data(源CRS, 保留作权威数据), 不做主线程重投影或 GPU staging。
        // 场景范围: 源==显示直接并入; 否则对该层 bbox 四角重投影到显示坐标(便宜, 供 fit)。
        // 完成时统一走 后台重建/分块, 主线程不阻塞。
        if (c.full) {
            L.data.vertices = std::move(c.verts);
            L.data.points = std::move(c.pts);
            L.data.triangles = std::move(c.tris);
            if (c.minx <= c.maxx) {
                L.data.minx = c.minx; L.data.miny = c.miny;
                L.data.maxx = c.maxx; L.data.maxy = c.maxy;
            }
            L.stagingKey = 0;
            L.stagingMixed = false;
            L.cpuOnlyStaging = true;
            if (scene.displayEpsg == 0 || c.srcEpsg == 0 || c.srcEpsg == scene.displayEpsg) {
                if (c.minx <= c.maxx) unionScene(c.minx, c.miny, c.maxx, c.maxy);
            } else if (c.minx <= c.maxx) {
                const double cxx[4] = {c.minx, c.maxx, c.minx, c.maxx};
                const double cyy[4] = {c.miny, c.miny, c.maxy, c.maxy};
                double rx[4], ry[4];
                bool ok = true;
                for (int q = 0; q < 4; q++)
                    if (!reprojectPoint(cxx[q], cyy[q], c.srcEpsg, scene.displayEpsg, rx[q], ry[q])) { ok = false; break; }
                if (ok) {
                    double mnx = *std::min_element(rx, rx + 4), mxx = *std::max_element(rx, rx + 4);
                    double mny = *std::min_element(ry, ry + 4), mxy = *std::max_element(ry, ry + 4);
                    unionScene(mnx, mny, mxx, mxy);
                }
            }
            loaderFirstDataRefit = true;
            spdlog::debug("[consume] full-cpu fast lane gi={} srcEpsg={} displayEpsg={}",
                          gi, c.srcEpsg, scene.displayEpsg);
            continue;
        }

        // 所有非 full 块统一走"块=桶"(缓存命中逐块 / 流式 MISS / CRS 重建重读):
        // 每块直接成桶(不整层累积几何/属性), 桶坐标 = 目标显示 CRS(重建=重建目标,
        // 否则当前显示; 源==目标即源坐标零拷贝)。L.data 只保留 meta 与范围(无几何)。
        int k = 0;
        if (c.rebuildEpsg != 0) k = c.rebuildEpsg;
        else if (scene.displayEpsg != 0 && c.srcEpsg != 0 && c.srcEpsg != scene.displayEpsg)
            k = scene.displayEpsg;
        const bool hasMeta = (c.minx < c.maxx);
        if (c.rebuildEpsg != 0 && L.cacheBucketInit && backend.isBlockRebuilding(gi)) {
            // 块桶层重建: 首个重建块到达 -> 结束重建态并换到新桶坐标系。
            // beginBucketLayer 会清旧桶(L.data 范围/meta 已就位, 无需重并场景)。
            backend.beginBucketLayer(gi, k != 0 ? k : (c.srcEpsg != 0 ? c.srcEpsg : 0));
            spdlog::info("[CRS] rebuild layer[{}] first chunk -> buckets EPSG {} ({} verts)",
                         gi, k, c.verts.size() + c.pts.size() + c.tris.size());
        }
        if (!L.cacheBucketInit) {
            L.cacheBucketInit = true;
            L.stagingKey = k;
            backend.beginBucketLayer(gi, k != 0 ? k : (c.srcEpsg != 0 ? c.srcEpsg : 0));
            if (c.rebuildEpsg != 0)
                spdlog::info("[CRS] rebuild layer[{}] first chunk -> buckets EPSG {} ({} verts)",
                             gi, k, c.verts.size() + c.pts.size() + c.tris.size());
            if (hasMeta) {
                L.data.minx = c.minx; L.data.miny = c.miny;
                L.data.maxx = c.maxx; L.data.maxy = c.maxy;
                if (scene.displayEpsg == 0 || c.srcEpsg == 0 || c.srcEpsg == scene.displayEpsg) {
                    unionScene(c.minx, c.miny, c.maxx, c.maxy);
                } else {
                    const double cxx[4] = {c.minx, c.maxx, c.minx, c.maxx};
                    const double cyy[4] = {c.miny, c.miny, c.maxy, c.maxy};
                    double rx[4], ry[4];
                    bool ok = true;
                    for (int q = 0; q < 4; q++)
                        if (!reprojectPoint(cxx[q], cyy[q], c.srcEpsg, scene.displayEpsg, rx[q], ry[q])) { ok = false; break; }
                    if (ok) {
                        double mnx = *std::min_element(rx, rx + 4), mxx = *std::max_element(rx, rx + 4);
                        double mny = *std::min_element(ry, ry + 4), mxy = *std::max_element(ry, ry + 4);
                        unionScene(mnx, mny, mxx, mxy);
                    }
                }
            }
            loaderFirstDataRefit = true;
            // 新打开图层的首批块到达即按整层范围定位(不等渲染完成), 语义与"缩放到图层"一致
            // (源CRS范围经四角投影到显示CRS)。仅整层 meta 已知时做(HIT缓存块首块携带);
            // MISS 流式无 meta(范围逐块累积)仍由完成时定位兜底。
            // 新打开图层首批块定位: 坐标无需落位转换时可提前做(显示==源 或显示未定)。
            // 另: 自动统一规则下地理显示CRS一经选定不会再变(投影基准可能被后来
            // 的地理文件推翻, 因而跨投影过渡期源->显示重投影会出垃圾范围拉走镜头), 
            // 故显示为地理坐标系时同样直接做, 源投影->地理 为可靠逆向。
            // 其余(显示为投影且源不同)留到完成时(显示已稳定)由 autoFit 兜底。
            if (hasMeta && (scene.displayEpsg == 0 || c.srcEpsg == 0 || c.srcEpsg == scene.displayEpsg ||
                            epsgIsGeographic(scene.displayEpsg)))
                autoFit(gi);
        }
        // 源范围: 无 meta(流式 MISS)时用源块坐标累计
        if (!hasMeta) {
            auto grow = [&](const std::vector<float>& b) {
                if (b.empty()) return;
                double a, bb, cc, d;
                computeExtent(b, a, bb, cc, d);
                if (L.data.minx > L.data.maxx) {
                    L.data.minx = a; L.data.miny = bb;
                    L.data.maxx = cc; L.data.maxy = d;
                } else {
                    L.data.minx = std::min(L.data.minx, a);
                    L.data.miny = std::min(L.data.miny, bb);
                    L.data.maxx = std::max(L.data.maxx, cc);
                    L.data.maxy = std::max(L.data.maxy, d);
                }
            };
            grow(c.verts); grow(c.pts); grow(c.tris);
        }
        // 该块重投影到桶坐标(仅当源!=桶坐标; 相等零拷贝)
        std::vector<float> dispV, dispP, dispT;
        dispV.swap(c.verts);
        dispP.swap(c.pts);
        dispT.swap(c.tris);
        if (k != 0 && c.srcEpsg != 0 && c.srcEpsg != k) {
            std::vector<float> rv, rp, rt;
            if (reprojectVertices(dispV, c.srcEpsg, k, rv)) dispV.swap(rv);
            if (reprojectVertices(dispP, c.srcEpsg, k, rp)) dispP.swap(rp);
            if (reprojectVertices(dispT, c.srcEpsg, k, rt)) dispT.swap(rt);
        }
        backend.addBucket(gi, dispV, dispP, dispT);
        // 场景显示范围: 流式 MISS(无 meta)按块并入; 有 meta 的整层范围已在首块并入
        if (!hasMeta) {
            bool any = false;
            auto unionDisp = [&](const std::vector<float>& b) {
                if (b.empty()) return;
                double a, bb, cc, d;
                computeExtent(b, a, bb, cc, d);
                unionScene(a, bb, cc, d);
                any = true;
            };
            unionDisp(dispV); unionDisp(dispP); unionDisp(dispT);
            if (any) loaderFirstDataRefit = true;
        }
        continue;
    }

    // 完成处理
    for (auto& t : doneList) {
        if (t.failed) {
            ui.status = t.msg;
            ui.statusErr = true;
        } else {
            ui.status = t.msg;
            ui.statusErr = false;
            // 该文件各层流式块已到齐。staging 坐标与当前显示 CRS 一致 -> 直接分块(快路径,
            // 避免对大文件做整层拷贝/重投影把主线程卡住); 中途显示CRS 变过(staging 混坐标)
            // 或为空 -> 从源CRS权威数据重投影后整层重建+分块。
            auto t0 = std::chrono::steady_clock::now();
            int rebuilt = 0;
            for (int gi = t.globalBase; gi < t.globalBase + (int)t.layerIndices.size(); gi++) {
                if (gi < 0 || gi >= (int)scene.layers.size()) continue;
                MapLayer& L = scene.layers[gi];
                if (backend.isBlockBucketLayer(gi)) {
                    // 块桶层: 桶已全量提交(逐块成桶), 无需 finalize/rebuild。
                    L.stagingKey = -1;
                    L.stagingMixed = false;
                    L.cacheBucketInit = false;
                    continue;
                }
                const VectorData& vd = L.data;
                int curKey = 0;
                if (scene.displayEpsg != 0 && vd.srcEpsg != 0 && vd.srcEpsg != scene.displayEpsg)
                    curKey = scene.displayEpsg;
                if (L.stagingKey == curKey && !L.stagingMixed) {
                    // staging 坐标统一
                    if (L.cpuOnlyStaging) {
                        // 缓存命中整层: 几何在 L.data(源==显示, 无需重投影)。大层后台重建
                        // (副本快照, L.data 保持权威), 小块先喂回 backend 再主线程分块(立即出现)。
                        L.cpuOnlyStaging = false;
                        long long sv = (long long)(vd.vertices.size() / 2 +
                                                   vd.points.size() / 2 + vd.triangles.size() / 2);
                        if (sv > 0 && sv <= 300000) {
                            backend.setStagingCpu(gi, std::vector<float>(vd.vertices),
                                                  std::vector<float>(vd.points),
                                                  std::vector<float>(vd.triangles));
                            backend.finalizeLayer(gi, scene.displayEpsg);
                        } else if (sv > 0) {
                            auto snap = std::make_shared<VectorData>(vd);   // 副本
                            backend.startLayerRebuild(gi, snap, scene.displayEpsg);
                            rebuilt++;
                        }
                    } else {
                        // 流式 staging(在 backend): 小块直接分块; 大层从 backend 零拷贝快照后台重建
                        long long sv = 0;
                        if (gi >= 0 && gi < (int)backend.geoms.size() && !backend.geoms[gi].committed) {
                            const auto& gg = backend.geoms[gi];
                            sv = (long long)(gg.cpuV.size() / 2 + gg.cpuP.size() / 2 +
                                             gg.cpuF.size() / 2);
                        }
                        if (sv <= 300000) {
                            backend.finalizeLayer(gi, scene.displayEpsg);
                        } else {
                            auto snap = std::make_shared<VectorData>();
                            if (backend.stealStagingCpu(gi, *snap)) {
                                snap->srcEpsg = (scene.displayEpsg != 0) ? scene.displayEpsg : 0;
                                snap->name = L.info.name;
                                backend.startLayerRebuild(gi, snap, scene.displayEpsg);
                                rebuilt++;
                            } else {
                                backend.finalizeLayer(gi, scene.displayEpsg);
                            }
                        }
                    }
                } else {
                    // staging 混过坐标(加载中途显示CRS变过)或缓存整层需重投影:
                    // 后台重投影+整层重建(副本快照, L.data 保持权威), 主线程不阻塞。
                    if (!(vd.vertices.empty() && vd.points.empty() && vd.triangles.empty())) {
                        auto snap = std::make_shared<VectorData>(vd);
                        backend.startLayerRebuild(gi, snap, scene.displayEpsg);
                        rebuilt++;
                    }
                }
                L.stagingKey = -1;
                L.stagingMixed = false;
                L.cpuOnlyStaging = false;
            }
            spdlog::info("[done] " + t.path + " layers=" + std::to_string(t.layerIndices.size()) +
                         " rebuilt=" + std::to_string(rebuilt) + " " +
                         std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()) + "ms");
            if (!t.rebuild) {
                // 只适配本次新打开的文件(而非全部图层并集), 与"缩放到图层"一致:
                // 打开第 N 个文件时视角落在该文件, 不再跳到几层合围框"飘走"。
                // 由 autoFit 判定归属: 较旧文件完成时(已被更新的打开覆盖)不再抢镜。
                int lastGi = t.globalBase + (int)t.layerIndices.size() - 1;
                autoFit(lastGi);
            }
        }
    }

    // 空图层(哨兵未复位)在所属文件完成后收尾。有几何但范围仍是哨兵的(旧缓存/遗漏路径)
    // 现场重算, 避免 (0,0,0,0) 范围使"适配视图"缩到原点 → 图层不可见。
    for (auto& t : doneList) {
        if (t.failed) continue;
        for (int gi = t.globalBase; gi < t.globalBase + (int)t.layerIndices.size(); gi++) {
            if (gi < 0 || gi >= (int)scene.layers.size()) continue;
            VectorData& vd = scene.layers[gi].data;
            if (!(vd.minx <= vd.maxx)) {
                double a = 0, b = 0, c = 0, d = 0;
                bool any = false;
                if (!vd.vertices.empty()) { computeExtent(vd.vertices, a, b, c, d); any = true; }
                if (!vd.points.empty()) {
                    double x0, y0, x1, y1;
                    computeExtent(vd.points, x0, y0, x1, y1);
                    if (!any) { a = x0; b = y0; c = x1; d = y1; }
                    else { a = std::min(a, x0); b = std::min(b, y0); c = std::max(c, x1); d = std::max(d, y1); }
                    any = true;
                }
                if (!vd.triangles.empty()) {
                    double x0, y0, x1, y1;
                    computeExtent(vd.triangles, x0, y0, x1, y1);
                    if (!any) { a = x0; b = y0; c = x1; d = y1; }
                    else { a = std::min(a, x0); b = std::min(b, y0); c = std::max(c, x1); d = std::max(d, y1); }
                    any = true;
                }
                if (any) { vd.minx = a; vd.miny = b; vd.maxx = c; vd.maxy = d; }
                else vd.minx = vd.miny = vd.maxx = vd.maxy = 0;
            }
        }
    }
}

void App::frame(GLFWwindow* window) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    renderUI(scene, backend, cfg, ui);

    if (scene.needRefit && scene.view.vpW > 1) {
        scene.fitToView(scene.view.vpW, scene.view.vpH);
        scene.needRefit = false;
    }

    if (ui.openRequested) {
        pendingOpen.insert(pendingOpen.end(), ui.openPaths.begin(), ui.openPaths.end());
        ui.openPaths.clear();
        ui.openRequested = false;
    }

    // 主线程轮询: worker 完成元数据预读 -> 置 metaReady(复位 done 防止下一帧重复消费)
    if (!ui.metaReady && metaDone.load()) {
        ui.metaReady = true;
        metaDone.store(false);
    }

    // v2 后台建缓存: 边建边看 + 进度; 完成后切回视口模式
    if (vtBuilding_.load()) {
        // 挂上 vt 渲染层(文件头由构建线程 create 后即可打开; 未就绪则下帧重试)
        if (!vtLayerReady_ && vtSceneIdx_ >= 0) {
            int h = backend.addVtLayer(vtOut_, vtSceneIdx_, vtSrcEpsg_, vtSrcEpsg_);
            if (h >= 0) {
                vtHandle_ = h;
                vtLayerReady_ = true;
                backend.vtRenderer().setBuilding(h, true);
                backend.vtRenderer().setPlaceholderBbox(h, vtBbox_[0], vtBbox_[1], vtBbox_[2], vtBbox_[3]);
                spdlog::info("[vt] 构建期渲染层已挂上 handle={} bbox=({:.4f},{:.4f},{:.4f},{:.4f})",
                             h, vtBbox_[0], vtBbox_[1], vtBbox_[2], vtBbox_[3]);
            }
        }
        // 把构建线程产出的瓦片投递给渲染器(每帧限量, 避免一次灌爆)
        if (vtLayerReady_) {
            int want = vtDisplayLevel_.load();
            if (want >= 0 && want != vtBuildLevel_) {
                backend.vtRenderer().setBuildLevel(vtHandle_, want);
                vtBuildLevel_ = want;
            }
            std::vector<std::array<int, 3>> batch;
            {
                std::lock_guard<std::mutex> lk(vtReadyMtx_);
                size_t take = std::min<size_t>(vtReady_.size(), 4000);
                batch.assign(vtReady_.begin(), vtReady_.begin() + take);
                vtReady_.erase(vtReady_.begin(), vtReady_.begin() + take);
            }
            for (auto& r : batch)
                if (r[0] == vtBuildLevel_)
                    backend.vtRenderer().requestTile(vtHandle_, r[0], r[1], r[2]);
        }
        {
            int pct = vtPct_.load();
            int lv = vtDisplayLevel_.load();
            if (pct < 50)
                ui.status = "正在生成最深层瓦片 (L" + std::to_string(lv) + ")… " + std::to_string(pct) + "%";
            else
                ui.status = "正在合并瓦片层 L" + std::to_string(lv) + "… " + std::to_string(pct) + "%";
        }
        ui.statusErr = false;
    }
    if (vtDone_.load()) {
        vtDone_.store(false);
        if (vtThread_.joinable()) vtThread_.join();
        bool ok = vtOk_.load();
        std::string name;
        {
            std::lock_guard<std::mutex> lk(vtMtx_);
            name = vtName_;
        }
        if (vtLayerReady_) {
            std::vector<std::array<int, 3>> batch;
            {
                std::lock_guard<std::mutex> lk(vtReadyMtx_);
                batch.swap(vtReady_);
            }
            for (auto& r : batch)
                backend.vtRenderer().requestTile(vtHandle_, r[0], r[1], r[2]);
            backend.vtRenderer().setBuilding(vtHandle_, false);
        } else if (ok && vtSceneIdx_ >= 0) {
            int h = backend.addVtLayer(vtOut_, vtSceneIdx_, vtSrcEpsg_, vtSrcEpsg_);
            if (h >= 0) { vtHandle_ = h; vtLayerReady_ = true; }
        }
        vtBuilding_.store(false);
        if (ok) {
            ui.status = "瓦片缓存已生成并渲染: " + name;
            ui.statusErr = false;
        } else {
            if (vtSceneIdx_ >= 0 && vtSceneIdx_ < (int)scene.layers.size()) {
                backend.removeLayer(vtSceneIdx_);
                backend.onVtSceneLayerRemoved(vtSceneIdx_);
                scene.removeLayer(vtSceneIdx_);
            }
            ui.status = "瓦片缓存生成失败(可改用较小阈值或手动 vt_build)";
            ui.statusErr = true;
        }
        vtSceneIdx_ = -1;
        vtHandle_ = -1;
        vtLayerReady_ = false;
        vtBuildLevel_ = -1;
        vtDisplayLevel_.store(-1);
    }
    // 消费后台完成的图层元数据预读(non-shp 多图层判断用 / 栅格 subdataset)
    if (ui.metaReady) {
        ui.metaReady = false;
        ui.metaLoading = false;
        std::string p = metaPath;
        std::vector<LayerMeta> meta;
        bool ok;
        bool isSds;
        {
            std::lock_guard<std::mutex> lk(metaMtx);
            meta = std::move(metaResult);
            ok = metaOk;
            isSds = sdsFlag.load();
            sdsFlag.store(false);
        }
        if (isSds) {
            if (ok && !meta.empty()) {
                ui.sdsDialog = true;
                ui.layerMeta = std::move(meta);
                ui.layerDialogPath = p;
                ui.showLayerDialog = true;
            } else {
                ui.status = "未检测到子数据集, 直接加载";
                ui.statusErr = false;
                {
                    std::lock_guard<std::mutex> lk(rasterMtx);
                    pendingRasterSpecs.push_back(RasterLoadJob{p, {}, -1});
                }
                refreshRasterQueue();
            }
        } else if (ok && !meta.empty()) {
            if (meta.size() > 1) {
                ui.sdsDialog = false;
                ui.layerMeta = std::move(meta);
                ui.layerDialogPath = p;
                ui.showLayerDialog = true;
            } else {
                queueVector(p, meta, {});
                ui.viewTouched = false;
            }
        } else {
            LayerMeta m;
            m.name = baseName(p);
            meta.push_back(m);
            queueVector(p, meta, {});
            ui.viewTouched = false;
            ui.status = "读取图层信息失败, 直接尝试加载";
            ui.statusErr = true;
        }
    }

    // 消费后台完成的栅格底图: 主线程建图层 + 上传纹理(GL 上下文在此)
    if (rasterDone.load()) {
        rasterDone.store(false);
        std::vector<RasterData> metas;
        std::vector<std::vector<unsigned char>> rgs;
        std::vector<std::array<int,2>> dims;
        std::vector<int> bws;
        std::vector<int> rebases;
        {
            std::lock_guard<std::mutex> lk(rasterMtx);
            metas = std::move(rasterMeta);
            rgs = std::move(rasterRgba);
            dims = std::move(rasterDims);
            bws = std::move(rasterBandW);
            rebases = std::move(rasterRebase);
        }
        for (size_t k = 0; k < metas.size(); k++) {
            int rb = (k < rebases.size()) ? rebases[k] : -1;
            if (rb >= 0 && rb < (int)scene.layers.size() &&
                scene.layers[rb].kind == LayerKind::Raster) {
                MapLayer& l = scene.layers[rb];
                if (l.rasterHandle >= 0 && l.rasterHandle < (int)backend.rasters.size()) {
                    l.raster = metas[k];
                    l.info.name = metas[k].name;
                    reprojectRasterExtent(l.raster, scene.displayEpsg);
                    backend.setRasterBase(l.rasterHandle, rgs[k], dims[k][0], dims[k][1]);
                    backend.clearRasterTiles(l.rasterHandle);
                }
                blockInflight.clear();
                {
                    std::lock_guard<std::mutex> bk(blockMtx);
                    blockResults.clear();
                }
                rebasePending.store(false);
            } else {
                bool dup = false;
                for (auto& l : scene.layers)
                    if (l.sourcePath == metas[k].openSpec) { dup = true; break; }
                if (dup) continue;
                RasterData rd = std::move(metas[k]);
                scene.addRasterLayer(rd);
                int ridx = backend.addRasterLayer(rd);
                backend.setRasterBase(ridx, rgs[k], dims[k][0], dims[k][1]);
                reprojectRasterExtent(scene.layers.back().raster, scene.displayEpsg);
                {
                    const RasterData& rd = scene.layers.back().raster;
                    if (rd.hasDispExtent)
                        scene.expandExtent(rd.dispMinx, rd.dispMiny, rd.dispMaxx, rd.dispMaxy);
                    else
                        scene.expandExtent(rd.minx, rd.miny, rd.maxx, rd.maxy);
                }
                if (!scene.layers.empty()) {
                    scene.layers.back().sourcePath = rd.openSpec;
                    scene.layers.back().rasterHandle = ridx;
                }
            }
        }
        ui.viewTouched = false;
        if (!metas.empty()) {
            ui.status = "已加载栅格: " + baseName(metas[0].openSpec);
            ui.statusErr = false;
        } else {
            ui.status = "栅格读取失败";
            ui.statusErr = true;
        }
    }

    // 栅格设置变更: 重新加载底图(rebase) 并清块, 按当前 rastOpts 重渲染
    if (ui.rasterSettingsDirty && ui.rasterSettingsLayer >= 0 &&
        ui.rasterSettingsLayer < (int)scene.layers.size() &&
        scene.layers[ui.rasterSettingsLayer].kind == LayerKind::Raster) {
        ui.rasterSettingsDirty = false;
        const MapLayer& rl = scene.layers[ui.rasterSettingsLayer];
        {
            std::lock_guard<std::mutex> lk(rasterMtx);
            pendingRasterSpecs.push_back(RasterLoadJob{rl.raster.openSpec, rl.rastOpts,
                                                       ui.rasterSettingsLayer});
        }
        rebasePending.store(true);
        blockGen.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(blockMtx);
            blockJobs.clear();
            blockResults.clear();
        }
        refreshRasterQueue();
    }

    // 逐文件处理: 有进行中的预读则等它结束; shp 直接入队
    if (!ui.showLayerDialog && !ui.metaLoading && !ui.metaReady) {
        if (!deferredOpen.empty() && deferredArmed && loader.stats().activeFiles == 0) {
            for (auto& dp : deferredOpen) pendingOpen.push_back(dp);
            deferredOpen.clear();
        }
        if (!pendingOpen.empty()) {
            std::string p = pendingOpen.front();
            pendingOpen.pop_front();
            deferredArmed = true;
            // 后缀从最后一个 '.' 取, 不能固定取末 4 字符(如 acpcp.1982.nc 会截成 "2.nc" 而错走矢量路径)
            std::string ext;
            size_t dot = p.find_last_of('.');
            if (dot != std::string::npos && dot + 1 < p.size()) ext = p.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (ext == ".vtk") {
                openVtFile(p);
                ui.viewTouched = false;
            } else if (!isRasterExt(ext) && tryOpenVtForSource(p)) {
                // 源文件已有匹配的 v2 缓存: 直接走瓦片渲染
                ui.viewTouched = false;
            } else if (ext == ".shp") {
                LayerMeta m;
                m.name = baseName(p);
                m.featureCount = -1;
                queueVector(p, {m}, {});
                ui.viewTouched = false;
            } else if (isRasterExt(ext)) {
                // 栅格: 后台读元数据 + 底图像素; 有多 subdataset 则先弹选择对话框
                ui.sdsDialog = false;
                bgThreads_.push_back(std::thread([this, p]() {
                    // geolocation 系 netCDF: 列出可打开的「数据变量」（lat/lon/1D/无坐标变量滤除）
                    std::vector<GeolocVar> gv;
                    bool geoList = geolocationVars(p, gv);
                    std::vector<std::string> subs;
                    bool hasSub = geoList ? !gv.empty()
                                          : (readRasterSubDatasets(p, subs) && subs.size() > 1);
                    if (hasSub) {
                        std::vector<LayerMeta> m;
                        if (geoList) {
                            m.resize(gv.size());
                            for (size_t k = 0; k < gv.size(); k++) {
                                m[k].name = gv[k].var;
                                m[k].featureCount = 0;
                                m[k].isGeolocVar = true;
                                m[k].auxCount = gv[k].timeCount;
                                m[k].auxSel = 0;
                            }
                        } else {
                            m.resize(subs.size());
                            for (size_t k = 0; k < subs.size(); k++) {
                                m[k].name = "subds::" + subs[k];
                                m[k].featureCount = 0;
                            }
                        }
                        {
                            std::lock_guard<std::mutex> lk(metaMtx);
                            metaResult = std::move(m);
                            metaOk = true;
                            metaPath = p;
                            sdsFlag.store(true);
                        }
                        metaDone.store(true);
                    } else {
                        {
                            std::lock_guard<std::mutex> lk(rasterMtx);
                            pendingRasterSpecs.push_back(RasterLoadJob{p, {}, -1});
                        }
                        rasterPing.store(true);
                    }
                }));
            } else {
                // 其他格式可能多图层: 后台线程读元数据, 完成后决定对话框/入库
                ui.metaLoading = true;
                ui.status = "正在读取图层信息: " + baseName(p);
                ui.statusErr = false;
                metaDone.store(false);
                bgThreads_.push_back(std::thread([this, p]() {
                    std::vector<LayerMeta> m;
                    bool ok = readLayerMetadata(p, m);
                    {
                        std::lock_guard<std::mutex> lk(metaMtx);
                        metaResult = std::move(m);
                        metaOk = ok;
                        metaPath = p;
                    }
                    metaDone.store(true);
                }));
            }
        }
    }

    if (ui.clearRequested) {
        scene.clearLayers();
        backend.clearLayers();
        loader.cancel();
        ui.displayCrsChoice = 0;
        ui.status.clear();
        ui.statusErr = false;
        ui.loadActive = false;
        ui.attr.isOpen = false;
        ui.attr.pages.clear();
        ui.attr.bindLayer = -1;
        ui.attr.hlActive = false;
        attrGen.fetch_add(1);    // 作废在途属性表任务
        ui.clearRequested = false;
    }

    // 卸载单个图层(右键菜单)
    if (ui.removeLayerRequested) {
        int idx = ui.removeLayerIdx;
        ui.removeLayerRequested = false;
        ui.removeLayerIdx = -1;
        if (idx >= 0 && idx < (int)scene.layers.size()) {
            std::string gone = scene.layers[idx].info.name;
            loader.cancel();              // 中止进行中加载, 避免块事件对错位图层追加
            if (idx == ui.attr.bindLayer) {
                ui.attr.isOpen = false;
                ui.attr.pages.clear();
                ui.attr.bindLayer = -1;
                ui.attr.hlActive = false;
                attrGen.fetch_add(1);    // 作废在途属性表任务
            }
            if (scene.layers[idx].kind == LayerKind::Raster) {
                if (scene.layers[idx].rasterHandle >= 0)
                    backend.removeRasterLayer(scene.layers[idx].rasterHandle);
            } else {
                backend.removeLayer(idx);
            }
            backend.onVtSceneLayerRemoved(idx);   // vt 层: 移除命中者 + 其余下标前移
            scene.removeLayer(idx);
            ui.status = "已卸载图层: " + gone;
            ui.statusErr = false;
        }
    }

    // 从 WKT 创建新图层(File ▸ Render WKT... 对话框确认后)
    if (ui.wktRequested) {
        std::string wkt = ui.wktText;
        ui.wktText.clear();
        ui.wktRequested = false;

        static const char kAlpha[] =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
        std::string name;
        for (int i = 0; i < 8; i++) name += kAlpha[rng() % (sizeof(kAlpha) - 1)];

        int base = scene.displayEpsg;
        if (base == 0 && !scene.layers.empty()) base = scene.layers[0].data.srcEpsg;
        if (base == 0) base = 4326;
        std::string crs = "EPSG:" + std::to_string(base);

        VectorData vd;
        if (loadWktToVectorData(name, wkt, crs, base, vd)) {
            scene.addLayer(vd);
            backend.addLayer(vd.vertices, vd.points, vd.triangles);
            ui.status = "已从 WKT 创建图层 " + name + " (" + crs + ")";
            ui.statusErr = false;
            ui.viewTouched = false;
        } else {
            ui.status = "WKT 解析失败或无可绘制几何";
            ui.statusErr = true;
        }
    }

    if (ui.loadFilteredRequested) {
        if (ui.sdsDialog) {
            for (size_t k = 0; k < ui.loadFilteredIndices.size(); k++) {
                const LayerMeta& mm = ui.loadFilteredMeta[k];
                std::string spec = mm.name;
                if (mm.isGeolocVar)
                    spec = makeGeolocSpec(ui.loadFilteredPath, mm.name, mm.auxSel);
                else if (spec.size() > 7 && spec.substr(0, 7) == "subds::") spec = spec.substr(7);
                {
                    std::lock_guard<std::mutex> lk(rasterMtx);
                    pendingRasterSpecs.push_back(RasterLoadJob{spec, {}, -1});
                }
            }
            refreshRasterQueue();
        } else {
            queueVector(ui.loadFilteredPath, std::move(ui.loadFilteredMeta), ui.loadFilteredIndices);
            ui.viewTouched = false;
        }
        ui.loadFilteredRequested = false;
        ui.sdsDialog = false;
    }

    // 底部属性表: 打开/切换图层(快照绑定, 后台拉取第 0 页)
    if (ui.attr.openRequested) {
        ui.attr.openRequested = false;
        int li = ui.attr.openLayerIdx;
        ui.attr.openLayerIdx = -1;
        if (li >= 0 && li < (int)scene.layers.size() && !scene.layers[li].sourcePath.empty()) {
            ui.attr.bindLayer = li;
            ui.attr.path = scene.layers[li].sourcePath;
            ui.attr.fileLayerIdx = scene.layers[li].sourceLayerIdx;
            ui.attr.srcEpsg = scene.layers[li].data.srcEpsg;
            ui.attr.pages.clear();
            ui.attr.info = AttrLayerInfo{};
            ui.attr.showCol.clear();
            ui.attr.currentPage = 0;
            ui.attr.hlActive = false;
            ui.attr.locateRequested = false;
            ui.attr.loading = true;
            ui.attr.isOpen = false;
            attrGen.fetch_add(1);    // 作废任何在途旧任务结果
            AttrTaskSpec spec;
            spec.path = ui.attr.path;
            spec.layerIdx = ui.attr.fileLayerIdx;
            spec.page = 0;
            spec.rowsPerPage = ui.attr.rowsPerPage;
            spec.enc = ui.attr.encoding;
            spec.needCount = true;         // 打开时需取总数
            attrOpenRetries = 0;           // 重置自动重开计数
            if (!launchAttrTask(spec)) ui.attr.loading = false;  // 忙: 回滚, 等下一次重开
        } else {
            ui.status = "该图层无源文件, 无法打开属性表";
            ui.statusErr = true;
        }
    }

    // 底部属性表: 翻页请求
    if (ui.attr.gotoRequested) {
        ui.attr.gotoRequested = false;
        if (ui.attr.isOpen && !ui.attr.path.empty() && ui.attr.info.ok) {
            ui.attr.loading = true;
            AttrTaskSpec spec;
            spec.path = ui.attr.path;
            spec.layerIdx = ui.attr.fileLayerIdx;
            spec.page = ui.attr.currentPage;
            spec.rowsPerPage = ui.attr.rowsPerPage;
            spec.enc = ui.attr.encoding;
            if (!launchAttrTask(spec)) ui.attr.loading = false;  // 忙: 回滚, 稍后自动补发
        }
    }

    // 底部属性表: 双击行居中(仅平移不缩放)
    if (ui.attr.locateRequested) {
        ui.attr.locateRequested = false;
        if (ui.attr.hlActive) {
            double sx = ui.attr.locateSrcX, sy = ui.attr.locateSrcY;
            int targetEpsg = scene.displayEpsg;
            if (targetEpsg == 0) targetEpsg = ui.attr.srcEpsg;
            double dx = sx, dy = sy;
            if (targetEpsg != 0 && ui.attr.srcEpsg != 0 && targetEpsg != ui.attr.srcEpsg) {
                std::vector<float> in = {(float)sx, (float)sy}, out;
                if (reprojectVertices(in, ui.attr.srcEpsg, targetEpsg, out) && out.size() >= 2) {
                    dx = out[0]; dy = out[1];
                }
            }
            double px = (scene.view.centerX - dx) / scene.view.scale;
            double py = (dy - scene.view.centerY) / scene.view.scale;
            scene.pan(px, py);
            ui.viewTouched = true;
        }
    }

    applyLoaderEvents();   // 每帧消费 AsyncLoader 后台结果(渐进绘制)
    backend.pollRebuilds();             // 每帧收取完成后台重建(显示CRS切换)并上传换桶

    // 调试/验证: PEEK_CRS=<epsg> 首帧适配后强制应用一次显示 CRS(测块桶层缓存重读重建路径)
    static const int dbgCrs = [] {
        const char* e = getenv("PEEK_CRS");
        return e && *e ? atoi(e) : 0;
    }();
    if (dbgCrs != 0 && ui.displayCrsChoice == 0 && loaderFirstDataRefit &&
        scene.displayEpsg != dbgCrs) {
        ui.displayCrsChoice = 1;   // 退出自动统一分支, 避免本帧再被 auto 覆盖
        spdlog::info("[CRS] PEEK_CRS force display -> {}", dbgCrs);
        applyDisplayCrs(scene, backend, dbgCrs);
    }

    // 块桶层 CRS 重建: 已进入重建态(applyDisplayCrs 置 blockRebuilding)但未入队的,
    // 每帧触发 loader 从缓存逐块重读重投影任务(rebuildQueued 防重复/支持再次切换)。
    {
        const size_t n = backend.geoms.size();
        if (rebuildQueued.size() < n) rebuildQueued.resize(n, 0);
        for (size_t gi = 0; gi < n; gi++) {
            const auto& gg = backend.geoms[gi];
            if (!gg.blockBuckets || !gg.blockRebuilding) continue;
            if ((int)gi >= (int)scene.layers.size()) continue;
            if (gg.reTarget == 0) continue;
            if (gg.reTarget == rebuildQueued[gi]) continue;
            rebuildQueued[gi] = gg.reTarget;
            loader.enqueueRebuild(scene.layers[gi].sourcePath, (int)gi,
                                  scene.layers[gi].sourceLayerIdx, gg.reTarget);
        }
    }

    // 自动统一显示CRS: 用户未显式指定且尚未统一时, 以"地理坐标系优先"为基准应用一次
    if (ui.displayCrsChoice == 0) {
        int baseEpsg = 0;
        bool haveGeo = false;
        for (size_t k = 0; k < scene.layers.size(); k++) {
            int e = (scene.layers[k].kind == LayerKind::Raster)
                        ? scene.layers[k].raster.srcEpsg
                        : scene.layers[k].data.srcEpsg;
            if (e == 0) continue;
            if (baseEpsg == 0) baseEpsg = e;
            if (!haveGeo && epsgIsGeographic(e)) { baseEpsg = e; haveGeo = true; }
        }
        if (baseEpsg != 0 && baseEpsg != scene.displayEpsg) {
            spdlog::info("[CRS] auto: baseEpsg={} display={} -> apply", baseEpsg, scene.displayEpsg);
            applyDisplayCrs(scene, backend, baseEpsg);
        }
    }

    // 取回属性表后台任务结果
    {
        int curGen = attrGen.load();
        std::lock_guard<std::mutex> lk(attrMtx);
        if (attrInfoReady) {
            attrInfoReady = false;
            if (attrResultGen == curGen) {
                if (attrNeedCount) {
                    ui.attr.info = std::move(attrFetchedInfo);
                    if ((int)ui.attr.showCol.size() != (int)ui.attr.info.fields.size())
                        ui.attr.showCol.assign(ui.attr.info.fields.size(), 1);
                    ui.attr.isOpen = ui.attr.info.ok;
                }
            }
        }
        if (attrPageReady) {
            attrPageReady = false;
            if (attrResultGen == curGen && ui.attr.isOpen && ui.attr.info.ok) {
                applyAttrPage(ui, ui.attr.info, std::move(attrFetchedPage));
                if (!attrNeedCount) ui.attr.loading = false;
            }
        }
        if (attrCountReady) {
            attrCountReady = false;
            if (attrResultGen == curGen) {
                if (attrNeedCount && ui.attr.isOpen && ui.attr.info.ok)
                    ui.attr.info.total = attrFetchedCount;
                ui.attr.loading = false;
            }
        }
    }
    // 若仍绑定图层但打开失败/未成功(首次 keeper 打开慢或失败), 自动有限次重开一次
    if (!ui.attr.loading && !ui.attr.isOpen && !ui.attr.path.empty() &&
        ui.attr.gotoRequested == false && attrOpenRetries < 3) {
        attrOpenRetries++;
        ui.attr.loading = true;
        AttrTaskSpec spec;
        spec.path = ui.attr.path;
        spec.layerIdx = ui.attr.fileLayerIdx;
        spec.page = 0;
        spec.rowsPerPage = ui.attr.rowsPerPage;
        spec.enc = ui.attr.encoding;
        spec.needCount = true;   // 重开仍需总数
        if (!launchAttrTask(spec)) ui.attr.loading = false;  // 忙: 回滚, 下一帧再试
    }
    // 若加载完成后当前页与请求目标不符, 重新下发(极端连点翻页情形)
    if (!ui.attr.loading && ui.attr.isOpen && ui.attr.info.ok) {
        bool haveCur = false;
        for (const auto& p : ui.attr.pages)
            if (p.page == ui.attr.currentPage) { haveCur = true; break; }
        if (!haveCur) {
            ui.attr.loading = true;
            AttrTaskSpec spec;
            spec.path = ui.attr.path;
            spec.layerIdx = ui.attr.fileLayerIdx;
            spec.page = ui.attr.currentPage;
            spec.rowsPerPage = ui.attr.rowsPerPage;
            spec.enc = ui.attr.encoding;
            if (!launchAttrTask(spec)) ui.attr.loading = false;  // 忙: 回滚, 下一帧再试
        }
    }

    // 取回异步识别 - 逐目标增量提交(几何+属性一起), 避免大文件占用时阻塞其他图层
    {
        std::lock_guard<std::mutex> lk(identifyMtx);
        if (identifyGeoReady) {
            identifyGeoReady = false;
            for (auto& h : identifyGeo) {
                h.applyEncoding((TextEncoding)ui.identify.encoding);
                ui.identify.geo.push_back(h);
                ui.identify.full.push_back(std::move(h));
            }
            identifyGeo.clear();
        }
        if (identifyDone) {
            identifyDone = false;
            ui.identify.pending = false;
            if (identifyRemaining == 0 && ui.identify.full.empty()) {
                ui.status = "未命中要素";
                ui.statusErr = true;
            }
        }
    }
    if (ui.identify.pending && !ui.identify.geo.empty()) {
        ui.status = "属性查询中...";
        ui.statusErr = false;
    }

    // 发起异步属性识别: 双击地图后抓拍可见图层快照, 后台线程逐层查询
    if (ui.identify.requested) {
        ui.identify.requested = false;
        ui.identify.full.clear();
        ui.identify.geo.clear();
        ui.identify.pending = true;
        ui.status = "属性查询中...";
        ui.statusErr = false;
        std::vector<IdentifyTarget> targets;
        for (const auto& L : scene.layers) {
            if (!L.info.visible) continue;
            if (L.sourcePath.empty()) continue;
            IdentifyTarget t;
            t.path = L.sourcePath;
            t.layerIdx = L.sourceLayerIdx;
            t.srcEpsg = (L.kind == LayerKind::Raster) ? L.raster.srcEpsg : L.data.srcEpsg;
            if (L.kind == LayerKind::Raster) {
                t.isRaster = true;
                t.openSpec = L.raster.openSpec;
                std::copy(L.raster.geo, L.raster.geo + 6, t.geo);
                t.width = L.raster.width;
                t.height = L.raster.height;
            }
            targets.push_back(std::move(t));
        }
        double qx = ui.identify.x, qy = ui.identify.y;
        double tol = 10.0 * scene.view.scale;   // 10 像素点击容差(显示单位)
        int dispEpsg = scene.displayEpsg;
        if (targets.empty()) {
            ui.identify.pending = false;
            ui.status = "未命中要素";
            ui.statusErr = true;
        } else {
            uint64_t gen = identifyGen.fetch_add(1) + 1;
            ui.identify.full.clear();
            ui.identify.geo.clear();
            ui.identify.pending = true;
            {
                std::lock_guard<std::mutex> lk(identifyMtx);
                identifyRemaining = (int)targets.size();
                identifyDone = false;
            }
            // 每个目标一个线程: 某文件首次打开(几十秒)不阻塞其他图层的查询
            for (const auto& t : targets) {
                bgThreads_.push_back(std::thread([this, gen, t, dispEpsg, qx, qy, tol]() {
                    IdentifyHit h;
                    bool ok = false;
                    if (t.isRaster) {
                        int col = 0, row = 0;
                        double sx = 0, sy = 0;
                        std::vector<double> vals;
                        if (identifyRasterSample(t.openSpec, t.geo, t.width, t.height,
                                                 qx, qy, dispEpsg, t.srcEpsg,
                                                 col, row, sx, sy, vals)) {
                            h.layerName = baseName(t.path);
                            h.geomType = "RASTER";
                            h.srcEpsg = t.srcEpsg;
                            h.hasRasterPoint = true;
                            h.rasterX = qx;
                            h.rasterY = qy;
                            auto add = [&h](const char* n, const std::string& v) {
                                IdentifyAttr a; a.name = n; a.value = v; h.attrs.push_back(std::move(a));
                            };
                            add("像方列", std::to_string(col));
                            add("像方行", std::to_string(row));
                            char buf[128];
                            std::snprintf(buf, sizeof buf, "%.6f", sx);
                            add("源 坐标 X", buf);
                            std::snprintf(buf, sizeof buf, "%.6f", sy);
                            add("源 坐标 Y", buf);
                            if (dispEpsg != 0 && t.srcEpsg != 0 && dispEpsg != t.srcEpsg) {
                                std::snprintf(buf, sizeof buf, "%.6f", qx);
                                add("显示 坐标 X", buf);
                                std::snprintf(buf, sizeof buf, "%.6f", qy);
                                add("显示 坐标 Y", buf);
                            }
                            for (size_t i = 0; i < vals.size(); i++) {
                                std::string nm = "波段 " + std::to_string((int)i + 1) + " 原值";
                                std::string vv;
                                double v = vals[i];
                                if (std::isnan(v)) {
                                    vv = "读取失败";
                                } else if (v == std::floor(v) && std::fabs(v) < 1e15) {
                                    vv = std::to_string((long long)v);
                                } else {
                                    char b[96];
                                    std::snprintf(b, sizeof b, "%.8g", v);
                                    vv = b;
                                }
                                add(nm.c_str(), vv);
                            }
                            ok = true;
                        }
                    } else {
                        ok = identifyFeatures(t.path, t.layerIdx, qx, qy, tol,
                                              dispEpsg, t.srcEpsg, h);
                    }
                    {
                        std::lock_guard<std::mutex> lk(identifyMtx);
                        if (gen == identifyGen.load()) {
                            if (ok) {
                                identifyGeo.push_back(std::move(h));
                                identifyGeoReady = true;
                            }
                            identifyRemaining--;
                            if (identifyRemaining <= 0) {
                                identifyRemaining = 0;
                                identifyDone = true;
                            }
                        }
                    }
                }));
            }
        }
    }

    // 进度填充
    LoadStats st = loader.stats();
    if (st.activeFiles > 0) {
        ui.loadActive = true;
        ui.loadFraction = st.fraction();
        ui.loadDoneFiles = st.doneFiles;
        ui.loadTotalFiles = st.totalFiles;
    } else {
        ui.loadActive = false;
    }

    refreshRasterQueue();   // 唤醒栅格背景 worker(空闲不做事, 有 spec 才处理)
    pumpRasterDetail(scene, backend);   // 细节块后台读取 + 上传(限速, 不影响交互)

    ImGui::Render();
    int w, h; glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
    backend.debugFrameStats();   // PEEK_DEBUG_FRAME=1: 帧间 dt + render 耗时统计
}