#include "render/vt_render.h"
#include "vt/vt_build.h"
#include "vt/vt_geom.h"
#include "vt/vt_scissor.h"
#include "vt/vt_level.h"
#include "data/reproject.h"

#include <glad/glad.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace {
// 构建期覆盖区最多记录的瓦片数: 覆盖只是进度提示, 20 万片已足够看出长势
constexpr size_t kCovMaxTiles = 200000;
// 覆盖块统一透明度(所有层/所有状态同值): 后铺的压在先铺的上, 同色叠加处自然越叠越深
constexpr float kCovAlpha = 0.30f;
// 超 Lmax 直读分块参数: 每块最多要素数/软时限(ms)。软时限保证 worker 不长时间霸占; 读盘效率由 EWMA 实测。
constexpr long long kRawChunkFeat = 20000;
constexpr double kRawChunkMs = 8.0;
// 期望直读层绝对上限(防 1<<L 越界); 具体封顶由 maxLevel 决定(见 wantedRawLevel)
constexpr int kRawLevelAbsCap = 24;
}

namespace {
uint64_t tileKey(int level, int tx, int ty) {
    return ((uint64_t)(level & 0xff) << 48) |
           ((uint64_t)(tx & 0xffffff) << 24) |
           (uint64_t)(ty & 0xffffff);
}
uint64_t jobKey(int layer, int level, int tx, int ty) {
    return ((uint64_t)(layer & 0xff) << 56) | tileKey(level, tx, ty);
}

// 按瓦片净区(0..512)在屏幕上的矩形设 scissor: 扩边输出被裁掉, 相邻瓦片净区无缝拼接。
void scissorTile(const VtRenderer::GpuTile& g, const MapScene& scene, int texW, int texH) {
    peekg::vt::ScissorRect r = peekg::vt::tileScissorRect(
        g.originX, g.originY, g.cell,
        scene.view.centerX, scene.view.centerY, scene.view.scale, texW, texH, g.tileSize);
    glScissor(r.x, r.y, r.w, r.h);
}

// 覆盖块逐瓦片随机色: 由 (图层,层号,tx,ty) 散列出稳定色相(每次运行一致, 同块不变),
// 相邻块互不相同 —— 新铺了哪块一眼可见; 不能按层取色(同层全一个色, 看着像没动静)。
void tileCoverColor(int idx, int level, int tx, int ty, float& r, float& g, float& b) {
    uint32_t h = (uint32_t)idx * 0x9E3779B9u ^ (uint32_t)level * 0x85EBCA77u
               ^ (uint32_t)tx * 0xC2B2AE3Du ^ (uint32_t)ty * 0x27D4EB2Fu;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    float hue = (float)(h & 0xffffu) / 65536.0f;
    float sat = 0.55f + (float)((h >> 8) & 0x1fu) / 31.0f * 0.25f;   // 0.55..0.80
    float val = 0.72f + (float)((h >> 3) & 0x0fu) / 15.0f * 0.20f;   // 0.72..0.92 偏亮底
    float hp = hue * 6.0f;
    int seg = (int)hp;
    float f = hp - seg;
    float p = val * (1.0f - sat), q = val * (1.0f - sat * f), t = val * (1.0f - sat * (1.0f - f));
    switch (seg) {
        case 0: r = val; g = t;  b = p;  break;
        case 1: r = q;  g = val; b = p;  break;
        case 2: r = p;  g = val; b = t;  break;
        case 3: r = p;  g = q;  b = val; break;
        case 4: r = t;  g = p;  b = val; break;
        default: r = val; g = p; b = q; break;
    }
}
}  // namespace

VtRenderer::~VtRenderer() { clear(); }

void VtRenderer::bumpGen() {
    gen_++;
    {
        std::lock_guard<std::mutex> lk(jobMtx_);
        jobs_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(resMtx_);
        results_.clear();
    }
    inflight_.clear();
}

int VtRenderer::addLayer(const std::string& cachePath, int sceneLayerIdx, int srcEpsg, int dstEpsg,
                         const std::string& srcPath) {
    auto c = std::make_shared<peekg::vt::VtCache>();
    if (!c->open(cachePath)) return -1;
    Layer L;
    L.cache = std::move(c);
    L.path = cachePath;
    L.srcPath = srcPath;
    L.sceneLayerIdx = sceneLayerIdx;
    L.srcEpsg = srcEpsg;
    L.dstEpsg = dstEpsg;
    L.renderEpsg = dstEpsg;
    const auto& h = L.cache->header();
    L.maxLevel = (int)h.maxLevel;
    L.curLevel = -1;
    L.originX = h.originX;
    L.originY = h.originY;
    L.tileW0 = h.tileW0 > 0 ? h.tileW0 : 1.0;
    L.rawEnabled = rawCfgEnabled_;          // 全局开关; 具体层还要求 srcPath 非空
    L.rawBudgetMs = rawCfgBudgetMs_;
    layers_.push_back(std::move(L));
    bumpGen();
    return (int)layers_.size() - 1;
}

void VtRenderer::setRawConfig(bool enabled, double budgetMs, int enterMargin) {
    rawCfgEnabled_ = enabled;
    if (budgetMs > 0) rawCfgBudgetMs_ = budgetMs;
    rawCfgEnterMargin_ = std::max(0, enterMargin);
    for (auto& L : layers_) {
        bool en = enabled && !L.srcPath.empty();
        L.rawEnabled = en;
        if (budgetMs > 0) L.rawBudgetMs = budgetMs;
        // 配置关闭: 立刻退出直读
        if (!en && L.rawActive) {
            // 不能 close()(worker 可能正持同一 stream): reset 即可, 最后持着的线程析构时安全关闭
            L.raw.reset();
            L.rawActive = false;
            L.rawBusy = false;
            L.rawLevel = -1;
            ++L.rawGen;
            auto it = L.tiles.begin();
            while (it != L.tiles.end()) {
                if ((int)((it->first >> 48) & 0xff) > L.maxLevel) {
                    releaseTile(it->second, L.bytes);
                    it = L.tiles.erase(it);
                } else ++it;
            }
            L.curLevel = -1;
        }
    }
    bumpGen();
}

