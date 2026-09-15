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
#include "render/raster_pyramid.h"
#include "platform/exe_path.h"
#include "util/logger.h"
#include <zstd.h>
#include <fstream>
#include <iterator>

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

// 缓存根目录: 相对路径按 exe 目录解析(与 v1.0 几何缓存一致), 不受启动工作目录影响。
static std::string resolvedCacheDir(const AppConfig& cfg) {
    std::string d = cfg.cache_dir.empty() ? "cache" : cfg.cache_dir;
    std::filesystem::path p(d);
    if (!p.is_absolute()) {
        std::string e = exeDir();
        if (!e.empty()) d = e + "/" + d;
    }
    return d;
}

// 烘焙纹理缓存路径(按 源路径哈希 + 显示CRS 区分)
std::string App::bakeCachePath(const std::string& src, int dstEpsg, int level, int tx, int ty) const {
    std::error_code ec;
    std::string dir = resolvedCacheDir(cfg) + "/bake";
    // 每图层一个子目录: <路径哈希前8位>_<扩展名>[_epsg<dst>](纯 ASCII, 避免中文路径编码问题)
    char hb[24];
    std::snprintf(hb, sizeof(hb), "%zx", std::hash<std::string>{}(src));
    std::string tag = "dat";
    size_t dot = src.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < src.size()) {
        tag = src.substr(dot + 1);
        for (auto& ch : tag) ch = (char)std::tolower((unsigned char)ch);
    }
    std::string sub = dir + "/" + std::string(hb).substr(0, 8) + "_" + tag;
    if (dstEpsg != 0) sub += "_epsg" + std::to_string(dstEpsg);
    std::filesystem::create_directories(sub, ec);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/L%d_%d_%d.bin", level, tx, ty);
    return sub + buf;
}

// 瓦片磁盘缓存(zstd 压缩 R8)
static bool saveTileDisk(const std::string& path, const std::vector<unsigned char>& px) {
    std::vector<char> comp(ZSTD_compressBound(px.size()));
    size_t cz = ZSTD_compress(comp.data(), comp.size(), px.data(), px.size(), 1);   // level 1: 轻量
    if (ZSTD_isError(cz)) return false;
    std::ofstream of(path, std::ios::binary);
    if (!of) return false;
    uint64_t n = (uint64_t)px.size();
    of.write((const char*)&n, 8);
    of.write(comp.data(), (std::streamsize)cz);
    return (bool)of;
}

static bool loadTileDisk(const std::string& path, std::vector<unsigned char>& px) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    uint64_t n = 0;
    in.read((char*)&n, 8);
    if (!in || n == 0 || n > (1u << 26)) return false;
    std::vector<char> comp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (comp.empty()) return false;
    px.resize((size_t)n);
    size_t got = ZSTD_decompress(px.data(), px.size(), comp.data(), comp.size());
    return !ZSTD_isError(got) && got == px.size();
}

// over-zoom: 按 bbox(源CRS) 从原始数据查询要素几何(线/点/面填充, 源坐标)
// ---- 烘焙高层片空间索引: OGR shp 的 SetSpatialFilter 默认是全扫(每片重扫上千万段 ~15s),
//     不可用。这里用一次性全扫把每要素 bbox 收进内存数组, 之后每片只线性匹配 bbox -> .shx 随机读。----
struct BakeSpaceIndex {
    std::string path;
    int layerIdx = -1;
    std::atomic<bool> ready{false};   // release/acquire 保证 bb 可见
    std::vector<float> bb;            // feature i -> [4i..4i+3]=minx,miny,maxx,maxy
};
struct BakeIdxStore {
    std::mutex m;
    std::vector<BakeSpaceIndex*> all;
    std::deque<std::pair<std::string, int>> jobs;   // {path, layerIdx} 待构建
} g_bakeIdx;

static BakeSpaceIndex* findBakeSpaceIndex(const std::string& path, int layerIdx) {
    std::lock_guard<std::mutex> lk(g_bakeIdx.m);
    for (auto* si : g_bakeIdx.all)
        if (si->path == path && si->layerIdx == layerIdx) return si;
    return nullptr;
}

static BakeSpaceIndex* enqueueBakeSpaceIndex(const std::string& path, int layerIdx) {
    std::lock_guard<std::mutex> lk(g_bakeIdx.m);
    for (auto* si : g_bakeIdx.all)
        if (si->path == path && si->layerIdx == layerIdx) return si;
    auto* si = new BakeSpaceIndex();
    si->path = path;
    si->layerIdx = layerIdx;
    g_bakeIdx.all.push_back(si);
    g_bakeIdx.jobs.emplace_back(path, layerIdx);
    return si;
}

static void bakeSpaceIndexBuildOne(BakeSpaceIndex& si) {
    GDALDatasetH ds = peekg::data::gdalOpenVector(si.path);
    if (!ds) return;
    OGRLayerH lyr = GDALDatasetGetLayer(ds, si.layerIdx);
    if (!lyr) { GDALClose(ds); return; }
    std::vector<float> bb;
    OGR_L_ResetReading(lyr);
    OGRFeatureH feat;
    while ((feat = OGR_L_GetNextFeature(lyr)) != nullptr) {
        OGRGeometryH g = OGR_F_GetGeometryRef(feat);
        if (g) {
            OGREnvelope env;
            OGR_G_GetEnvelope(g, &env);
            bb.push_back((float)env.MinX); bb.push_back((float)env.MinY);
            bb.push_back((float)env.MaxX); bb.push_back((float)env.MaxY);
        }
        OGR_F_Destroy(feat);
    }
    si.bb = std::move(bb);
    si.ready.store(true, std::memory_order_release);   // bb 写入完成后再置位
    GDALClose(ds);
}

