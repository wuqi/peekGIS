#include "data/async_loader.h"
#include "data/gdal_common.h"
#include "data/geom_util.h"
#include "data/geom_cache.h"
#include "data/reproject.h"

#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>
#include <optional>

#include "util/logger.h"

namespace peekg::data {
static constexpr int kChunkVertsFloat = 200000;   // 单块回传主线程的顶点 float 上限
static constexpr int kChunkFeature = 20000;       // 顺序读一块的要素数

static void appLog(const std::string& s) { spdlog::info("{}", s); }

static void readLayerMetaFull(GDALDatasetH ds, int li, VectorData& vd) {
    OGRLayerH lyr = GDALDatasetGetLayer(ds, li);
    vd.name = OGR_L_GetName(lyr);
    // 正常 .shx 下 OGR_L_GetFeatureCount(FALSE) 是毫秒级(只读索引);
    // 缺/坏 shx 时 gdalOpenVector 已触发重建, 之后同样秒回。取不到(=-1)则走流式兜底。
    long long n = OGR_L_GetFeatureCount(lyr, FALSE);
    vd.featureCount = (n >= 0) ? (long long)n : -1;
    OGRSpatialReferenceH srs = OGR_L_GetSpatialRef(lyr);
    if (srs) {
        vd.srcEpsg = gdalSrsEpsg(srs);
        if (vd.srcEpsg) vd.sourceCrs = "EPSG:" + std::to_string(vd.srcEpsg);
        else vd.sourceCrs = "unknown";
    } else {
        vd.sourceCrs = "unknown";
    }
    spdlog::debug("[meta] layer[{}] name={} srcEpsg={} crs={} featureCount={}",
                  li, vd.name, vd.srcEpsg, vd.sourceCrs, vd.featureCount);
    vd.minx = vd.miny = 1e300;
    vd.maxx = vd.maxy = -1e300;
}

AsyncLoader::~AsyncLoader() {
    cancel();
    stopWorkers();
    gdalKeeperClear();   // 关闭本进程缓存的所有只读 dataset
}

void AsyncLoader::stopWorkers() {
    {
        std::lock_guard<std::mutex> g(mtx);
        stopping = true;
    }
    cv.notify_all();
    for (auto& w : workers) if (w.joinable()) w.join();
    workers.clear();
}

void AsyncLoader::keepAttrDataset(const std::string& path, int gen) {
    if (m_gen.load() != gen) return;
    // 加载完成后预开/缓存该文件只读 dataset, 供属性识别复用(避免首次双击等几十秒)。
    // gdalKeeperEnsure 后台异步打开, 不阻塞加载收尾, 也不影响其他文件查询。
    gdalKeeperEnsure(path);
}

void AsyncLoader::finishOk(std::shared_ptr<Task> t, const std::string& msg) {
    std::lock_guard<std::mutex> g(mtx);
    t->failed = false;
    t->resultMsg = msg;
    t->done = true;
    m_doneTasks++;
    finished.push_back(t);
}

void AsyncLoader::finishFail(std::shared_ptr<Task> t, const std::string& msg) {
    std::lock_guard<std::mutex> g(mtx);
    t->failed = true;
    t->resultMsg = msg;
    t->done = true;
    m_doneTasks++;
    finished.push_back(t);
}

// 整层几何(缓存命中/兜底补读)作为单块推给主线程(移动语义, 不复制几十 MB 几何)
void AsyncLoader::pushFullLayer(int gi, VectorData&& vd) {
    ChunkEvent c;
    c.globalIdx = gi;
    c.srcEpsg = vd.srcEpsg;
    c.name = vd.name;
    c.crs = vd.sourceCrs;
    c.full = true;
    c.minx = vd.minx; c.miny = vd.miny; c.maxx = vd.maxx; c.maxy = vd.maxy;
    c.verts = std::move(vd.vertices);
    c.pts = std::move(vd.points);
    c.tris = std::move(vd.triangles);
    std::lock_guard<std::mutex> g(mtx);
    chunks.push_back(std::move(c));
}

// 主线程: 入队一个文件。占位图层/去重由调用方(已造好 scene/backend 图层)负责,
// 本函数只建任务本身。globalBase 为占位图层的全局索引基址。
int AsyncLoader::enqueue(const std::string& path, const AppConfig& cfg,
                         const std::vector<LayerMeta>& meta,
                         const std::vector<int>& layerIndices, int globalBase) {
    std::vector<int> idxs = layerIndices;
    if (idxs.empty())
        for (size_t i = 0; i < meta.size(); i++) idxs.push_back((int)i);
    if (idxs.empty()) return -1;

    auto t = std::make_shared<Task>();
    t->gen = m_gen.load();
    t->path = path;
    t->cfg = cfg;
    t->layerIndices = idxs;
    t->globalBase = globalBase;
    for (int li : idxs) {
        t->names.push_back((li >= 0 && li < (int)meta.size()) ? meta[li].name : "");
        t->counts.push_back((li >= 0 && li < (int)meta.size()) ? meta[li].featureCount : 0);
    }
    long long total0 = 0;
    for (long long c : t->counts) if (c > 0) total0 += c;
    t->total = total0;

    {
        std::lock_guard<std::mutex> g(mtx);
        m_totalTasks++;
        m_aggTotal.fetch_add(total0);
        tasks.push_back(t);
        if (workers.empty() && !stopping) {
            int n = std::min(kMaxConcurrent, (int)std::max(1u, std::thread::hardware_concurrency()));
            if (n < 1) n = 1;
            for (int i = 0; i < n; i++)
                workers.emplace_back([this] {
                    for (;;) {
                        std::shared_ptr<Task> tk;
                        {
                            std::unique_lock<std::mutex> lk(mtx);
                            cv.wait(lk, [&] { return stopping || !tasks.empty(); });
                            if (stopping && tasks.empty()) return;
                            tk = tasks.front(); tasks.pop_front();
                        }
                        runTask(tk);
                    }
                });
        }
    }
    cv.notify_one();
    appLog("[enqueue] " + path + " layers=" + std::to_string(idxs.size()));
    return globalBase;
}

void AsyncLoader::cancel() {
    {
        std::lock_guard<std::mutex> g(mtx);
        m_gen.fetch_add(1);                     // 使所有在跑任务失效
        tasks.clear();                          // 丢队列
        finished.clear();                       // 丢弃过期结果
        chunks.clear();
        m_totalTasks = 0;
        m_doneTasks = 0;
        m_aggProcessed = 0;
        m_aggTotal = 0;
    }
    cv.notify_all();
}

LoadStats AsyncLoader::stats() const {
    LoadStats s;
    {
        std::lock_guard<std::mutex> g(mtx);
        s.doneFiles = m_doneTasks;
        s.totalFiles = m_totalTasks;
        s.processed = m_aggProcessed.load();
        s.total = m_aggTotal.load();
    }
    s.activeFiles = s.totalFiles - s.doneFiles;
    return s;
}

// 每帧取回: 完成事件 + 块事件。应用(占位图层 -> 几何/VBO/范围/重建)由调用方负责。
void AsyncLoader::poll(std::vector<LoadEvent>& done, std::vector<ChunkEvent>& outChunks) {
    {
        std::lock_guard<std::mutex> g(mtx);
        for (auto& t : finished) {
            LoadEvent e;
            e.path = t->path;
            e.msg = t->resultMsg;
            e.failed = t->failed;
            e.globalBase = t->globalBase;
            e.layerIndices = t->layerIndices;
            done.push_back(std::move(e));
        }
        finished.clear();
        outChunks.reserve(outChunks.size() + this->chunks.size());
        for (auto& c : this->chunks) outChunks.push_back(std::move(c));
        this->chunks.clear();
    }
}

// ---------------------------------------------------------------------------
// 工作线程: 一个任务 = 一个文件(内部按层/FID 并行读 GDAL, 块事件流回主线程)
// ---------------------------------------------------------------------------
void AsyncLoader::runTask(std::shared_ptr<Task> t) {
 try {
    ensureGdal();
    appLog("[open] " + t->path);

    // ---- 缓存命中: 逐块解码回传(每块=缓存写库块, 直接成桶), 整层几何不驻留内存 ----
    // readCacheLayerChunks 每解出一块立即回调 pushChunk; 块几何移动进块事件, 局部缓冲随之释放。
    auto tryHit = [&](const std::vector<int>& idxs) -> bool {
        long long proc = 0;
        for (size_t i = 0; i < idxs.size(); i++) {
            VectorData m;
            bool ok = GeomCache::readCacheLayerChunks(
                t->path, idxs[i], t->cfg, m,
                [&](std::vector<float>& v, std::vector<float>& p, std::vector<float>& tr) {
                    if (m_gen.load() != t->gen) return;   // 作废中: 结果不入队
                    if (v.empty() && p.empty() && tr.empty()) return;
                    ChunkEvent c;
                    c.globalIdx = t->globalBase + (int)i;
                    c.cacheChunk = true;
                    c.srcEpsg = m.srcEpsg;
                    c.name = m.name;
                    c.crs = m.sourceCrs;
                    c.minx = m.minx; c.miny = m.miny; c.maxx = m.maxx; c.maxy = m.maxy;
                    c.verts = std::move(v);
                    c.pts = std::move(p);
                    c.tris = std::move(tr);
                    {
                        std::lock_guard<std::mutex> g(mtx);
                        chunks.push_back(std::move(c));
                    }
                });
            if (!ok) return false;
            if (m.featureCount > 0) proc += m.featureCount;
        }
        if (proc <= 0) proc = 1;
        m_aggProcessed.fetch_add(proc);
        return true;
    };

    if (tryHit(t->layerIndices)) {
        appLog("[cache] HIT " + t->path + " layers=" + std::to_string(t->layerIndices.size()));
        keepAttrDataset(t->path, t->gen);
        finishOk(t, "缓存命中, 加载 " + std::to_string(t->layerIndices.size()) + " 图层");
        return;
    }
    appLog("[cache] MISS, reading from GDAL");

    gdalErrClear();
    GDALDatasetH ds = gdalOpenVector(t->path);
    if (!ds) {
        std::string err = gdalErrLast();
        appLog("[open] FAILED: " + t->path + (err.empty() ? "" : " err=" + err));
        finishFail(t, "打开失败: " + t->path + (err.empty() ? "" : " (" + err + ")"));
        return;
    }
    int nAll = GDALDatasetGetLayerCount(ds);
    if (nAll == 0) {
        GDALClose(ds);
        finishFail(t, "无图层: " + t->path);
        return;
    }

    int nLayer = (int)t->layerIndices.size();
    std::vector<VectorData> merged(nLayer);
    std::vector<long long> layerSegs(nLayer, 0), layerPts(nLayer, 0);   // 仅统计(几何不累积)
    std::vector<bool> layerAnyGeo(nLayer, false);
    for (int mi = 0; mi < nLayer; mi++)
        readLayerMetaFull(ds, t->layerIndices[mi], merged[mi]);
    GDALClose(ds);   // 把句柄交给下面每个 worker 自己的 dataset, 避免共享冲突

    int hw = (int)std::thread::hardware_concurrency();
    if (hw < 1) hw = 1;

    std::mutex lk;   // 保护 merged 统计(extent/count)
    std::atomic<int> remaining{0};
    bool filtered = t->layerIndices.size() != (size_t)nAll;

    // 流式写缓存: 边读边把块追加进缓存文件, 整层几何不常驻内存(见 CacheWriter)。
    // 仅全量加载写缓存(过滤加载不写, 避免部分缓存破坏完整缓存一致性)。
    std::optional<CacheWriter> cw;
    if (!filtered) {
        cw.emplace(t->path, merged, t->cfg);
        if (!cw->ok()) appLog("[cache] write open failed, skip caching");
    }

    // 块回传: 流式写缓存(存块) + 进块队列(绘图) + 只聚合范围, 不累积整层几何
    auto pushChunk = [&](int mi, std::vector<float>& local, std::vector<float>& localTris,
                         std::vector<float>& localPts) {
        if (m_gen.load() != t->gen) return;
        if (local.empty() && localPts.empty() && localTris.empty()) return;
        if (cw && cw->ok()) cw->append(mi, local, localPts, localTris);   // 边读边落盘(块级)
        ChunkEvent c;
        c.globalIdx = t->globalBase + mi;
        c.srcEpsg = merged[mi].srcEpsg;
        c.name = merged[mi].name;
        c.crs = merged[mi].sourceCrs;
        c.verts = std::move(local);
        c.pts = std::move(localPts);
        c.tris = std::move(localTris);
        {
            std::lock_guard<std::mutex> g(lk);
            VectorData& M = merged[mi];
            auto grow = [&](const std::vector<float>& src) {
                if (src.empty()) return;
                double a, b, cc, d; computeExtent(src, a, b, cc, d);
                if (a < M.minx) M.minx = a; if (b < M.miny) M.miny = b;
                if (cc > M.maxx) M.maxx = cc; if (d > M.maxy) M.maxy = d;
            };
            grow(c.verts);
            grow(c.pts);
            grow(c.tris);
            layerSegs[mi] += (long long)c.verts.size() / 2;
            layerPts[mi] += (long long)c.pts.size() / 2;
            if (!c.verts.empty() || !c.tris.empty() || !c.pts.empty()) layerAnyGeo[mi] = true;
        }
        {
            std::lock_guard<std::mutex> g(mtx);
            chunks.push_back(std::move(c));
        }
        local = {};
        localTris = {};
        localPts = {};
    };

    // 通用读块函数
    std::vector<std::thread> workers;
    auto readRange = [&](int mi, int li, long long start, long long end) {
        // 读线程必须纯只读: 不走任何"重建索引"配置。元数据打开已存在, 这里毫秒级打开;
        // 绝不允许同路径多线程并发写 .shx(会互相打架、把用户的索引文件写坏)
        GDALDatasetH d = gdalOpenVector(t->path);
        if (!d) { remaining--; return; }
        OGRLayerH lyr = GDALDatasetGetLayer(d, li);
        std::vector<float> local, localTris, localPts;
        int within = 0;
        if (end < 0) {
            OGR_L_ResetReading(lyr);
            OGRFeatureH f;
            while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
                if (m_gen.load() != t->gen) { OGR_F_Destroy(f); GDALClose(d); remaining--; return; }
                addFilledGeometry(OGR_F_GetGeometryRef(f), local, localTris, &localPts);
                OGR_F_Destroy(f);
                m_aggProcessed.fetch_add(1, std::memory_order_relaxed);
                ++within;
                if (within % kChunkFeature == 0 || (int)local.size() >= kChunkVertsFloat)
                    pushChunk(mi, local, localTris, localPts);
            }
        } else {
            for (long long fid = start; fid < end; fid++) {
                if (m_gen.load() != t->gen) { GDALClose(d); remaining--; return; }
                OGRFeatureH f = OGR_L_GetFeature(lyr, (GIntBig)fid);
                if (f) {
                    addFilledGeometry(OGR_F_GetGeometryRef(f), local, localTris, &localPts);
                    OGR_F_Destroy(f);
                }
                m_aggProcessed.fetch_add(1, std::memory_order_relaxed);
                if ((++within % kChunkFeature) == 0 || (int)local.size() >= kChunkVertsFloat)
                    pushChunk(mi, local, localTris, localPts);
            }
        }
        pushChunk(mi, local, localTris, localPts);
        {   // 流式读完成后回填真实要素数(缓存/统计用)
            std::lock_guard<std::mutex> g(lk);
            if (merged[mi].featureCount < 0) merged[mi].featureCount = within;
        }
        GDALClose(d);
        remaining--;
    };

    for (int mi = 0; mi < nLayer; mi++) {
        int li = t->layerIndices[mi];
        // 已知要素数(对话框给出)才可用按 FID 并行; 未知/损坏索引必须流式(GetNextFeature)顺序读,
        // 否则按 fid 随机访问在坏索引下会病态卡死
        long long count = (mi < (int)t->counts.size()) ? t->counts[mi] : -1;
        if (count < 0) count = merged[mi].featureCount;
        int K = (count > 20000) ? std::min(4, hw) : 1;
        if (nLayer * K > 16) K = std::max(1, 16 / nLayer);

        if (count <= 0) {
            workers.emplace_back([this, t, mi, li, &readRange, &remaining]() { readRange(mi, li, 0, -1); });
            remaining++;
            continue;
        }
        long long blockSize = (count + K - 1) / K;
        for (int k = 0; k < K; k++) {
            long long start = k * blockSize;
            long long end = std::min(count, (k + 1) * blockSize);
            if (start >= end) continue;
            workers.emplace_back([this, t, mi, li, start, end, &readRange, &remaining]() {
                readRange(mi, li, start, end);
            });
            remaining++;
        }
    }

    // 等待所有 worker(块已边读边回传, 主线程并行画)
    while (remaining.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        if (m_gen.load() != t->gen) {
            for (auto& w : workers) w.join();
            finishFail(t, "已取消");
            return;
        }
    }
    for (auto& w : workers) w.join();

    if (m_gen.load() != t->gen) { finishFail(t, "已取消"); return; }

    // 兜底: 有要素却 0 几何的层(FID 段读异常使几何落空) -> 单 dataset 顺序补读,
    // 并把补读几何以块形式回传(绘图) + 追加进流式缓存。
    {
        GDALDatasetH d = gdalOpenVector(t->path);
        if (d) {
            for (int mi = 0; mi < nLayer; mi++) {
                if (merged[mi].featureCount > 0 && !layerAnyGeo[mi]) {
                    OGRLayerH lyr = GDALDatasetGetLayer(d, t->layerIndices[mi]);
                    OGR_L_ResetReading(lyr);
                    std::vector<float> local, localTris, localPts;
                    OGRFeatureH f;
                    while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
                        addFilledGeometry(OGR_F_GetGeometryRef(f), local, localTris, &localPts);
                        OGR_F_Destroy(f);
                    }
                    if (!local.empty() || !localTris.empty() || !localPts.empty()) {
                        if (cw && cw->ok()) cw->append(mi, local, localPts, localTris);
                        ChunkEvent c;
                        c.globalIdx = t->globalBase + mi;
                        c.srcEpsg = merged[mi].srcEpsg;
                        c.name = merged[mi].name;
                        c.crs = merged[mi].sourceCrs;
                        c.verts = std::move(local);
                        c.pts = std::move(localPts);
                        c.tris = std::move(localTris);
                        {
                            std::lock_guard<std::mutex> g(lk);
                            VectorData& M = merged[mi];
                            auto grow = [&](const std::vector<float>& src) {
                                if (src.empty()) return;
                                double a, b, cc, d; computeExtent(src, a, b, cc, d);
                                if (a < M.minx) M.minx = a; if (b < M.miny) M.miny = b;
                                if (cc > M.maxx) M.maxx = cc; if (d > M.maxy) M.maxy = d;
                            };
                            grow(c.verts); grow(c.pts); grow(c.tris);
                            layerSegs[mi] += (long long)c.verts.size() / 2;
                            layerPts[mi] += (long long)c.pts.size() / 2;
                        }
                        {
                            std::lock_guard<std::mutex> g(mtx);
                            chunks.push_back(std::move(c));
                        }
                    }
                }
            }
            GDALClose(d);
        }
    }