void VtRenderer::removeLayer(int idx) {
    if (idx < 0 || idx >= (int)layers_.size()) return;
    for (auto& kv : layers_[idx].tiles) releaseTile(kv.second, layers_[idx].bytes);
    releaseCoverage(layers_[idx]);
    layers_.erase(layers_.begin() + idx);
    bumpGen();
}

bool VtRenderer::holdsPath(const std::string& path) const {
    for (const auto& L : layers_)
        if (L.path == path) return true;
    return false;
}

void VtRenderer::onSceneLayerRemoved(int sceneIdx) {
    bool changed = false;
    for (int i = (int)layers_.size() - 1; i >= 0; --i) {
        if (layers_[i].sceneLayerIdx == sceneIdx) {
            for (auto& kv : layers_[i].tiles) releaseTile(kv.second, layers_[i].bytes);
            releaseCoverage(layers_[i]);
            layers_.erase(layers_.begin() + i);
            changed = true;
        } else if (layers_[i].sceneLayerIdx > sceneIdx) {
            layers_[i].sceneLayerIdx--;
            changed = true;
        }
    }
    if (changed) bumpGen();
}

void VtRenderer::clear() {
    stopWorker();
    for (auto& L : layers_) {
        for (auto& kv : L.tiles) releaseTile(kv.second, L.bytes);
        releaseCoverage(L);
    }
    layers_.clear();
    if (phVbo_) { glDeleteBuffers(1, &phVbo_); phVbo_ = 0; }
    if (phVao_) { glDeleteVertexArrays(1, &phVao_); phVao_ = 0; }
    {
        std::lock_guard<std::mutex> lk(resMtx_);
        results_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(jobMtx_);
        jobs_.clear();
    }
    inflight_.clear();
    gen_++;
}

bool VtRenderer::hasLayer(int idx) const {
    return idx >= 0 && idx < (int)layers_.size() && layers_[idx].cache != nullptr;
}

void VtRenderer::setBuilding(int idx, bool b) {
    if (idx < 0 || idx >= (int)layers_.size()) return;
    layers_[idx].building = b;
    if (!b) {
        if (layers_[idx].cache) layers_[idx].cache->reloadHeader();
        releaseCoverage(layers_[idx]);   // 构建结束: 去掉占位框/覆盖色块, 切正常缩放渲染
    }
}

void VtRenderer::releaseCoverage(Layer& L) {
    if (L.covVbo) { glDeleteBuffers(1, &L.covVbo); L.covVbo = 0; }
    if (L.covVao) { glDeleteVertexArrays(1, &L.covVao); L.covVao = 0; }
    L.cov.clear();
    L.covIndex.clear();
    L.covN = 0;
    L.covVerts = 0;
    L.covDirty = false;
    L.covRenderEpsg = 0;
    L.hasBbox = false;
}

void VtRenderer::setPlaceholderBbox(int idx, double x0, double y0, double x1, double y1) {
    if (idx < 0 || idx >= (int)layers_.size()) return;
    Layer& L = layers_[idx];
    L.hasBbox = true;
    L.bx0 = x0; L.by0 = y0; L.bx1 = x1; L.by1 = y1;
}