static bool queryBakeWithIndex(const BakeSpaceIndex* si, double minx, double miny,
                               double maxx, double maxy, OGRLayerH lyr,
                               std::vector<float>& v, std::vector<float>& p, std::vector<float>& f) {
    const std::vector<float>& bb = si->bb;
    const size_t n = bb.size() >> 2;
    std::vector<long long> fids;
    fids.reserve(512);
    for (size_t i = 0; i < n; i++) {
        const float* b = &bb[i * 4];
        if (b[0] <= maxx && b[2] >= minx && b[1] <= maxy && b[3] >= miny)
            fids.push_back((long long)i);
    }
    if (fids.empty()) return true;
    for (long long fid : fids) {
        OGRFeatureH feat = OGR_L_GetFeature(lyr, fid);   // .shx 随机读
        if (!feat) continue;
        peekg::data::addFilledGeometry(OGR_F_GetGeometryRef(feat), v, f, &p);
        OGR_F_Destroy(feat);
    }
    return true;
}

static bool queryGeomInBBox(const std::string& path, int layerIdx,
                            double minx, double miny, double maxx, double maxy,
                            std::vector<float>& v, std::vector<float>& p, std::vector<float>& f) {
    GDALDatasetH ds = peekg::data::gdalOpenVector(path);
    if (!ds) return false;
    OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);
    if (!lyr) { GDALClose(ds); return false; }
    OGR_L_SetSpatialFilterRect(lyr, minx, miny, maxx, maxy);
    OGR_L_ResetReading(lyr);
    OGRFeatureH feat;
    while ((feat = OGR_L_GetNextFeature(lyr)) != nullptr) {
        peekg::data::addFilledGeometry(OGR_F_GetGeometryRef(feat), v, f, &p);
        OGR_F_Destroy(feat);
    }
    OGR_L_SetSpatialFilter(lyr, nullptr);
    GDALClose(ds);
    return true;
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
    bakeStop_.store(true);
    for (auto& t : bakeThreads_) if (t.joinable()) t.join();
    bakeThreads_.clear();
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
            scene.layers.erase(scene.layers.begin() + i);
        }

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
        L.bake = (meta[li].featureCount > cfg.bake_threshold_features);   // 大数据走烘焙; 小数据走原矢量路径
        L.openSeq = ++openSeqCounter_;
        const float* c = kPalette[(s_layerColorIdx++) % 16];
        L.color[0] = c[0]; L.color[1] = c[1]; L.color[2] = c[2];
        scene.layers.push_back(std::move(L));
        backend.addLayerPlaceholder();
    }

    return loader.enqueue(path, cfg, meta, idxs, globalBase, false);   // 渲染走烘焙图, 不写几何缓存
}

