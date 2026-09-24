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
constexpr int kCovGrid = 256;   // 构建进度覆盖框的粗网格边长

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

void VtRenderer::setRawConfig(bool enabled, double budgetMs) {
    rawCfgEnabled_ = enabled;
    if (budgetMs > 0) rawCfgBudgetMs_ = budgetMs;
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
    L.covN = 0;
    L.covVerts = 0;
    L.covDirty = false;
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
    if (level < 0 || level > 30) return;   // 防 1<<level UB
    int nn = 1 << level;
    if (nn <= 0) return;
    if (L.covN == 0) { L.covN = kCovGrid; L.cov.assign((size_t)L.covN * L.covN, 0); }
    int cx = (int)(((double)tx + 0.5) / (double)nn * L.covN);
    int cy = (int)(((double)ty + 0.5) / (double)nn * L.covN);
    if (cx < 0) cx = 0; if (cx >= L.covN) cx = L.covN - 1;
    if (cy < 0) cy = 0; if (cy >= L.covN) cy = L.covN - 1;
    if (!L.cov[(size_t)cy * L.covN + cx]) { L.cov[(size_t)cy * L.covN + cx] = 1; L.covDirty = true; }
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
                double minFillCells = (cellPx > 0) ? 1.0 / (cellPx * cellPx) : 0;
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
            // 亚像素小面(屏幕面积 <1px²)只描边不填充, 省掉大量 earcut
            double cellPx = (j.scale > 0) ? (j.cell / j.scale) : 0;
            double minFillCells = (cellPx > 0) ? 1.0 / (cellPx * cellPx) : 0;
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
                    double feas = L.rawMs > 0 ? (double)L.rawScanned / L.rawMs * 1000.0 : 0;
                    spdlog::warn(
                        "[vt] 层{} 超Lmax 直读超预算: 整遍 {}ms > {}ms (实测 {:.0f} 要素/s, {} 要素) -> 回退 Lmax 缓存",
                        r.layer, L.rawMs, L.rawBudgetMs, feas, L.rawScanned);
                    disableRaw(L);
                    continue;
                }
                double feas = L.rawMs > 0 ? (double)L.rawScanned / L.rawMs * 1000.0 : 0;
                spdlog::debug("[vt] 层{} 超Lmax 直读完成: {}ms <= {}ms (实测 {:.0f} 要素/s, {} 要素, 层{})",
                              r.layer, L.rawMs, L.rawBudgetMs, feas, L.rawScanned, L.rawLevel);
            } else {
                // 中途: 投影整遍时间(已扫 + 剩余按 EWMA 估)超预算则提前掐断, 不浪费读盘
                long long remain = 0;
                if (L.raw && (remain = L.raw->featureCount() - L.rawScanned) > 0 && L.rawEma > 0) {
                    double projected = L.rawMs + L.rawEma * (double)remain;
                    if (projected > L.rawBudgetMs) {
                        spdlog::warn(
                            "[vt] 层{} 超Lmax 直读投影超预算: 预计 {}ms > {}ms (EWMA {:.4g}ms/要素, 剩 {} 要素) -> 回退",
                            r.layer, projected, L.rawBudgetMs, L.rawEma, remain);
                        disableRaw(L);
                    }
                }
            }
            continue;
        }

        // ---- 普通瓦片 / 直读瓦片几何 ----
        if (r.rawGen && r.rawGen != L.rawGen) continue;   // 旧代直读瓦片: 丢弃
        if (r.gen != gen_) continue;
        if (L.building) {
            // 构建期: 各层交错产出, 接受任意层并累积显示(不再按 curLevel 过滤/清屏)
            if (L.curLevel == -1) L.curLevel = r.level;
        } else if (r.level != L.curLevel) {
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
    // 判死即永久停用: 一次整遍(或投影)超预算说明该源经空间过滤仍读不动(如无 .qix 索引的
    // 大表), 后续缩放只会更浅层->更大区域, 只会反复卡。重试就每次进入都实测一遍 -> 卡顿。
    int Lw = wantedRawLevel(scale, L);
    bool wantRaw = L.rawEnabled && !L.rawDisabled && !L.srcPath.empty() && Lw > L.maxLevel;
    if (wantRaw && !L.rawActive) {
        enterRaw(L, li, scene, queued, scale);
        // enterRaw 失败(打不开/预算投影超)会判死当前层, 落缓存路径
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
            for (auto it = L.tiles.begin(); it != L.tiles.end();) {
                int lv = (int)((it->first >> 48) & 0xff);
                if (lv != L.rawLevel) { releaseTile(it->second, L.bytes); it = L.tiles.erase(it); }
                else ++it;
            }
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
    // 封顶: 至少比缓存最深层多 4(防早就该直读却封顶回缓存), 上限 24(防 1<<L 越界)
    int cap = std::min(kRawLevelAbsCap, std::max(L.maxLevel + 4, 14));
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

    // 预算投影: EWMA 已知且按当前效率扫完全源必超预算 -> 直接判死, 不浪费读盘
    if (L.rawEma > 0 && L.rawEma * (double)raw->featureCount() > L.rawBudgetMs) {
        double feas = L.rawEma > 0 ? 1000.0 / L.rawEma : 0;
        spdlog::warn("[vt] 层{} 超Lmax 直读投影超预算: {}要素 * {:.4g}ms ≈ {:.2f}s > {:.1f}ms (实测 {:.0f} 要素/s) -> 回退 Lmax 缓存",
                     li, raw->featureCount(), L.rawEma,
                     L.rawEma * (double)raw->featureCount(), L.rawBudgetMs, feas);
        L.rawDisabled = true;
        return;
    }

    L.raw = std::move(raw);
    L.rawLevel = Lw;
    L.rawActive = true;
    L.rawBusy = false;
    L.rawDone = false;
    L.rawScanned = 0;
    L.rawMs = 0;
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
    // 清掉驻留的直读层(>maxLevel)瓦片
    for (auto it = L.tiles.begin(); it != L.tiles.end();) {
        if ((int)((it->first >> 48) & 0xff) > L.maxLevel) {
            releaseTile(it->second, L.bytes);
            it = L.tiles.erase(it);
        } else ++it;
    }
    L.curLevel = -1;
}

// 回退 Lmax 缓存(读盘效率不达标): 退出直读 + 本层会话停用直读(不再实测重试)
void VtRenderer::disableRaw(Layer& L) {
    exitRaw(L);
    L.rawDisabled = true;
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
                if (L.building) { nb++; for (uint8_t b : L.cov) cov += b; }
            }
            spdlog::info("[vt] frame={} layers={} building={} tiles={} cov={} bytes={}MB jobs={} drawn={} scale={:.6g} L={}",
                         frame_, layers_.size(), nb, residentTiles(), cov, bytes / 1048576,
                         pendingJobs(), drawnVerts_, scene.view.scale,
                         layers_.empty() ? -1 : layers_[0].curLevel);
        }
    }
}