// 占位框(复用动态 VBO): filled=实心(2 三角), 否则描边(4 段)
void VtRenderer::drawRect(float x0, float y0, float x1, float y1, bool filled) {
    if (!phVao_) {
        glGenVertexArrays(1, &phVao_);
        glGenBuffers(1, &phVbo_);
        glBindVertexArray(phVao_);
        glBindBuffer(GL_ARRAY_BUFFER, phVbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
    }
    const std::array<float, 12> filledQ = {x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1};
    const std::array<float, 16> outlineQ = {x0, y0, x1, y0, x1, y0, x1, y1, x1, y1, x0, y1, x0, y1, x0, y0};
    const float* v = filled ? filledQ.data() : outlineQ.data();
    const int n = filled ? 6 : 8;
    glBindVertexArray(phVao_);
    glBindBuffer(GL_ARRAY_BUFFER, phVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * n * 2, v, GL_DYNAMIC_DRAW);
    glDrawArrays(filled ? GL_TRIANGLES : GL_LINES, 0, n);
    glBindVertexArray(0);
}

void VtRenderer::markBuildTile(int idx, int level, int tx, int ty) {
    if (idx < 0 || idx >= (int)layers_.size()) return;
    Layer& L = layers_[idx];
    if (!L.building) return;
    // 排除非法层号(1<<level 越界)。L0/L1 不再排除: 它们各只有 1/4 枚瓦片, 画出来
    // 只是几个大框, 不会糊住画面; 排除反而让"最粗那层建好了"完全看不见。
    if (level < 0 || level > 30) return;
    int nn = 1 << level;
    if (nn <= 0) return;
    if (tx < 0 || ty < 0 || tx >= nn || ty >= nn) return;
    // 同一瓦片会被 onCover(首次触及) 与 onTile(落盘) 各投递一次, 按瓦片键去重
    uint64_t k = ((uint64_t)level << 48) | ((uint64_t)tx << 24) | (uint64_t)ty;
    if (L.covIndex.count(k)) return;
    // 上限保护: 覆盖区只是构建进度提示, 不需要无上限增长(几十万瓦片会让每次重建
    // 缓冲 + 重投影变成明显卡顿)。超出后停止新增, 已有部分照常显示。
    if (L.covIndex.size() >= (size_t)kCovMaxTiles) return;
    L.covIndex.emplace(k, (int)L.cov.size());
    // 记录瓦片外框(缓存 CRS 的轴对齐矩形)。绘制时逐个重投影到显示 CRS, 与瓦片渲染一致。
    double tileW = L.tileW0 / (double)nn;
    float cr, cg, cb;
    tileCoverColor(idx, level, tx, ty, cr, cg, cb);
    L.cov.push_back({level, false, cr, cg, cb,
                     (float)(L.originX + tx * tileW), (float)(L.originY + ty * tileW),
                     (float)(L.originX + (tx + 1) * tileW), (float)(L.originY + (ty + 1) * tileW)});
    L.covN++;
    L.covDirty = true;
}

void VtRenderer::markMergedTile(int idx, int level, int tx, int ty) {
    if (idx < 0 || idx >= (int)layers_.size()) return;
    Layer& L = layers_[idx];
    if (!L.building) return;
    if (level < 0 || level > 30) return;
    uint64_t k = ((uint64_t)level << 48) | ((uint64_t)tx << 24) | (uint64_t)ty;
    auto it = L.covIndex.find(k);
    if (it == L.covIndex.end()) return;      // 该片没被记录(超出上限/被排除的层), 无需改色
    auto& t = L.cov[(size_t)it->second];
    if (t.merged) return;
    t.merged = true;
    // 已合并: 色相不变、整体压暗(色相无关的明度跳变, 色弱也能看到"这块状态变了")
    t.r *= 0.45f; t.g *= 0.45f; t.b *= 0.45f;
    L.covDirty = true;
}

void VtRenderer::requestTile(int idx, int level, int tx, int ty) {
    if (idx < 0 || idx >= (int)layers_.size()) return;
    Layer& L = layers_[idx];
    if (!L.cache) return;
    if (level < 0 || level > L.maxLevel) return;   // 防 1<<level UB / 越层
    int n = 1 << level;
    if (tx < 0 || tx >= n || ty < 0 || ty >= n) return;
    uint64_t key = tileKey(level, tx, ty);
    if (L.tiles.count(key)) return;
    uint64_t jk = jobKey(idx, level, tx, ty);
    if (inflight_.count(jk)) return;
    double tileW = L.tileW0 / (double)n;
    double cell = tileW / (double)peekg::vt::tileSizeAt(level, L.maxLevel);
    Job j;
    j.cache = L.cache;
    j.layer = idx;
    j.level = level;
    j.maxLevel = L.maxLevel;
    j.tx = tx; j.ty = ty;
    j.cell = cell;
    j.fromEpsg = L.dstEpsg;
    j.toEpsg = L.renderEpsg;
    j.gen = gen_;
    {
        std::lock_guard<std::mutex> lk(jobMtx_);
        jobs_.push_back(std::move(j));
    }
    inflight_.insert(jk);
    jobCv_.notify_one();
}

size_t VtRenderer::residentTiles() const {
    size_t n = 0;
    for (const auto& L : layers_) n += L.tiles.size();
    return n;
}

size_t VtRenderer::pendingJobs() const {
    std::lock_guard<std::mutex> lk(jobMtx_);
    return jobs_.size();
}

void VtRenderer::releaseTile(GpuTile& g, long long& bytes) {
    if (g.vbo) { glDeleteBuffers(1, &g.vbo); g.vbo = 0; }
    if (g.vao) { glDeleteVertexArrays(1, &g.vao); g.vao = 0; }
    g.vcount = g.pcount = g.fcount = 0;
    bytes -= g.bytes;
    if (bytes < 0) bytes = 0;
    g.bytes = 0;
}

void VtRenderer::ensureWorker() {
    if (workerStarted_) return;
    workerStarted_ = true;
    stop_.store(false);
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    if (n > 6) n = 6;
    for (unsigned i = 0; i < n; ++i)
        workers_.emplace_back(&VtRenderer::workerLoop, this);
}

void VtRenderer::stopWorker() {
    if (!workerStarted_) return;
    stop_.store(true);
    jobCv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    workerStarted_ = false;
}

void VtRenderer::workerLoop() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(jobMtx_);
            jobCv_.wait(lk, [&] { return stop_.load() || !jobs_.empty(); });
            if (jobs_.empty()) {
                if (stop_.load()) break;
                continue;
            }
            j = std::move(jobs_.front());
            jobs_.pop_front();
        }
        // 超 Lmax 直读: 分块扫描源, 产出统计(EWMA 门控用) + 本块积累的瓦片几何
        if (j.raw) {
            long long scanned = 0; double ms = 0; bool done = false;
            j.raw->chunk(j.rawMaxFeat, j.rawMaxMs, scanned, ms, done);
            {
                std::lock_guard<std::mutex> lk(resMtx_);
                Result rs;
                rs.layer = j.layer; rs.gen = j.gen;
                rs.rawStat = true; rs.rawDone = done;
                rs.rawScanned = scanned; rs.rawMs = ms;
                rs.rawGen = j.rawGen;
                results_.push_back(std::move(rs));
            }
            if (done) {
                std::vector<std::pair<uint64_t, peekg::vt::VtTile>> tiles;
                j.raw->takeTiles(tiles);
                double cellPx = (j.scale > 0) ? (j.cell / j.scale) : 0;
                // 最深层(含直读)不过滤: 亚像素小面是真实数据, 滤了就出洞
                double minFillCells = (cellPx > 0 && j.level < j.maxLevel) ? 1.0 / (cellPx * cellPx) : 0;
                for (auto& kv : tiles) {
                    const uint64_t k = kv.first;
                    int level = (int)((k >> 48) & 0xff);   // = j.level
                    int tx = (int)((k >> 24) & 0xffffff);
                    int ty = (int)(k & 0xffffff);
                    Result r;
                    r.layer = j.layer; r.level = level; r.tx = tx; r.ty = ty; r.gen = j.gen;
                    r.rawGen = j.rawGen;
                    std::vector<float> lines, points, fill;
                    peekg::vt::buildTileGeometry(kv.second, j.cell, true, lines, points, fill, minFillCells);
                    r.vcount = (long long)lines.size() / 2;
                    r.pcount = (long long)points.size() / 2;
                    r.fcount = (long long)fill.size() / 2;
                    r.data.reserve(lines.size() + points.size() + fill.size());
                    r.data.insert(r.data.end(), lines.begin(), lines.end());
                    r.data.insert(r.data.end(), points.begin(), points.end());
                    r.data.insert(r.data.end(), fill.begin(), fill.end());
                    if (j.fromEpsg > 0 && j.toEpsg > 0 && j.fromEpsg != j.toEpsg && !r.data.empty()) {
                        std::vector<float> out;
                        if (peekg::data::reprojectVertices(r.data, j.fromEpsg, j.toEpsg, out) &&
                            out.size() == r.data.size()) {
                            r.data.swap(out);
                        }
                    }
                    std::lock_guard<std::mutex> lk(resMtx_);
                    results_.push_back(std::move(r));
                }
            }
            continue;
        }
        Result r;
        r.layer = j.layer; r.level = j.level; r.tx = j.tx; r.ty = j.ty; r.gen = j.gen;
        peekg::vt::VtTile t;
        if (j.cache && j.cache->readTile(j.level, j.tx, j.ty, t)) {
            std::vector<float> lines, points, fill;
            bool stroke = true;   // 所有层都描边(每层直接从源裁, 人工裁切边在 10 格扩边里被 scissor 裁掉)
            // 亚像素小面(屏幕面积 <1px²)只描边不填充, 省掉大量 earcut; 最深层不过滤(滤了出洞)
            double cellPx = (j.scale > 0) ? (j.cell / j.scale) : 0;
            double minFillCells = (cellPx > 0 && j.level < j.maxLevel) ? 1.0 / (cellPx * cellPx) : 0;
            peekg::vt::buildTileGeometry(t, j.cell, stroke, lines, points, fill, minFillCells);
            r.vcount = (long long)lines.size() / 2;
            r.pcount = (long long)points.size() / 2;
            r.fcount = (long long)fill.size() / 2;
            r.data.reserve(lines.size() + points.size() + fill.size());
            r.data.insert(r.data.end(), lines.begin(), lines.end());
            r.data.insert(r.data.end(), points.begin(), points.end());
            r.data.insert(r.data.end(), fill.begin(), fill.end());
            if (j.fromEpsg > 0 && j.toEpsg > 0 && j.fromEpsg != j.toEpsg && !r.data.empty()) {
                std::vector<float> out;
                if (peekg::data::reprojectVertices(r.data, j.fromEpsg, j.toEpsg, out) &&
                    out.size() == r.data.size()) {
                    r.data.swap(out);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(resMtx_);
            results_.push_back(std::move(r));
        }
    }
}