// 每帧消费 AsyncLoader 的完成/块事件, 应用到 scene/backend(原 AsyncLoader::update 主体)
// 估算烘焙最深层: 与矢量瓦片同一算法(采样要素 -> 各层格化点数 P(L) -> 取 P(L)/4^L 最接近 target)。
// 广东(DLTB, 459万面)同法算出 L8。
static int estimateBakeMaxLevel(const std::string& path, int layerIdx, int dstEpsg,
                                int targetPerTile, int cap) {
    peekg::data::ensureGdal();
    GDALDatasetH ds = peekg::data::gdalOpenVector(path);
    if (!ds) return 6;
    int nl = GDALDatasetGetLayerCount(ds);
    if (layerIdx < 0 || layerIdx >= nl) { GDALClose(ds); return 6; }
    OGRLayerH lyr = GDALDatasetGetLayer(ds, layerIdx);
    OGRSpatialReferenceH srcSrs = OGR_L_GetSpatialRef(lyr);
    int srcEpsg = peekg::data::gdalSrsEpsg(srcSrs);
    int dst = dstEpsg > 0 ? dstEpsg : srcEpsg;
    OGRCoordinateTransformationH ct = nullptr;
    if (dst > 0 && srcEpsg > 0 && dst != srcEpsg && srcSrs) {
        OGRSpatialReferenceH d = OSRNewSpatialReference(nullptr);
        if (OSRImportFromEPSG(d, dst) == OGRERR_NONE)
            ct = OCTNewCoordinateTransformation(srcSrs, d);
        OSRDestroySpatialReference(d);
    }
    OGREnvelope env;
    bool hasExt = OGR_L_GetExtent(lyr, &env, TRUE) == OGRERR_NONE;
    double minx = 0, miny = 0, maxx = 0, maxy = 0;
    if (hasExt) {
        minx = env.MinX; miny = env.MinY; maxx = env.MaxX; maxy = env.MaxY;
        if (ct) {
            const double xs[4] = {env.MinX, env.MaxX, env.MinX, env.MaxX};
            const double ys[4] = {env.MinY, env.MinY, env.MaxY, env.MaxY};
            minx = miny = 1e300; maxx = maxy = -1e300;
            for (int i = 0; i < 4; ++i) {
                double x = xs[i], y = ys[i];
                if (OCTTransform(ct, 1, &x, &y, nullptr)) {
                    minx = std::min(minx, x); maxx = std::max(maxx, x);
                    miny = std::min(miny, y); maxy = std::max(maxy, y);
                }
            }
            if (minx > maxx) { minx = env.MinX; miny = env.MinY; maxx = env.MaxX; maxy = env.MaxY; }
        }
    }
    double S = hasExt ? std::max(maxx - minx, maxy - miny) : 1.0;
    if (S <= 0) S = 1.0;
    long long F = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
    if (F <= 0) { if (ct) OCTDestroyCoordinateTransformation(ct); GDALClose(ds); return 6; }

    double spanX = maxx - minx, spanY = maxy - miny;
    double originX = minx - (S - spanX) / 2, originY = miny - (S - spanY) / 2;
    int C = std::max(0, cap);
    std::vector<double> cell(C + 1);
    for (int L = 0; L <= C; ++L) cell[L] = S / (512.0 * std::pow(2.0, L));

    auto snapRing = [](OGRGeometryH ring, double ox, double oy, double c) -> long long {
        int n = OGR_G_GetPointCount(ring);
        long long cnt = 0; long px = 0, py = 0; bool has = false;
        for (int i = 0; i < n; ++i) {
            long gx = std::lround((OGR_G_GetX(ring, i) - ox) / c);
            long gy = std::lround((OGR_G_GetY(ring, i) - oy) / c);
            if (has && gx == px && gy == py) continue;
            ++cnt; px = gx; py = gy; has = true;
        }
        return cnt;
    };
    std::function<long long(OGRGeometryH, double, double, double)> snapGeom;
    snapGeom = [&](OGRGeometryH g, double ox, double oy, double c) -> long long {
        if (!g) return 0;
        OGRwkbGeometryType t = wkbFlatten(OGR_G_GetGeometryType(g));
        switch (t) {
            case wkbPolygon: {
                long long s = 0; int nr = OGR_G_GetGeometryCount(g);
                for (int r = 0; r < nr; ++r) {
                    long long k = snapRing(OGR_G_GetGeometryRef(g, r), ox, oy, c);
                    if (k >= 3) s += k;
                }
                return s;
            }
            case wkbLineString:
            case wkbLinearRing: { long long k = snapRing(g, ox, oy, c); return k >= 2 ? k : 0; }
            case wkbPoint: return 1;
            case wkbMultiPoint:
            case wkbMultiPolygon:
            case wkbMultiLineString:
            case wkbGeometryCollection: {
                long long s = 0; int ng = OGR_G_GetGeometryCount(g);
                for (int i = 0; i < ng; ++i) s += snapGeom(OGR_G_GetGeometryRef(g, i), ox, oy, c);
                return s;
            }
            default: return 0;
        }
    };

    const long long sampleK = 10000;
    std::vector<double> sumS(C + 1, 0.0);
    long long n = 0;
    OGR_L_ResetReading(lyr);
    OGRFeatureH f;
    while (n < sampleK && (f = OGR_L_GetNextFeature(lyr)) != nullptr) {
        OGRGeometryH g = OGR_F_GetGeometryRef(f);
        if (g) {
            OGRGeometryH gg = g, owned = nullptr;
            if (ct) { owned = OGR_G_Clone(g); OGR_G_Transform(owned, ct); gg = owned; }
            for (int L = 0; L <= C; ++L) sumS[L] += (double)snapGeom(gg, originX, originY, cell[L]);
            if (owned) OGR_G_DestroyGeometry(owned);
            ++n;
        }
        OGR_F_Destroy(f);
    }
    if (ct) OCTDestroyCoordinateTransformation(ct);
    GDALClose(ds);
    if (n == 0) return 6;

    int best = 0; double bestErr = 1e300;
    for (int L = 0; L <= C; ++L) {
        double P = (sumS[L] / (double)n) * (double)F;
        double r = P / std::pow(4.0, L);
        double err = std::fabs(r - (double)targetPerTile);
        if (err < bestErr) { bestErr = err; best = L; }
    }
    return best;
}

void App::startBakeWorkers() {
    if (bakeStarted_) return;
    bakeStarted_ = true;
    int n = std::min(4, (int)std::max(1u, std::thread::hardware_concurrency()));
    n = 1;   // 单线程烘焙: 避免多个 worker 并发打开同一大 shp 互锁(查询由内存索引加速)
    for (int i = 0; i < n; i++)
        bakeThreads_.emplace_back([this] { bakeWorker(); });
    bakeThreads_.emplace_back([this] { bakeIndexLoop(); });   // 索引构建线程
}