    if (m_gen.load() != t->gen) { finishFail(t, "已取消"); return; }

    // 空图层范围复位(缓存与数据对齐)
    for (int mi = 0; mi < nLayer; mi++)
        if (merged[mi].minx > merged[mi].maxx)
            merged[mi].minx = merged[mi].miny = merged[mi].maxx = merged[mi].maxy = 0;

    // 收尾流式缓存由 CacheWriter 析构完成(回填块数/写索引/LRU 预算; 失败自动清理半成品)
    cw.reset();
    keepAttrDataset(t->path, t->gen);

    long long totV = 0, totP = 0;
    for (int mi = 0; mi < nLayer; mi++) {
        totV += layerSegs[mi];
        totP += layerPts[mi];
    }
    appLog("[read] OK " + t->path + " layers=" + std::to_string(nLayer) +
           " seg=" + std::to_string(totV) + " pts=" + std::to_string(totP));
    finishOk(t, "加载完成: " + std::to_string(nLayer) + " 图层, " +
               std::to_string(totV) + " 线段, " + std::to_string(totP) + " 点");
 } catch (const std::exception& e) {
    spdlog::error("[runTask] 异常: {}", e.what());
    finishFail(t, "加载异常: " + std::string(e.what()));
 } catch (...) {
    spdlog::error("[runTask] 未知异常");
    finishFail(t, "加载异常(未知)");
 }
}
}  // namespace peekg::data