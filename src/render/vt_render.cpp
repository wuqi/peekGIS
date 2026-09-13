#include "render/vt_render.h"
#include "vt/vt_geom.h"
#include "vt/vt_scissor.h"
#include "data/reproject.h"

#include <glad/glad.h>

#include <algorithm>
#include <climits>
#include <cmath>

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
        scene.view.centerX, scene.view.centerY, scene.view.scale, texW, texH);
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

int VtRenderer::addLayer(const std::string& cachePath, int sceneLayerIdx, int srcEpsg, int dstEpsg) {
    auto c = std::make_shared<peekg::vt::VtCache>();
    if (!c->open(cachePath)) return -1;
    Layer L;
    L.cache = std::move(c);
    L.path = cachePath;
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
    layers_.push_back(std::move(L));
    bumpGen();
    return (int)layers_.size() - 1;
}

void VtRenderer::removeLayer(int idx) {
    if (idx < 0 || idx >= (int)layers_.size()) return;
    for (auto& kv : layers_[idx].tiles) releaseTile(kv.second, layers_[idx].bytes);
    layers_.erase(layers_.begin() + idx);
    bumpGen();
}

void VtRenderer::onSceneLayerRemoved(int sceneIdx) {
    bool changed = false;
    for (int i = (int)layers_.size() - 1; i >= 0; --i) {
        if (layers_[i].sceneLayerIdx == sceneIdx) {
            for (auto& kv : layers_[i].tiles) releaseTile(kv.second, layers_[i].bytes);
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
    for (auto& L : layers_)
        for (auto& kv : L.tiles) releaseTile(kv.second, L.bytes);
    layers_.clear();
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
    if (!b && layers_[idx].cache) layers_[idx].cache->reloadHeader();
}

void VtRenderer::requestTile(int idx, int level, int tx, int ty) {
    if (idx < 0 || idx >= (int)layers_.size()) return;
    Layer& L = layers_[idx];
    if (!L.cache) return;
    int n = 1 << level;
    if (tx < 0 || tx >= n || ty < 0 || ty >= n) return;
    uint64_t key = tileKey(level, tx, ty);
    if (L.tiles.count(key)) return;
    uint64_t jk = jobKey(idx, level, tx, ty);
    if (inflight_.count(jk)) return;
    double tileW = L.tileW0 / (double)n;
    double cell = tileW / (double)peekg::vt::TILE_SIZE;
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
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(jobMtx_));
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
    worker_ = std::thread(&VtRenderer::workerLoop, this);
}

void VtRenderer::stopWorker() {
    if (!workerStarted_) return;
    stop_.store(true);
    jobCv_.notify_all();
    if (worker_.joinable()) worker_.join();
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
        Result r;
        r.layer = j.layer; r.level = j.level; r.tx = j.tx; r.ty = j.ty; r.gen = j.gen;
        peekg::vt::VtTile t;
        if (j.cache && j.cache->readTile(j.level, j.tx, j.ty, t)) {
            std::vector<float> lines, points, fill;
            peekg::vt::buildTileGeometry(t, j.cell, lines, points, fill);
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

void VtRenderer::sync(const MapScene& scene, int texW, int texH, long long frameNo) {
    frame_ = frameNo;
    texW_ = texW; texH_ = texH;
    if (layers_.empty() || texW <= 0 || texH <= 0) return;
    ensureWorker();

    // 1) 收后台结果 → 上传 VBO(主线程 GL)
    {
        std::deque<Result> got;
        {
            std::lock_guard<std::mutex> lk(resMtx_);
            got.swap(results_);
        }
        for (Result& r : got) {
            inflight_.erase(jobKey(r.layer, r.level, r.tx, r.ty));
            if (r.gen != gen_) continue;
            if (r.layer < 0 || r.layer >= (int)layers_.size()) continue;
            Layer& L = layers_[r.layer];
            if (L.building) {
                if (L.curLevel == -1) L.curLevel = r.level;
                if (r.level != L.curLevel) continue;
            } else if (r.level != L.curLevel) {
                continue;
            }
            uint64_t key = tileKey(r.level, r.tx, r.ty);
            if (L.tiles.count(key)) continue;
            GpuTile g;
            g.lastUse = frame_;
            g.vcount = r.vcount; g.pcount = r.pcount; g.fcount = r.fcount;
            {
                int n = 1 << r.level;
                double tileW = L.tileW0 / (double)n;
                g.cell = tileW / (double)peekg::vt::TILE_SIZE;
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

    // 2) 逐层: 构建中只按内存淘汰; 否则视口选层 + 投递缺片 + LRU
    bool queued = false;
    for (size_t li = 0; li < layers_.size(); ++li) {
        Layer& L = layers_[li];
        if (!L.cache) continue;

        if (L.building) {
            while (L.bytes > buildByteBudget_ && L.tiles.size() > 1) {
                auto victim = L.tiles.end();
                long long oldest = LLONG_MAX;
                for (auto it = L.tiles.begin(); it != L.tiles.end(); ++it)
                    if (it->second.lastUse < oldest) { oldest = it->second.lastUse; victim = it; }
                if (victim == L.tiles.end()) break;
                releaseTile(victim->second, L.bytes);
                L.tiles.erase(victim);
            }
            continue;
        }

        if (L.sceneLayerIdx >= 0 && L.sceneLayerIdx < (int)scene.layers.size() &&
            !scene.layers[L.sceneLayerIdx].info.visible)
            continue;
        double scale = scene.view.scale;
        if (!(scale > 0)) continue;
        double S = L.tileW0;
        if (S <= 0) continue;

        int wantEpsg = scene.displayEpsg > 0 ? scene.displayEpsg : L.dstEpsg;
        if (wantEpsg != L.renderEpsg) {
            for (auto& kv : L.tiles) releaseTile(kv.second, L.bytes);
            L.tiles.clear();
            L.renderEpsg = wantEpsg;
            inflight_.clear();
        }

        uint32_t built = L.cache->header().fullyBuiltLevels;
        int Ld = (int)std::lround(std::log2(S / (512.0 * scale)));
        if (Ld < 0) Ld = 0;
        if (Ld > L.maxLevel) Ld = L.maxLevel;
        while (Ld > 0 && !(built & (1u << Ld))) --Ld;

        if (Ld != L.curLevel) {
            for (auto& kv : L.tiles) releaseTile(kv.second, L.bytes);
            L.tiles.clear();
            L.curLevel = Ld;
            inflight_.clear();
        }
        int n = 1 << L.curLevel;
        double tileW = S / (double)n;
        double cell = tileW / (double)peekg::vt::TILE_SIZE;

        double halfW = scale * texW * 0.5, halfH = scale * texH * 0.5;
        double vx0 = scene.view.centerX - halfW, vx1 = scene.view.centerX + halfW;
        double vy0 = scene.view.centerY - halfH, vy1 = scene.view.centerY + halfH;
        int tx0 = (int)std::floor((vx0 - L.originX) / tileW);
        int tx1 = (int)std::floor((vx1 - L.originX) / tileW);
        int ty0 = (int)std::floor((vy0 - L.originY) / tileW);
        int ty1 = (int)std::floor((vy1 - L.originY) / tileW);
        tx0 = std::max(0, std::min(n - 1, tx0));
        tx1 = std::max(0, std::min(n - 1, tx1));
        ty0 = std::max(0, std::min(n - 1, ty0));
        ty1 = std::max(0, std::min(n - 1, ty1));

        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                uint64_t key = tileKey(L.curLevel, tx, ty);
                auto it = L.tiles.find(key);
                if (it != L.tiles.end()) { it->second.lastUse = frame_; continue; }
                uint64_t jk = jobKey((int)li, L.curLevel, tx, ty);
                if (inflight_.count(jk)) continue;
                Job j;
                j.cache = L.cache;
                j.layer = (int)li;
                j.level = L.curLevel;
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
                queued = true;
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
    if (queued) jobCv_.notify_one();
}

void VtRenderer::drawFill(unsigned program, int locColor, int locAlpha, const MapScene& scene) {
    drawnVerts_ = 0;
    if (layers_.empty()) return;
    glUseProgram(program);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
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