void App::bakeIndexLoop() {
    for (;;) {
        std::pair<std::string, int> job;
        {
            std::lock_guard<std::mutex> lk(g_bakeIdx.m);
            if (!g_bakeIdx.jobs.empty()) {
                job = g_bakeIdx.jobs.front();
                g_bakeIdx.jobs.pop_front();
            } else {
                if (bakeStop_.load()) return;
            }
        }
        if (job.first.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        BakeSpaceIndex* si = findBakeSpaceIndex(job.first, job.second);
        if (si && !si->ready.load(std::memory_order_acquire)) {
            if (getenv("PEEK_DEBUG_DRAW"))
                spdlog::info("[IDX] build space index for {}", job.first);
            bakeSpaceIndexBuildOne(*si);
            if (getenv("PEEK_DEBUG_DRAW"))
                spdlog::info("[IDX] done, {} features", (int)(si->bb.size() >> 2));
        }
    }
}

void App::bakeWorker() {
    for (;;) {
        BakeJob job;
        {
            std::lock_guard<std::mutex> lk(bakeMtx_);
            if (!bakeJobs_.empty()) {
                job = bakeJobs_.front();
                bakeJobs_.pop_front();
            } else {
                if (bakeStop_.load()) return;
            }
        }
        if (job.path.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        std::vector<float> v, p, f;
        auto bwt0 = std::chrono::steady_clock::now();
        bool usedIdx = false;
        {
            BakeSpaceIndex* si = enqueueBakeSpaceIndex(job.path, job.srcLayerIdx);
            if (si && !si->ready.load(std::memory_order_acquire)) {
                // 空间索引还在建: 不做全表扫描(459万要素每片 ~15s 且与索引线程抢文件),
                // 稍后重试; 等索引就绪后每片查询是毫秒级。
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::lock_guard<std::mutex> lk(bakeMtx_);
                bakeJobs_.push_front(std::move(job));
                continue;
            }
            if (si) {
                GDALDatasetH idxDs = peekg::data::gdalOpenVector(job.path);
                if (idxDs) {
                    OGRLayerH idxLyr = GDALDatasetGetLayer(idxDs, job.srcLayerIdx);
                    if (idxLyr)
                        usedIdx = queryBakeWithIndex(si, job.sx0, job.sy0, job.sx1, job.sy1,
                                                     idxLyr, v, p, f);
                    GDALClose(idxDs);
                }
            }
        }
        if (!usedIdx)
            queryGeomInBBox(job.path, job.srcLayerIdx, job.sx0, job.sy0, job.sx1, job.sy1, v, p, f);
        if (getenv("PEEK_DEBUG_DRAW"))
            spdlog::info("[BW] worker done v={} p={} f={} took {:.1f}s",
                         (int)v.size(), (int)p.size(), (int)f.size(),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - bwt0).count());
        if (job.dstEpsg != 0 && job.srcEpsg != 0 && job.srcEpsg != job.dstEpsg) {
            std::vector<float> rv, rp, rf;
            if (reprojectVertices(v, job.srcEpsg, job.dstEpsg, rv)) v.swap(rv);
            if (reprojectVertices(p, job.srcEpsg, job.dstEpsg, rp)) p.swap(rp);
            if (reprojectVertices(f, job.srcEpsg, job.dstEpsg, rf)) f.swap(rf);
        }
        BakeResult r;
        r.layer = job.layer; r.level = job.level; r.tx = job.tx; r.ty = job.ty;
        r.dstEpsg = job.dstEpsg;
        r.ozType = job.ozType;
        r.v = std::move(v); r.p = std::move(p); r.f = std::move(f);
        std::lock_guard<std::mutex> lk(bakeMtx_);
        bakeResults_.push_back(std::move(r));
    }
}

// 烘焙 LOD: 按需烘焙视口瓦片; 超过最细级则 over-zoom 查原始数据
// 提取几何为 线/点/环(世界坐标), 供 GPU 模板填充(面不需要耳切)
static void collectGeomRings(OGRGeometryH g, std::vector<float>& lines,
                             std::vector<float>& points, std::vector<std::vector<float>>& rings) {
    if (!g) return;
    OGRwkbGeometryType t = wkbFlatten(OGR_G_GetGeometryType(g));
    if (t == wkbMultiPolygon || t == wkbMultiLineString || t == wkbMultiPoint ||
        t == wkbGeometryCollection) {
        int ng = OGR_G_GetGeometryCount(g);
        for (int i = 0; i < ng; ++i) collectGeomRings(OGR_G_GetGeometryRef(g, i), lines, points, rings);
        return;
    }
    if (t == wkbPolygon) {
        int nr = OGR_G_GetGeometryCount(g);
        for (int r = 0; r < nr; ++r) {
            OGRGeometryH ring = OGR_G_GetGeometryRef(g, r);
            int np = OGR_G_GetPointCount(ring);
            if (np < 3) continue;
            std::vector<float> xy;
            xy.reserve((size_t)np * 2);
            for (int i = 0; i < np; ++i) {
                xy.push_back((float)OGR_G_GetX(ring, i));
                xy.push_back((float)OGR_G_GetY(ring, i));
            }
            rings.push_back(std::move(xy));
        }
    } else if (t == wkbLineString || t == wkbLinearRing) {
        int np = OGR_G_GetPointCount(g);
        for (int i = 0; i + 1 < np; ++i) {
            lines.push_back((float)OGR_G_GetX(g, i));     lines.push_back((float)OGR_G_GetY(g, i));
            lines.push_back((float)OGR_G_GetX(g, i + 1)); lines.push_back((float)OGR_G_GetY(g, i + 1));
        }
    } else if (t == wkbPoint) {
        points.push_back((float)OGR_G_GetX(g, 0));
        points.push_back((float)OGR_G_GetY(g, 0));
    }
}

// GPU 建栅格金字塔(主线程): 逐要素用 GL 光栅化进最深层 FBO(显存有界), 再逐层 2x2 降采样。
// 每片存 R8 覆盖度; 渲染 shader 乘颜色。
void App::buildRasterPyramidGpu(int gi) {
    if (gi < 0 || gi >= (int)scene.layers.size()) return;
    MapLayer& L = scene.layers[gi];
    std::string src = L.sourcePath;
    int srcEpsg = L.data.srcEpsg;
    int dst = scene.displayEpsg != 0 ? scene.displayEpsg : srcEpsg;
    int Lmax = L.bakeMaxLevel;
    const int res = GLBackend::kTileRes;
    double minx = L.bakeMinx, miny = L.bakeMiny, maxx = L.bakeMaxx, maxy = L.bakeMaxy;
    double spanX = maxx - minx, spanY = maxy - miny, S = std::max(spanX, spanY);
    if (S <= 0) return;
    double originX = minx - (S - spanX) / 2, originY = miny - (S - spanY) / 2;
    int n = 1 << Lmax;
    double tileW = S / n;

    GDALDatasetH ds = peekg::data::gdalOpenVector(src);
    if (!ds) return;
    OGRLayerH lyr = GDALDatasetGetLayer(ds, L.sourceLayerIdx);
    if (!lyr) { GDALClose(ds); return; }
    OGRSpatialReferenceH srcSrs = OGR_L_GetSpatialRef(lyr);
    OGRCoordinateTransformationH ct = nullptr;
    if (dst > 0 && srcEpsg > 0 && dst != srcEpsg && srcSrs) {
        OGRSpatialReferenceH d = OSRNewSpatialReference(nullptr);
        if (OSRImportFromEPSG(d, dst) == OGRERR_NONE) ct = OCTNewCoordinateTransformation(srcSrs, d);
        OSRDestroySpatialReference(d);
    }
    long long F = (long long)OGR_L_GetFeatureCount(lyr, TRUE);
    if (F <= 0) F = 1;

    // 显存有界: 把当前驻留片 dump 存盘 + 清空
    auto dumpAll = [&]() {
        if (gi < (int)backend.bakes.size()) {
            for (auto& kv : backend.bakes[gi].tiles) {
                int lv = (int)((kv.first >> 40) & 0xffffff);
                int ty = (int)((kv.first >> 20) & 0xfffff);
                int tx = (int)(kv.first & 0xfffff);
                std::vector<unsigned char> px;
                if (backend.dumpBakeTile(gi, lv, tx, ty, px))
                    saveTileDisk(bakeCachePath(src, dst, lv, tx, ty), px);
            }
        }
        backend.evictBakeTiles(gi, 0);   // 释放显存
    };

    OGR_L_ResetReading(lyr);
    OGRFeatureH feat;
    long long fi = 0;
    std::vector<float> v, p;
    std::vector<std::vector<float>> rings;
    while ((feat = OGR_L_GetNextFeature(lyr)) != nullptr) {
        OGRGeometryH g = OGR_F_GetGeometryRef(feat);
        if (g) {
            OGRGeometryH gg = g, owned = nullptr;
            if (ct) { owned = OGR_G_Clone(g); OGR_G_Transform(owned, ct); gg = owned; }
            v.clear(); p.clear(); rings.clear();
            collectGeomRings(gg, v, p, rings);
            if (!rings.empty() || !v.empty() || !p.empty()) {
                OGREnvelope env; OGR_G_GetEnvelope(gg, &env);
                int tx0 = std::max(0, std::min(n - 1, (int)std::floor((env.MinX - originX) / tileW)));
                int tx1 = std::max(0, std::min(n - 1, (int)std::floor((env.MaxX - originX) / tileW)));
                int ty0 = std::max(0, std::min(n - 1, (int)std::floor((env.MinY - originY) / tileW)));
                int ty1 = std::max(0, std::min(n - 1, (int)std::floor((env.MaxY - originY) / tileW)));
                for (int ty = ty0; ty <= ty1; ++ty)
                    for (int tx = tx0; tx <= tx1; ++tx) {
                        uint64_t k = backend.tileKey(Lmax, tx, ty);
                        if (gi >= (int)backend.bakes.size() ||
                            backend.bakes[gi].tiles.find(k) == backend.bakes[gi].tiles.end()) {
                            std::vector<unsigned char> px;   // 已存盘则先读回(继续累加)
                            if (loadTileDisk(bakeCachePath(src, dst, Lmax, tx, ty), px) &&
                                px.size() == (size_t)res * res)
                                backend.uploadBakeTile(gi, Lmax, tx, ty, px.data(), res);
                        }
                        backend.bakeTileBegin(gi, Lmax, tx, ty);
                        backend.bakeTileAppendRings(gi, v, p, rings);
                    }
            }
            if (owned) OGR_G_DestroyGeometry(owned);
        }
        OGR_F_Destroy(feat);
        ++fi;
        if ((fi % 50000) == 0) dumpAll();   // 显存有界
    }
    dumpAll();
    if (ct) OCTDestroyCoordinateTransformation(ct);
    GDALClose(ds);
    spdlog::info("[bake] GPU 金字塔最深层完成, {} 要素", fi);

    // 逐层 2x2 降采样(读盘 -> 取 max -> 存盘)
    std::vector<unsigned char> child((size_t)res * res), parent((size_t)res * res);
    for (int lv = Lmax - 1; lv >= 0; --lv) {
        int m = 1 << lv;
        for (int ty = 0; ty < m; ++ty)
            for (int tx = 0; tx < m; ++tx) {
                std::fill(parent.begin(), parent.end(), 0);
                for (int cy = 0; cy < 2; ++cy)
                    for (int cx = 0; cx < 2; ++cx) {
                        std::vector<unsigned char> c;
                        if (!loadTileDisk(bakeCachePath(src, dst, lv + 1, tx * 2 + cx, ty * 2 + cy), c) ||
                            c.size() != (size_t)res * res) continue;
                        int off = (cy * res / 2) * res + cx * res / 2;
                        for (int y = 0; y < res / 2; ++y)
                            for (int x = 0; x < res / 2; ++x) {
                                unsigned char val = c[(size_t)(y * 2) * res + x * 2];
                                unsigned char& pp = parent[(size_t)(off + y * res + x)];
                                if (val > pp) pp = val;
                            }
                    }
                saveTileDisk(bakeCachePath(src, dst, lv, tx, ty), parent);
            }
    }
    spdlog::info("[bake] GPU 金字塔降采样完成");
}

void App::updateOverZoom() {
    startBakeWorkers();
    int nBake = 0, nOver = 0, lastLv = -1, maxBakeLv = -1;
    // 收后台烘焙结果 -> 烘 GPU
    {
        std::lock_guard<std::mutex> lk(bakeMtx_);
        for (auto& r : bakeResults_) {
            if (r.ozType) {
                if (r.layer >= 0 && r.layer < (int)scene.layers.size())
                    backend.setOverZoom(r.layer, r.v, r.p, r.f, scene.layers[r.layer].color);
            } else {
                backend.bakeTileBegin(r.layer, r.level, r.tx, r.ty);
                backend.bakeTileAppend(r.layer, r.v, r.p, r.f);
                backend.bakeTileEnd(r.layer);
                std::vector<unsigned char> px;
                if (backend.dumpBakeTile(r.layer, r.level, r.tx, r.ty, px) &&
                    r.layer >= 0 && r.layer < (int)scene.layers.size())
                    saveTileDisk(bakeCachePath(scene.layers[r.layer].sourcePath, r.dstEpsg, r.level, r.tx, r.ty), px);
            }
            if (r.ozType)
                bakePending_.erase(((uint64_t)r.layer << 48) | 0xFFFFFFFFu);   // over-zoom 专用 key
            else
                bakePending_.erase(((uint64_t)r.layer << 48) ^ backend.tileKey(r.level, r.tx, r.ty));
        }
        bakeResults_.clear();
    }
    if (scene.layers.empty()) return;
    double halfW = scene.view.vpW * 0.5 * scene.view.scale;
    double halfH = scene.view.vpH * 0.5 * scene.view.scale;
    double vx0 = scene.view.centerX - halfW, vx1 = scene.view.centerX + halfW;
    double vy0 = scene.view.centerY - halfH, vy1 = scene.view.centerY + halfH;
    if (ozState.size() < scene.layers.size()) ozState.resize(scene.layers.size());
    if (scene.layers.size() > backend.bakes.size()) {
        static long long nbb = 0;
        if (nbb++ % 30 == 0)
            spdlog::info("[DBG] layers={} bakes={} 有层被烘焙循环跳过!", (int)scene.layers.size(), (int)backend.bakes.size());
    }
    for (size_t i = 0; i < scene.layers.size() && i < backend.bakes.size(); i++) {
        MapLayer& L = scene.layers[i];
        if (L.kind != LayerKind::Vector) continue;
        if (!backend.hasBakeBounds((int)i)) {
            static long long hbn = 0;
            if (hbn++ % 30 == 0)
                spdlog::info("[DBG] layer[{}] 无烘焙范围, 跳过渲染!", (int)i);
            continue;
        }
        int srcEpsg = L.data.srcEpsg;
        bool crossCrs = (scene.displayEpsg != 0 && srcEpsg != 0 && scene.displayEpsg != srcEpsg);
        int Lv = backend.bakeLevelFor((int)i, scene);
        {
            static long long dl = 0;
            if (dl++ % 5 == 0) {
                int rg0 = -1, rg1 = -1, rgt0 = -1, rgt1 = -1;
                if (Lv >= 0) backend.bakeTileRange((int)i, Lv, scene, rgt0, rgt1, rg0, rg1);
                spdlog::info("[DL] layer[{}] Lv={} scale={:.9f} bbox=({:.4f},{:.4f},{:.4f},{:.4f}) view=({:.4f},{:.4f},{:.4f},{:.4f}) rangeX=[{}..{}] rangeY=[{}..{}]",
                             (int)i, Lv, scene.view.scale,
                             L.bakeMinx, L.bakeMiny, L.bakeMaxx, L.bakeMaxy,
                             vx0, vy0, vx1, vy1, rgt0, rg0, rgt1, rg1);
            }
        }
        OzState& st = ozState[i];
        if (Lv < 0) {
            // ---- over-zoom: 放大超过最细烘焙级, 查原始数据 ----
            nOver++;
            bool need = !st.valid;
            if (st.valid) {
                double moved = std::max(std::fabs(scene.view.centerX - st.cx), std::fabs(scene.view.centerY - st.cy));
                double ratio = scene.view.scale / st.scale;
                if (moved > halfW * 0.25 || ratio < 0.8 || ratio > 1.25) need = true;
            }
            if (!need) continue;
            double sx0 = vx0, sy0 = vy0, sx1 = vx1, sy1 = vy1;
            if (crossCrs) {
                const double cx4[4] = {vx0, vx1, vx0, vx1};
                const double cy4[4] = {vy0, vy0, vy1, vy1};
                double rx[4], ry[4];
                bool ok = true;
                for (int q = 0; q < 4; q++)
                    if (!reprojectPoint(cx4[q], cy4[q], scene.displayEpsg, srcEpsg, rx[q], ry[q])) { ok = false; break; }
                if (ok) {
                    sx0 = *std::min_element(rx, rx + 4); sx1 = *std::max_element(rx, rx + 4);
                    sy0 = *std::min_element(ry, ry + 4); sy1 = *std::max_element(ry, ry + 4);
                }
            }
            // 异步: 入队后台 bbox 查询, 主线程不阻塞(拖动不卡); 结果回来前继续显示烘焙图
            const uint64_t ozKey = ((uint64_t)i << 48) | 0xFFFFFFFFu;
            BakeJob job;
            job.layer = (int)i; job.ozType = 1; job.level = -1; job.tx = -1; job.ty = -1;
            job.srcLayerIdx = L.sourceLayerIdx; job.srcEpsg = srcEpsg; job.dstEpsg = scene.displayEpsg;
            job.path = L.sourcePath;
            job.sx0 = sx0; job.sy0 = sy0; job.sx1 = sx1; job.sy1 = sy1;
            {
                std::lock_guard<std::mutex> lk(bakeMtx_);
                if (bakePending_.count(ozKey)) continue;   // 该层已有查询在跑
                if (bakeJobs_.size() > 64) continue;       // 队列上限, 防积压
                bakeJobs_.push_back(std::move(job));
                bakePending_.insert(ozKey);
            }
            st.cx = scene.view.centerX; st.cy = scene.view.centerY; st.scale = scene.view.scale; st.valid = true;
            continue;
        }
        // ---- 按需烘焙(异步): 视口缺片入队后台查询 ----
        nBake++; lastLv = Lv;
        if (Lv > maxBakeLv) maxBakeLv = Lv;
        if (st.valid) { backend.clearOverZoom((int)i); st.valid = false; }
        int tx0, ty0, tx1, ty1;
        if (!backend.bakeTileRange((int)i, Lv, scene, tx0, ty0, tx1, ty1)) continue;
        double W = L.bakeMaxx - L.bakeMinx, H = L.bakeMaxy - L.bakeMiny;
        if (W <= 0 || H <= 0) continue;
        int n = 1 << Lv;
        for (int ty = ty0; ty <= ty1; ty++)
            for (int tx = tx0; tx <= tx1; tx++) {
                if (backend.hasBakeTile((int)i, Lv, tx, ty)) continue;
                uint64_t pk = ((uint64_t)i << 48) ^ backend.tileKey(Lv, tx, ty);
                {
                    std::lock_guard<std::mutex> lk(bakeMtx_);
                    if (bakePending_.count(pk)) continue;
                }
                {
                    std::vector<unsigned char> px;
                    if (loadTileDisk(bakeCachePath(L.sourcePath, scene.displayEpsg, Lv, tx, ty), px)) {
                        backend.uploadBakeTile((int)i, Lv, tx, ty, px.data(), GLBackend::kTileRes);
                        spdlog::info("[bake] tile L{} ({},{}) 读盘命中", Lv, tx, ty);
                        continue;
                    }
                }
                std::lock_guard<std::mutex> lk(bakeMtx_);
                if (bakePending_.count(pk)) continue;
                if (bakeJobs_.size() > 64) continue;   // 队列上限, 防积压
                double x0 = L.bakeMinx + (double)tx / n * W, x1 = L.bakeMinx + (double)(tx + 1) / n * W;
                double y0 = L.bakeMiny + (double)ty / n * H, y1 = L.bakeMiny + (double)(ty + 1) / n * H;
                double sx0 = x0, sy0 = y0, sx1 = x1, sy1 = y1;
                if (crossCrs) {
                    const double cx4[4] = {x0, x1, x0, x1};
                    const double cy4[4] = {y0, y0, y1, y1};
                    double rx[4], ry[4];
                    bool ok = true;
                    for (int q = 0; q < 4; q++)
                        if (!reprojectPoint(cx4[q], cy4[q], scene.displayEpsg, srcEpsg, rx[q], ry[q])) { ok = false; break; }
                    if (ok) {
                        sx0 = *std::min_element(rx, rx + 4); sx1 = *std::max_element(rx, rx + 4);
                        sy0 = *std::min_element(ry, ry + 4); sy1 = *std::max_element(ry, ry + 4);
                    }
                }
                // 瓦片由栅格金字塔构建(后台)产出到磁盘, 这里不再按需查询源(459万要素太慢)
                (void)sx0; (void)sy0; (void)sx1; (void)sy1; (void)pk;
                (void)crossCrs; (void)srcEpsg;
            }
    }
    if (nOver > 0 && nBake == 0) ui.viewMode = "原始数据";
    else if (nBake > 0) ui.viewMode = "烘焙 L" + std::to_string(maxBakeLv >= 0 ? maxBakeLv : 0);
    else ui.viewMode.clear();
}

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
        const bool bakeMode = hasMeta && L.bake;   // 大数据才烘焙; 小数据走原矢量路径
        if (c.rebuildEpsg != 0 && L.cacheBucketInit && backend.isBlockRebuilding(gi)) {
            // 块桶层重建: 首个重建块到达 -> 用新坐标系重新流式烘焙(清旧纹理)
            backend.bakeTileBegin(gi, 0, 0, 0);   // CRS 重建: 重烘 z0
            L.bakeCached = false;                 // 重建忽略 z0 磁盘缓存
            spdlog::info("[CRS] rebuild layer[{}] re-bake EPSG {} ({} verts)",
                         gi, k, c.verts.size() + c.pts.size() + c.tris.size());
        }
        if (!L.cacheBucketInit) {
            L.cacheBucketInit = true;
            L.stagingKey = k;
            if (!bakeMode)
                backend.beginBucketLayer(gi, k != 0 ? k : (c.srcEpsg != 0 ? c.srcEpsg : 0));
            if (c.rebuildEpsg != 0)
                spdlog::info("[CRS] rebuild layer[{}] first chunk -> buckets EPSG {} ({} verts)",
                             gi, k, c.verts.size() + c.pts.size() + c.tris.size());
            if (hasMeta) {
                L.data.minx = c.minx; L.data.miny = c.miny;
                L.data.maxx = c.maxx; L.data.maxy = c.maxy;
                spdlog::info("[DBG] L{} firstchunk hasMeta srcEpsg={} disp={} rebuild={} bakeMode={} meta=({:.4f},{:.4f},{:.4f},{:.4f})",
                             gi, c.srcEpsg, scene.displayEpsg, c.rebuildEpsg, bakeMode, c.minx, c.miny, c.maxx, c.maxy);
                // 整层显示坐标范围(源==显示直接用; 否则四角重投影)
                double dminx = c.minx, dminy = c.miny, dmaxx = c.maxx, dmaxy = c.maxy;
                if (!(scene.displayEpsg == 0 || c.srcEpsg == 0 || c.srcEpsg == scene.displayEpsg)) {
                    const double cxx[4] = {c.minx, c.maxx, c.minx, c.maxx};
                    const double cyy[4] = {c.miny, c.miny, c.maxy, c.maxy};
                    double rx[4], ry[4];
                    bool ok = true;
                    for (int q = 0; q < 4; q++)
                        if (!reprojectPoint(cxx[q], cyy[q], c.srcEpsg, scene.displayEpsg, rx[q], ry[q])) { ok = false; break; }
                    if (ok) {
                        dminx = *std::min_element(rx, rx + 4); dmaxx = *std::max_element(rx, rx + 4);
                        dminy = *std::min_element(ry, ry + 4); dmaxy = *std::max_element(ry, ry + 4);
                    }
                }
                unionScene(dminx, dminy, dmaxx, dmaxy);
                if (bakeMode) {
                    int bakeLv = estimateBakeMaxLevel(L.sourcePath, L.sourceLayerIdx,
                        scene.displayEpsg != 0 ? scene.displayEpsg : c.srcEpsg, 2048, 12);
                    backend.setBakeBounds(gi, dminx, dminy, dmaxx, dmaxy, bakeLv);
                    spdlog::info("[bake] layer[{}] 自动最深层 = L{}", gi, bakeLv);
                    L.bakeMinx = dminx; L.bakeMiny = dminy; L.bakeMaxx = dmaxx; L.bakeMaxy = dmaxy;
                    L.bakeMaxLevel = bakeLv;
                    // GPU 建栅格金字塔(主线程, 逐要素 GL 光栅化最深层 + 降采样)
                    {
                        static std::mutex bkM; static std::set<std::string> bkBuilt;
                        bool need = false;
                        { std::lock_guard<std::mutex> lk(bkM); if (bkBuilt.insert(L.sourcePath).second) need = true; }
                        if (need) {
                            std::string src2 = L.sourcePath;
                            int lyr2 = L.sourceLayerIdx;
                            int dst2 = scene.displayEpsg != 0 ? scene.displayEpsg : c.srcEpsg;
                            spdlog::info("[bake] 开始 CPU 多线程建栅格金字塔 {} L{}", src2, bakeLv);
                            bgThreads_.push_back(std::thread([this, src2, lyr2, dst2, bakeLv]() {
                                static std::atomic<int> lastPct{-1};
                                peekg::render::buildRasterPyramid(src2, lyr2, dst2, bakeLv,
                                    GLBackend::kTileRes, resolvedCacheDir(cfg), [](int p) {
                                        if (p >= lastPct.load() + 10) { lastPct.store(p); spdlog::info("[bake] 金字塔 {}%", p); }
                                    });
                                spdlog::info("[bake] 金字塔构建完成 {}", src2);
                            }));
                        }
                    }
                    // z0 先查磁盘缓存: 命中直接贴图, 本轮不再重烘
                    // key 用"预期显示 CRS"(display 未定时取源 CRS, 与 done 存盘一致)
                    int zepsg = scene.displayEpsg != 0 ? scene.displayEpsg : c.srcEpsg;
                    std::vector<unsigned char> z0px;
                    if (loadTileDisk(bakeCachePath(L.sourcePath, zepsg, 0, 0, 0), z0px)) {
                        backend.uploadBakeTile(gi, 0, 0, 0, z0px.data(), GLBackend::kTileRes);
                        L.bakeCached = true;
                        spdlog::info("[bake] layer[{}] z0 读盘命中, 跳过重烘", gi);
                    } else {
                        L.bakeCached = false;
                        backend.bakeTileBegin(gi, 0, 0, 0);   // z0 全图片: 边读边烘(顺带利用本次读)
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
        if (bakeMode && !L.bakeCached) backend.bakeTileAppend(gi, dispV, dispP, dispT);
        else backend.addBucket(gi, dispV, dispP, dispT);
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
            // 烘焙 LOD: z0 片收尾(边读边烘结束) + 存盘
            if (!t.failed) {
                for (int gi = t.globalBase; gi < t.globalBase + (int)t.layerIndices.size(); gi++) {
                    if (gi < 0 || gi >= (int)scene.layers.size()) continue;
                    if (scene.layers[gi].bakeCached) continue;   // z0 来自磁盘缓存, 无需收尾/存盘
                    backend.bakeTileEnd(gi);
                    std::vector<unsigned char> px;
                    if (backend.dumpBakeTile(gi, 0, 0, 0, px))
                        saveTileDisk(bakeCachePath(scene.layers[gi].sourcePath, scene.displayEpsg, 0, 0, 0), px);
                }
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
            if (ext == ".shp") {
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
    // 测试钩子: PEEK_TEST_PAN=lon,lat 启动后把视图中心移到该点(配合 PEEK_TEST_ZOOM 复现)
    {
        static bool panApplied = false;
        if (!panApplied && scene.hasExtent && scene.view.scale > 0) {
            const char* pp = getenv("PEEK_TEST_PAN");
            double lon = 0, lat = 0;
            if (pp && sscanf(pp, "%lf,%lf", &lon, &lat) == 2) {
                scene.view.centerX = lon;
                scene.view.centerY = lat;
                panApplied = true;
                spdlog::info("[TEST] pan to ({:.6f},{:.6f}) scale={:.9f}", lon, lat, scene.view.scale);
            }
            if (!pp) panApplied = true;
        }
    }
    // 测试钩子: PEEK_TEST_ZOOM=倍数 适配后放大, 用于触发 over-zoom(被 autoFit 重置则重新应用)
    {
        static double ztScale = 0;
        const char* zf = getenv("PEEK_TEST_ZOOM");
        if (zf && scene.hasExtent && scene.view.scale > 0) {
            double f = atof(zf);
            if (f > 1 && std::fabs(scene.view.scale - ztScale) > 1e-12) {
                ztScale = scene.view.scale / f;
                scene.view.scale = ztScale;
                spdlog::info("[D] ZOOM hook: scale {} -> {} (vf {})", ztScale * f, ztScale, f);
            }
        }
    }
    updateOverZoom();      // 放大超过烘焙精度时按视口 bbox 查原始数据
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