// 收后台结果 → 上传 VBO(主线程 GL)
void VtRenderer::uploadResults() {
    std::deque<Result> got;
    {
        std::lock_guard<std::mutex> lk(resMtx_);
        got.swap(results_);
    }
    for (Result& r : got) {
        if (!r.rawStat) inflight_.erase(jobKey(r.layer, r.level, r.tx, r.ty));
        if (r.layer < 0 || r.layer >= (int)layers_.size()) continue;
        Layer& L = layers_[r.layer];

        // ---- 超 Lmax 直读: 统计结果(EWMA 门控 + 区域切换丢弃) ----
        if (r.rawStat) {
            L.rawBusy = false;                     // 本块在途已清(无论新旧) 
            if (r.rawGen != L.rawGen) continue;    // 旧代结果: 区域已变, 丢弃
            if (r.gen != gen_) continue;
            if (r.rawScanned > 0 && r.rawMs > 0) {
                double sample = r.rawMs / (double)r.rawScanned;   // 单要素耗时(ms)
                L.rawEma = (L.rawEma < 0) ? sample : 0.3 * sample + 0.7 * L.rawEma;
            }
            L.rawScanned += r.rawScanned;
            L.rawMs += r.rawMs;
            if (r.rawDone) {
                L.rawDone = true;
                if (L.rawMs > L.rawBudgetMs) {
                    // 整遍已扫完(数据已上传可见), 只记一次: 不判死, 保留本层直读片
                    double feas = L.rawMs > 0 ? (double)L.rawScanned / L.rawMs * 1000.0 : 0;
                    spdlog::debug("[vt] 层{} 超Lmax 直读整遍 {}ms > 预算 {}ms (实测 {:.0f} 要素/s, {} 要素), 保留本层直读结果",
                                  r.layer, L.rawMs, L.rawBudgetMs, feas, L.rawScanned);
                } else {
                    double feas = L.rawMs > 0 ? (double)L.rawScanned / L.rawMs * 1000.0 : 0;
                    spdlog::debug("[vt] 层{} 超Lmax 直读完成: {}ms <= {}ms (实测 {:.0f} 要素/s, {} 要素, 层{})",
                                  r.layer, L.rawMs, L.rawBudgetMs, feas, L.rawScanned, L.rawLevel);
                }
            } else {
                // 中途: 投影整遍时间超预算 —— 只记一次日志, 不掐断也不判死。
                // 直读是按 kRawChunkMs=8ms 分片投给 worker 的后台流, 渐进出结果, 不阻塞界面;
                // 无空间索引这类真正读不动的源已在 open 阶段(OLCFastSpatialFilter)就挡掉了,
                // 没必要在这里牺牲"能看到原始数据"换预算。跑完这遍即可, 期间 L6 垫底照常显示。
                long long remain = 0;
                if (!L.rawBudgetNoted && L.raw &&
                    (remain = L.raw->featureCount() - L.rawScanned) > 0 && L.rawEma > 0) {
                    double projected = L.rawMs + L.rawEma * (double)remain;
                    if (projected > L.rawBudgetMs) {
                        spdlog::debug(
                            "[vt] 层{} 超Lmax 直读整遍预计 {:.0f}ms > 预算 {}ms (EWMA {:.4g}ms/要素, 剩 {} 要素); 后台继续跑完, 期间以 Lmax 缓存垫底",
                            r.layer, projected, L.rawBudgetMs, L.rawEma, remain);
                        L.rawBudgetNoted = true;
                    }
                }
            }
            continue;
        }

        // ---- 普通瓦片 / 直读瓦片几何 ----
        if (r.rawGen && r.rawGen != L.rawGen) {   // 旧代直读瓦片: 丢弃
            spdlog::debug("[vt] 直读片丢弃(rawGen不匹配): L{} ({},{}) r.rawGen={} L.rawGen={}",
                          r.level, r.tx, r.ty, r.rawGen, L.rawGen);
            continue;
        }
        if (r.gen != gen_) {
            if (r.rawGen) spdlog::debug("[vt] 直读片丢弃(gen不匹配): L{} ({},{}) r.gen={} gen_={}",
                                        r.level, r.tx, r.ty, r.gen, gen_);
            continue;
        }
        if (L.building) {
            // 构建期: 各层交错产出, 接受任意层并累积显示(不再按 curLevel 过滤/清屏)
            if (L.curLevel == -1) L.curLevel = r.level;
        } else if (r.level != L.curLevel) {
            if (r.rawGen) spdlog::debug("[vt] 直读片丢弃(level!=curLevel): r.level={} curLevel={}",
                                        r.level, L.curLevel);
            continue;
        }
        uint64_t key = tileKey(r.level, r.tx, r.ty);
        if (L.tiles.count(key)) {
            if (r.rawGen) {   // 直读瓦片渐进刷新: 覆盖旧版
                releaseTile(L.tiles[key], L.bytes);
                L.tiles.erase(key);
            } else continue;
        }
        GpuTile g;
        g.lastUse = frame_;
        g.vcount = r.vcount; g.pcount = r.pcount; g.fcount = r.fcount;
        {
            int n = 1 << r.level;
            double tileW = L.tileW0 / (double)n;
            g.cell = tileW / (double)peekg::vt::tileSizeAt(r.level, L.maxLevel);
            g.tileSize = peekg::vt::tileSizeAt(r.level, L.maxLevel);
            g.originX = L.originX + r.tx * tileW;
            g.originY = L.originY + r.ty * tileW;
        }
        if (!r.data.empty()) {
            glGenVertexArrays(1, &g.vao);
            glGenBuffers(1, &g.vbo);
            glBindVertexArray(g.vao);
            glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(r.data.size() * sizeof(float)),
                         r.data.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            glBindVertexArray(0);
            g.bytes = (long long)(r.data.size() * sizeof(float));
            L.bytes += g.bytes;
        }
        L.tiles.emplace(key, g);
        if (r.rawGen)
            spdlog::debug("[vt] 直读片入库: L{} ({},{}) vcount={} pcount={} fcount={} data={}B",
                          r.level, r.tx, r.ty, r.vcount, r.pcount, r.fcount, r.data.size() * sizeof(float));
    }
}