void VtRenderer::drawFill(unsigned program, int locColor, int locAlpha, const MapScene& scene) {
    drawnVerts_ = 0;
    if (layers_.empty()) return;
    glUseProgram(program);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // 构建期: 已建覆盖(图层色, 256×256 粗网格) —— 不裁 scissor
    for (auto& L : layers_) {
        if (!L.building) continue;
        if (L.sceneLayerIdx < 0 || L.sceneLayerIdx >= (int)scene.layers.size()) continue;
        if (!scene.layers[L.sceneLayerIdx].info.visible) continue;
        const float* c = scene.layers[L.sceneLayerIdx].color;
        if (L.covN > 0) {
            if (L.covDirty) {
                std::vector<float> v;
                double cw = L.tileW0 / L.covN;
                for (int cy = 0; cy < L.covN; ++cy)
                    for (int cx = 0; cx < L.covN; ++cx) {
                        if (!L.cov[(size_t)cy * L.covN + cx]) continue;
                        float x0 = (float)(L.originX + cx * cw), y0 = (float)(L.originY + cy * cw);
                        float x1 = (float)(L.originX + (cx + 1) * cw), y1 = (float)(L.originY + (cy + 1) * cw);
                        float q[12] = {x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1};
                        v.insert(v.end(), q, q + 12);
                    }
                if (!L.covVao) {
                    glGenVertexArrays(1, &L.covVao);
                    glGenBuffers(1, &L.covVbo);
                    glBindVertexArray(L.covVao);
                    glBindBuffer(GL_ARRAY_BUFFER, L.covVbo);
                    glEnableVertexAttribArray(0);
                    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
                    glBindVertexArray(0);
                }
                glBindVertexArray(L.covVao);
                glBindBuffer(GL_ARRAY_BUFFER, L.covVbo);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(float)),
                             v.data(), GL_DYNAMIC_DRAW);
                L.covVerts = (long long)(v.size() / 2);
                L.covDirty = false;
            }
            if (L.covVerts > 0) {
                glUniform3f(locColor, c[0], c[1], c[2]);
                glUniform1f(locAlpha, 0.5f);
                glBindVertexArray(L.covVao);
                glDrawArrays(GL_TRIANGLES, 0, (GLsizei)L.covVerts);
                glBindVertexArray(0);
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
}

void VtRenderer::drawLines(unsigned program, int locColor, int locAlpha, const MapScene& scene) {
    if (layers_.empty()) return;
    glUseProgram(program);
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