// 构建期: 按内存预算淘汰最久未用的瓦片
void VtRenderer::evictBuildingTiles(Layer& L) {
    while (L.bytes > buildByteBudget_ && L.tiles.size() > 1) {
        auto victim = L.tiles.end();
        long long oldest = LLONG_MAX;
        for (auto it = L.tiles.begin(); it != L.tiles.end(); ++it)
            if (it->second.lastUse < oldest) { oldest = it->second.lastUse; victim = it; }
        if (victim == L.tiles.end()) break;
        releaseTile(victim->second, L.bytes);
        L.tiles.erase(victim);
    }
}

// 视口模式: 选层 + 投递缺失瓦片 + 淘汰垫底/超限瓦片
void VtRenderer::updateViewportTiles(Layer& L, size_t li, const MapScene& scene, bool& queued) {
    if (L.sceneLayerIdx >= 0 && L.sceneLayerIdx < (int)scene.layers.size() &&
        !scene.layers[L.sceneLayerIdx].info.visible)
        return;
    double scale = scene.view.scale;
    if (!(scale > 0)) return;
    double S = L.tileW0;
    if (S <= 0) return;

    int wantEpsg = scene.displayEpsg > 0 ? scene.displayEpsg : L.dstEpsg;
    if (wantEpsg != L.renderEpsg) {
        if (L.rawActive) exitRaw(L);   // 显示 CRS 变化: 直读瓦片穿新 CRS 无效, 重进
        for (auto& kv : L.tiles) releaseTile(kv.second, L.bytes);
        L.tiles.clear();
        L.renderEpsg = wantEpsg;
        inflight_.clear();
    }

    // ---- 超 Lmax 直读判定 ----
    // rawDisabled 只给"结构性不可用"用(无空间索引/源打不开/视口在数据范围外), 会话内不重试。
    // 单纯的"超预算"不设它: 有 .qix 的源区域要素数随放大迅速变小, 这次超不代表下次超。
    int Lw = wantedRawLevel(scale, L);
    // 余量 raw_enter_margin: 期望层至少到 Lmax+margin 才切直读(默认 2 = 再放大 4 倍,
    // Lmax=6 时 L8 才切), 避免刚过 Lmax 一级就切原始数据。
    bool wantRaw = L.rawEnabled && !L.rawDisabled && !L.srcPath.empty() &&
                   Lw >= L.maxLevel + rawCfgEnterMargin_;
    if (wantRaw && !L.rawActive) {
        enterRaw(L, li, scene, queued, scale);
        // enterRaw 失败(打不开/视口在范围外)会判死当前层; 预算投影超只是本次跳过
    }
    if (!wantRaw) {
        if (L.rawActive) exitRaw(L);   // 已缩回缓存层范围内: 退回 Lmax 缓存
    } else if (L.rawActive) {
        // 区域漂移: 当前遍已完成且视口移出本遍区域 -> 重开一遍新区域
        peekg::vt::TileRange rng = peekg::vt::visibleTileRange(
            scene.view.centerX, scene.view.centerY, scale, texW_, texH_,
            L.originX, L.originY, S, L.rawLevel);
        if (L.rawDone &&
            (rng.tx1 < rng.tx0 || rng.ty1 < rng.ty0 ||
             rng.tx0 < L.rawRgX0 + 1 || rng.tx1 > L.rawRgX1 - 1 ||
             rng.ty0 < L.rawRgY0 + 1 || rng.ty1 > L.rawRgY1 - 1)) {
            exitRaw(L);
            enterRaw(L, li, scene, queued, scale);
        }
        dispatchRawChunk(L, li, scene, queued);
        // 直读本遍读完且视口仍在直读区内: 已无新直读瓦片会来, 清掉其它层(缓存垫底)的瓦片。
        // draw 遍历全部 tiles(不按层过滤), 不清理就会把垫底缓存片与直读片叠加渲染。
        // 注: 不能按"视口全驻留"判定 —— 直读只产出有要素的瓦片, 空瓦片永远不会出现。
        if (L.rawDone &&
            rng.tx0 >= L.rawRgX0 && rng.tx1 <= L.rawRgX1 &&
            rng.ty0 >= L.rawRgY0 && rng.ty1 <= L.rawRgY1) {
            int removed = 0, rawTiles = 0;
            for (auto it = L.tiles.begin(); it != L.tiles.end();) {
                int lv = (int)((it->first >> 48) & 0xff);
                if (lv != L.rawLevel) { releaseTile(it->second, L.bytes); it = L.tiles.erase(it); ++removed; }
                else { ++rawTiles; ++it; }
            }
            if (removed > 0)
                spdlog::debug("[vt] 直读完成清垫底: 移除{}片, 保留直读{}片 (rawLevel={})",
                              removed, rawTiles, L.rawLevel);
        }
        return;   // 直读帧: 不走缓存路径(缓存片作垫底, 本遍读完即清)
    }
    if (L.rawActive) return;   // 刚进入直读, 本帧已按直读处理

    uint32_t built = L.cache->header().fullyBuiltLevels;
    int Ld = peekg::vt::chooseVtLevel(scale, S, L.maxLevel, built);
    if (Ld != L.curLevel) {
        // 不立刻清空: 旧层瓦片留作垫底, 避免新层没加载完就空屏(闪屏)
        L.curLevel = Ld;
        inflight_.clear();
    }
    int n = 1 << L.curLevel;
    double tileW = S / (double)n;
    double cell = tileW / (double)peekg::vt::tileSizeAt(L.curLevel, L.maxLevel);

    peekg::vt::TileRange rng = peekg::vt::visibleTileRange(
        scene.view.centerX, scene.view.centerY, scale, texW_, texH_,
        L.originX, L.originY, S, L.curLevel);
    if (rng.tx1 < rng.tx0 || rng.ty1 < rng.ty0) return;

    bool allResident = true;
    for (int ty = rng.ty0; ty <= rng.ty1; ++ty) {
        for (int tx = rng.tx0; tx <= rng.tx1; ++tx) {
            uint64_t key = tileKey(L.curLevel, tx, ty);
            auto it = L.tiles.find(key);
            if (it != L.tiles.end()) { it->second.lastUse = frame_; continue; }
            allResident = false;
            uint64_t jk = jobKey((int)li, L.curLevel, tx, ty);
            if (inflight_.count(jk)) continue;
            Job j;
            j.cache = L.cache;
            j.layer = (int)li;
            j.level = L.curLevel;
            j.maxLevel = L.maxLevel;
            j.tx = tx; j.ty = ty;
            j.cell = cell;
            j.scale = scene.view.scale;
            j.fromEpsg = L.dstEpsg;
            j.toEpsg = L.renderEpsg;
            j.gen = gen_;
            {
                std::lock_guard<std::mutex> lk(jobMtx_);
                jobs_.push_back(std::move(j));
            }
            inflight_.insert(jk);
            queued = true;
        }
    }

    // 新层全部就绪 -> 淘汰其它层(垫底)的瓦片
    if (allResident) {
        for (auto it = L.tiles.begin(); it != L.tiles.end();) {
            int lv = (int)((it->first >> 48) & 0xff);
            if (lv != L.curLevel) { releaseTile(it->second, L.bytes); it = L.tiles.erase(it); }
            else ++it;
        }
    }

    while (L.tiles.size() > L.tileLimit) {
        auto victim = L.tiles.end();
        long long oldest = LLONG_MAX;
        for (auto it = L.tiles.begin(); it != L.tiles.end(); ++it)
            if (it->second.lastUse < oldest) { oldest = it->second.lastUse; victim = it; }
        if (victim == L.tiles.end()) break;
        releaseTile(victim->second, L.bytes);
        L.tiles.erase(victim);
    }
}

int VtRenderer::wantedRawLevel(double scale, const Layer& L) const {
    // 封顶: 至少盖住触发线(Lmax+margin+1, 否则余量一大就永远进不了直读), 上限 24(防 1<<L 越界)
    int cap = std::min(kRawLevelAbsCap,
                       std::max({L.maxLevel + 4, L.maxLevel + rawCfgEnterMargin_ + 1, 14}));
    return peekg::vt::chooseVtLevelWanted(scale, L.tileW0, cap);
}

// 投递一块直读: 由 worker 分块扫描源(时间分片), 产出 rawStat(EWMA 门控) + 本块瓦片几何
void VtRenderer::dispatchRawChunk(Layer& L, size_t li, const MapScene& scene, bool& queued) {
    if (L.rawBusy || L.rawDone) return;
    if (!L.raw || L.rawLevel < 0) return;
    L.rawBusy = true;
    Job j;
    j.layer = (int)li;
    j.level = L.rawLevel;
    j.maxLevel = L.maxLevel;   // 直读层 > maxLevel, 同样属"最深", 不过滤
    j.cell = (L.tileW0 / (double)(1LL << L.rawLevel)) /
             (double)peekg::vt::tileSizeAt(L.rawLevel, L.maxLevel);
    j.scale = scene.view.scale;
    j.fromEpsg = L.dstEpsg;
    j.toEpsg = L.renderEpsg;
    j.gen = gen_;
    j.raw = L.raw;
    j.rawMaxFeat = kRawChunkFeat;
    j.rawMaxMs = kRawChunkMs;
    j.rawGen = L.rawGen;
    {
        std::lock_guard<std::mutex> lk(jobMtx_);
        jobs_.push_back(std::move(j));
    }
    // 不进 inflight_ : 分块任务无 tile 维度; 由 rawBusy 防重入
    queued = true;
    jobCv_.notify_one();
}

// 进入超 Lmax 直读: 按视口区域(rawLevel 层)开直读流并投递第一块
void VtRenderer::enterRaw(Layer& L, size_t li, const MapScene& scene, bool& queued, double scale) {
    // 预算跳过时调用点是每帧一次, 而下面要开源+空间过滤计数。节流: 缩放变化 <3% 不重试
    // (预算只取决于区域大小, 也就是缩放; 原地小幅平移重试结果一样, 纯浪费)。
    if (L.rawTryScale > 0) {
        double r = scale / L.rawTryScale;
        if (r > 0.97 && r < 1.03) return;
    }
    L.rawTryScale = scale;

    int Lw = wantedRawLevel(scale, L);
    int n = 1 << Lw;
    double tileW = L.tileW0 / (double)n;
    peekg::vt::TileRange rng = peekg::vt::visibleTileRange(
        scene.view.centerX, scene.view.centerY, scale, texW_, texH_,
        L.originX, L.originY, L.tileW0, Lw);
    if (rng.tx1 < rng.tx0 || rng.ty1 < rng.ty0) { L.rawDisabled = true; return; }

    // 区域外扩 1 瓦片: 轻微平移不重开整遍
    int rgX0 = std::max(0, rng.tx0 - 1);
    int rgY0 = std::max(0, rng.ty0 - 1);
    int rgX1 = std::min(n - 1, rng.tx1 + 1);
    int rgY1 = std::min(n - 1, rng.ty1 + 1);

    auto raw = std::make_shared<peekg::vt::RawRegionStream>();
    double rx0 = L.originX + rgX0 * tileW;
    double ry0 = L.originY + rgY0 * tileW;
    double rx1 = L.originX + (rgX1 + 1) * tileW;
    double ry1 = L.originY + (rgY1 + 1) * tileW;
    if (!raw->open(L.srcPath, 0, L.dstEpsg, Lw, L.maxLevel,
                   L.originX, L.originY, L.tileW0, rx0, ry0, rx1, ry1)) {
        spdlog::warn("[vt] 层{} 超Lmax 直读打不开源文件: {}", li, L.srcPath);
        L.rawDisabled = true;
        return;
    }

    // 预算投影: EWMA 已知且按当前效率扫完当前区域必超预算 -> 本次不进(不浪费读盘)。
    // 只"跳过这次", 不判死: 区域要素数随缩放变小, 再放大一档可能就够预算了,
    // 判死会让整场会话再也看不到原始数据(与 exitRaw 的"下次可再进"本意相悖)。
    if (L.rawEma > 0 && L.rawEma * (double)raw->featureCount() > L.rawBudgetMs) {
        double feas = L.rawEma > 0 ? 1000.0 / L.rawEma : 0;
        spdlog::debug("[vt] 层{} 超Lmax 直读本次跳过(投影超预算): {}要素 * {:.4g}ms ≈ {:.2f}s > {:.1f}ms (实测 {:.0f} 要素/s); 继续用 Lmax 缓存, 放大后可能自动进直读",
                      li, raw->featureCount(), L.rawEma,
                      L.rawEma * (double)raw->featureCount(), L.rawBudgetMs, feas);
        return;
    }

    L.raw = std::move(raw);
    L.rawLevel = Lw;
    L.rawActive = true;
    L.rawBusy = false;
    L.rawDone = false;
    L.rawScanned = 0;
    L.rawMs = 0;
    L.rawBudgetNoted = false;
    L.rawRgX0 = rgX0; L.rawRgY0 = rgY0; L.rawRgX1 = rgX1; L.rawRgY1 = rgY1;
    ++L.rawGen;                 // 区域/代际切换: 使在途旧结果失效
    L.curLevel = Lw;            // 上传接受直读层瓦片; 旧缓存片留作垫底
    spdlog::debug("[vt] 层{} 进入超Lmax 直读: Lw={} 区域({},{})-({},{}) 要素{}",
                  li, Lw, rgX0, rgY0, rgX1, rgY1, L.raw->featureCount());
    dispatchRawChunk(L, li, scene, queued);
}

// 退出直读(缩回缓存层): 清状态 + 清直读层瓦片; rawEnabled 保留(下次超 Lmax 可再进)
void VtRenderer::exitRaw(Layer& L) {
    if (!L.rawActive && !L.raw) return;
    L.raw.reset();                          // 流析构在 worker 不再引用后发生(GDAL 关闭)
    L.rawActive = false;
    L.rawBusy = false;
    L.rawLevel = -1;
    ++L.rawGen;
    L.rawScanned = 0;
    L.rawMs = 0;
    L.rawBudgetNoted = false;
    L.rawTryScale = 0;       // 下次进直读必做一次尝试(否则缩回再放回同比例会被节流挡住)
    // 清掉驻留的直读层(>maxLevel)瓦片
    for (auto it = L.tiles.begin(); it != L.tiles.end();) {
        if ((int)((it->first >> 48) & 0xff) > L.maxLevel) {
            releaseTile(it->second, L.bytes);
            it = L.tiles.erase(it);
        } else ++it;
    }
    L.curLevel = -1;
}

void VtRenderer::sync(const MapScene& scene, int texW, int texH, long long frameNo) {
    frame_ = frameNo;
    texW_ = texW; texH_ = texH;
    if (layers_.empty() || texW <= 0 || texH <= 0) return;
    ensureWorker();

    uploadResults();

    // 逐层: 构建中只按内存淘汰; 否则视口选层 + 投递缺片 + LRU
    bool queued = false;
    for (size_t li = 0; li < layers_.size(); ++li) {
        Layer& L = layers_[li];
        if (!L.cache) continue;
        if (L.building) { evictBuildingTiles(L); continue; }
        updateViewportTiles(L, li, scene, queued);
    }
    if (queued) jobCv_.notify_one();

    static const bool dbgVt = std::getenv("PEEK_DEBUG_VT") != nullptr;
    if (dbgVt) {
        static long long lastLog = 0;
        if (frame_ - lastLog >= 120) {
            lastLog = frame_;
            long long bytes = 0; int nb = 0; int cov = 0;
            for (auto& L : layers_) {
                bytes += L.bytes;
                if (L.building) { nb++; cov += (int)L.cov.size(); }
            }
            spdlog::info("[vt] frame={} layers={} building={} tiles={} cov={} bytes={}MB jobs={} drawn={} scale={:.6g} L={}",
                         frame_, layers_.size(), nb, residentTiles(), cov, bytes / 1048576,
                         pendingJobs(), drawnVerts_, scene.view.scale,
                         layers_.empty() ? -1 : layers_[0].curLevel);
        }
    }
}

void VtRenderer::drawFill(unsigned program, int locColor, int locAlpha, int locUseVColor, const MapScene& scene) {
    drawnVerts_ = 0;
    if (layers_.empty()) return;
    glUseProgram(program);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // 构建期: 已建覆盖(逐瓦片随机色 + 统一透明度, 同处叠加越叠越深) —— 不裁 scissor
    for (auto& L : layers_) {
        if (!L.building) continue;
        if (L.sceneLayerIdx < 0 || L.sceneLayerIdx >= (int)scene.layers.size()) continue;
        if (!scene.layers[L.sceneLayerIdx].info.visible) continue;
        if (L.covN > 0) {
            // 显示 CRS 变了要重建(覆盖四边形存的是缓存 CRS 坐标)
            int wantEpsg = scene.displayEpsg > 0 ? scene.displayEpsg : L.dstEpsg;
            if (L.covRenderEpsg != wantEpsg) { L.covRenderEpsg = wantEpsg; L.covDirty = true; }
            if (L.covDirty) {
                // 顶点 [x,y,r,g,b] 交错(每瓦片 6 顶点), 颜色逐瓦片随机(见 tileCoverColor)。
                // 四角逐个重投影到显示 CRS, 与瓦片顶点走同一条 reprojectVertices 路径,
                // 保证覆盖区与真实瓦片在屏幕上严格重合。
                const bool doReproj = L.dstEpsg > 0 && L.covRenderEpsg > 0 && L.dstEpsg != L.covRenderEpsg;
                std::vector<float> v;
                v.reserve(L.cov.size() * 6 * 5);
                static const int tri[6] = {0, 1, 2, 0, 2, 3};
                for (const auto& t : L.cov) {
                    double px[4] = {t.x0, t.x1, t.x1, t.x0};
                    double py[4] = {t.y0, t.y0, t.y1, t.y1};
                    if (doReproj) {
                        for (int i = 0; i < 4; ++i) {
                            double rx, ry;
                            if (peekg::data::reprojectPoint(px[i], py[i], L.dstEpsg, L.covRenderEpsg, rx, ry)) {
                                px[i] = rx; py[i] = ry;
                            }
                        }
                    }
                    for (int i : tri) {
                        v.push_back((float)px[i]); v.push_back((float)py[i]);
                        v.push_back(t.r); v.push_back(t.g); v.push_back(t.b);
                    }
                }
                if (!L.covVao) {
                    glGenVertexArrays(1, &L.covVao);
                    glGenBuffers(1, &L.covVbo);
                    glBindVertexArray(L.covVao);
                    glBindBuffer(GL_ARRAY_BUFFER, L.covVbo);
                    glEnableVertexAttribArray(0);
                    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void*)0);
                    glEnableVertexAttribArray(1);
                    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                                          (const void*)(2 * sizeof(float)));
                    glBindVertexArray(0);
                }
                glBindVertexArray(L.covVao);
                glBindBuffer(GL_ARRAY_BUFFER, L.covVbo);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(float)),
                             v.data(), GL_DYNAMIC_DRAW);
                L.covVerts = (long long)(v.size() / 5);
                L.covDirty = false;
            }
            if (L.covVerts > 0) {
                // 一批画完: 顶点自带随机色, 统一透明度 kCovAlpha(后续块压上来就越叠越深)
                glBindVertexArray(L.covVao);
                glUniform1f(locUseVColor, 1.0f);
                glUniform1f(locAlpha, kCovAlpha);
                glDrawArrays(GL_TRIANGLES, 0, (GLsizei)L.covVerts);
                glBindVertexArray(0);
                glUniform1f(locUseVColor, 0.0f);   // 恢复: 后面的常规瓦片用 uColor
            }
        }
    }
    glEnable(GL_SCISSOR_TEST);
    for (auto& L : layers_) {
        if (L.sceneLayerIdx < 0 || L.sceneLayerIdx >= (int)scene.layers.size()) continue;
        if (!scene.layers[L.sceneLayerIdx].info.visible) continue;
        const float* c = scene.layers[L.sceneLayerIdx].color;
        glUniform3f(locColor, c[0], c[1], c[2]);
        float a = c[3];
        if (a < 0) a = 0; if (a > 1) a = 1;
        glUniform1f(locAlpha, a);
        for (auto& kv : L.tiles) {
            GpuTile& g = kv.second;
            if (!g.vao || g.fcount <= 0) continue;
            scissorTile(g, scene, texW_, texH_);
            glBindVertexArray(g.vao);
            glDrawArrays(GL_TRIANGLES, (GLint)(g.vcount + g.pcount), (GLsizei)g.fcount);
            glBindVertexArray(0);
            drawnVerts_ += g.fcount;
        }
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUniform1f(locAlpha, 1.0f);
    glUniform1f(locUseVColor, 0.0f);
}

void VtRenderer::drawLines(unsigned program, int locColor, int locAlpha, int locUseVColor, const MapScene& scene) {
    if (layers_.empty()) return;
    glUseProgram(program);
    glUniform1f(locUseVColor, 0.0f);   // 线/点只用 uColor
    glEnable(GL_SCISSOR_TEST);
    for (auto& L : layers_) {
        if (L.sceneLayerIdx < 0 || L.sceneLayerIdx >= (int)scene.layers.size()) continue;
        if (!scene.layers[L.sceneLayerIdx].info.visible) continue;
        const float* c = scene.layers[L.sceneLayerIdx].color;
        glUniform3f(locColor, c[0], c[1], c[2]);
        glUniform1f(locAlpha, 1.0f);
        for (auto& kv : L.tiles) {
            GpuTile& g = kv.second;
            if (!g.vao) continue;
            if (g.vcount <= 0 && g.pcount <= 0) continue;
            scissorTile(g, scene, texW_, texH_);
            if (g.vcount > 0) {
                glBindVertexArray(g.vao);
                glDrawArrays(GL_LINES, 0, (GLsizei)g.vcount);
                glBindVertexArray(0);
            }
            if (g.pcount > 0) {
                glBindVertexArray(g.vao);
                glDrawArrays(GL_POINTS, (GLint)g.vcount, (GLsizei)g.pcount);
                glBindVertexArray(0);
            }
        }
    }
    glDisable(GL_SCISSOR_TEST);
}
