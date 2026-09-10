#include "gl_backend.h"
#include "render/bucketize.h"
#include "map/map_scene.h"
#include "util/logger.h"
#include "data/reproject.h"
#include <glad/glad.h>
#include <vector>
#include <cmath>
#include <climits>
#include <unordered_set>
#include <chrono>

using namespace peekg::data;

static const char* VS = R"(
#version 330 core
layout(location=0) in vec2 aPos;
uniform vec2 uCenter;
uniform vec2 uInv;
void main(){
    gl_PointSize = 4.0;
    gl_Position = vec4((aPos - uCenter) * uInv, 0.0, 1.0);
})";

static const char* FS = R"(
#version 330 core
out vec4 fragColor;
uniform vec3 uColor;
uniform float uAlpha;
void main(){ fragColor = vec4(uColor, uAlpha); })";

// 栅格: 顶点 shader —— 位置(世界坐标) + 纹理坐标(0..1) 传给片段。
// 位置的世界->NDC 映射用 uCenter/uInv(与矢量一致), uv 原样透传采样纹理。
static const char* RVS = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUv;
uniform vec2 uCenter;
uniform vec2 uInv;
out vec2 UV;
void main(){
    gl_Position = vec4((aPos - uCenter) * uInv, 0.0, 1.0);
    UV = aUv;
})";

// 栅格: 片段 shader —— 直接采样纹理, 不做拉伸(拉伸已在 CPU 组装 RGBA 时完成)。
static const char* RFS = R"(
#version 330 core
out vec4 fragColor;
uniform sampler2D uImage;
in vec2 UV;
void main(){ fragColor = texture(uImage, UV); })";

static uint32_t compileShader(uint32_t type, const char* src) {
    uint32_t s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetShaderInfoLog(s, 512, nullptr, buf);
        spdlog::error("shader compile error: {}", buf);
    }
    return s;
}

uint32_t GLBackend::buildProgram() {
    uint32_t vs = compileShader(GL_VERTEX_SHADER, VS);
    uint32_t fs = compileShader(GL_FRAGMENT_SHADER, FS);
    uint32_t p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    int ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetProgramInfoLog(p, 512, nullptr, buf);
        spdlog::error("program link error: {}", buf);
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

GLuint GLBackend::buildRasterProgram() {
    uint32_t vs = compileShader(GL_VERTEX_SHADER, RVS);
    uint32_t fs = compileShader(GL_FRAGMENT_SHADER, RFS);
    uint32_t p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    int ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetProgramInfoLog(p, 512, nullptr, buf);
        spdlog::error("raster program link error: {}", buf);
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

void GLBackend::init() {
    program = buildProgram();
    glEnable(GL_PROGRAM_POINT_SIZE);   // 允许 VS 用 gl_PointSize 控制点大小(否则固定 1px)
    rProgram = buildRasterProgram();

    locCenter = glGetUniformLocation(program, "uCenter");
    locInv = glGetUniformLocation(program, "uInv");
    locColor = glGetUniformLocation(program, "uColor");
    locAlpha = glGetUniformLocation(program, "uAlpha");
    rLocCenter = glGetUniformLocation(rProgram, "uCenter");
    rLocInv = glGetUniformLocation(rProgram, "uInv");
    rLocImage = glGetUniformLocation(rProgram, "uImage");

    glGenVertexArrays(1, &gridVao);
    glGenBuffers(1, &gridVbo);
    createTestGrid();
}

void GLBackend::createTestGrid() {
    std::vector<float> verts;
    const float lo = -10.0f, hi = 10.0f, step = 1.0f;
    for (float x = lo; x <= hi + 1e-3f; x += step) {
        verts.push_back(x); verts.push_back(lo);
        verts.push_back(x); verts.push_back(hi);
    }
    for (float y = lo; y <= hi + 1e-3f; y += step) {
        verts.push_back(lo); verts.push_back(y);
        verts.push_back(hi); verts.push_back(y);
    }
    glBindVertexArray(gridVao);
    glBindBuffer(GL_ARRAY_BUFFER, gridVbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);
    gridCount = (long long)verts.size() / 2;
}

int GLBackend::addLayerPlaceholder() {
    geoms.emplace_back();
    geoms.back().seq = layerSeq_++;
    return (int)geoms.size() - 1;
}

void GLBackend::appendGeom(LayerGeom& g, const std::vector<float>& v, const std::vector<float>& pts,
                           const std::vector<float>& fill) {
    // 追加一个块到缓冲: 必要时按 2 倍扩容(orphan 后从 CPU 副本重传, 摊薄成本 <=2x)
    auto appendBuf = [](LayerGeom& g, const std::vector<float>& data,
                        GLuint& vao, GLuint& vbo, long long& count, long long& cap,
                        std::vector<float>& cpu) {
        if (data.empty()) return;
        size_t cpuByte = cpu.size() * sizeof(float);   // 已有内容(字节)
        cpu.insert(cpu.end(), data.begin(), data.end());
        long long n = (long long)data.size() / 2;       // 顶点数
        if (!vao) glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        if (!vbo) {
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        } else {
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
        }
        long long need = count + n;
        if (need > cap) {
            long long newCap = cap ? cap : (1 << 16);   // 初始 64K 顶点
            while (newCap < need) newCap *= 2;
            cap = newCap;
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cap * 2 * sizeof(float)), nullptr, GL_STATIC_DRAW);
            if (cpuByte > 0)
                glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)cpuByte, cpu.data());
        }
        glBufferSubData(GL_ARRAY_BUFFER, (GLsizeiptr)(count * 2 * sizeof(float)),
                        (GLsizeiptr)(n * 2 * sizeof(float)), data.data());
        count += n;
        glBindVertexArray(0);
    };
    appendBuf(g, v, g.vao, g.vbo, g.count, g.capV, g.cpuV);
    appendBuf(g, pts, g.pvao, g.pvbo, g.pcount, g.capP, g.cpuP);
    appendBuf(g, fill, g.fvao, g.fvbo, g.fcount, g.capF, g.cpuF);
}

void GLBackend::appendLayer(int idx, const std::vector<float>& v, const std::vector<float>& pts,
                            const std::vector<float>& fill) {
    if (idx < 0 || idx >= (int)geoms.size()) return;
    LayerGeom& g = geoms[idx];
    if (g.committed) {
        // 分块后仍有增量(加载中途切换 CRS 的罕见路径): 并回所属块, 重新上传
        mergeChunk(g, v, pts, fill);
        return;
    }
    appendGeom(g, v, pts, fill);
}

int GLBackend::addLayer(const std::vector<float>& v, const std::vector<float>& pts,
                        const std::vector<float>& fill) {
    int idx = addLayerPlaceholder();
    appendGeom(geoms[idx], v, pts, fill);
    finalizeLayer(idx);
    return idx;
}

void GLBackend::updateLayer(int idx, const std::vector<float>& v, const std::vector<float>& pts,
                            const std::vector<float>& fill) {
    if (idx < 0 || idx >= (int)geoms.size()) return;
    LayerGeom& g = geoms[idx];
    // 若已分块: 先释放所有块(整体替换/重投影)
    if (g.committed) {
        for (auto& b : g.buckets) evictBucket(b);
        g.buckets.clear();
        g.cellToBucket.clear();
        g.gridN = 0;
        g.committed = false;
    }
    // 整体替换(重投影/整层更新): 重建缓冲区
    if (g.vao) { glDeleteVertexArrays(1, &g.vao); g.vao = 0; }
    if (g.vbo) { glDeleteBuffers(1, &g.vbo); g.vbo = 0; }
    if (g.pvao) { glDeleteVertexArrays(1, &g.pvao); g.pvao = 0; }
    if (g.pvbo) { glDeleteBuffers(1, &g.pvbo); g.pvbo = 0; }
    if (g.fvao) { glDeleteVertexArrays(1, &g.fvao); g.fvao = 0; }
    if (g.fvbo) { glDeleteBuffers(1, &g.fvbo); g.fvbo = 0; }
    g.count = 0; g.capV = 0; g.pcount = 0; g.capP = 0; g.fcount = 0; g.capF = 0;
    g.cpuV.clear(); g.cpuP.clear(); g.cpuF.clear();
    appendGeom(g, v, pts, fill);
    finalizeLayer(idx);
}

void GLBackend::setStagingCpu(int idx, std::vector<float>&& v, std::vector<float>&& p,
                              std::vector<float>&& f) {
    if (idx < 0 || idx >= (int)geoms.size()) return;
    LayerGeom& g = geoms[idx];
    if (g.committed || g.rebuilding) return;
    releaseStaging(g);
    g.cpuV = std::move(v);
    g.cpuP = std::move(p);
    g.cpuF = std::move(f);
    g.count = (long long)(g.cpuV.size() / 2);
    g.pcount = (long long)(g.cpuP.size() / 2);
    g.fcount = (long long)(g.cpuF.size() / 2);
    g.capV = g.capP = g.capF = 0;
}

bool GLBackend::stealStagingCpu(int idx, VectorData& out) {
    if (idx < 0 || idx >= (int)geoms.size()) return false;
    LayerGeom& g = geoms[idx];
    if (g.committed || g.rebuilding) return false;
    if (g.cpuV.empty() && g.cpuP.empty() && g.cpuF.empty()) return false;
    out.vertices = std::move(g.cpuV);
    out.points = std::move(g.cpuP);
    out.triangles = std::move(g.cpuF);
    out.minx = g.minx; out.miny = g.miny; out.maxx = g.maxx; out.maxy = g.maxy;
    releaseStaging(g);
    return true;
}

void GLBackend::releaseStaging(LayerGeom& g) {
    if (g.vao) glDeleteVertexArrays(1, &g.vao);
    if (g.vbo) glDeleteBuffers(1, &g.vbo);
    if (g.pvao) glDeleteVertexArrays(1, &g.pvao);
    if (g.pvbo) glDeleteBuffers(1, &g.pvbo);
    if (g.fvao) glDeleteVertexArrays(1, &g.fvao);
    if (g.fvbo) glDeleteBuffers(1, &g.fvbo);
    g.vao = g.vbo = g.pvao = g.pvbo = g.fvao = g.fvbo = 0;
    g.count = g.pcount = g.fcount = 0;
    g.capV = g.capP = g.capF = 0;
    g.cpuV.clear(); g.cpuP.clear(); g.cpuF.clear();
    g.cpuV.shrink_to_fit(); g.cpuP.shrink_to_fit(); g.cpuF.shrink_to_fit();
}

// 块桶层: 缓存命中逐块读取时, 块直接成桶(与写库块一一对应, 不按空间网格切分)。
// 释放旧 staging/桶后进入 committed 块桶模式; 所有块随后 addBucket 逐块加入。
void GLBackend::beginBucketLayer(int idx, int crsEpsg) {
    if (idx < 0 || idx >= (int)geoms.size()) return;
    LayerGeom& g = geoms[idx];
    if (g.rebuilding) return;   // 后台重建中(理论不会发生, 拦截保护)
    for (auto& b : g.buckets) evictBucket(b);
    g.buckets.clear();
    g.cellToBucket.clear();
    g.gridN = 0;
    releaseStaging(g);
    g.committed = true;
    g.blockBuckets = true;
    g.bucketsEpsg = crsEpsg;
}

// 追加一块为桶(块=桶)。CPU 几何随桶驻留(state), 上传(syncVectorView)后释放;
// 桶 bbox 不计算(块桶层不做视口裁剪, bbox 无消费方)。
void GLBackend::addBucket(int idx, std::vector<float>& v, std::vector<float>& p,
                          std::vector<float>& f) {
    if (idx < 0 || idx >= (int)geoms.size()) return;
    LayerGeom& g = geoms[idx];
    if (!g.blockBuckets) return;
    if (v.empty() && p.empty() && f.empty()) return;
    VectorBucket b;
    b.v.swap(v);
    b.p.swap(p);
    b.f.swap(f);
    b.count = (long long)(b.v.size() / 2);
    b.pcount = (long long)(b.p.size() / 2);
    b.fcount = (long long)(b.f.size() / 2);
    b.minx = b.miny = b.maxx = b.maxy = 0;
    g.buckets.push_back(std::move(b));
}

bool GLBackend::isBlockBucketLayer(int idx) const {
    return idx >= 0 && idx < (int)geoms.size() && geoms[idx].blockBuckets;
}

void GLBackend::removeLayer(int idx) {
    if (idx < 0 || idx >= (int)geoms.size()) return;
    LayerGeom& g = geoms[idx];
    if (g.vao) glDeleteVertexArrays(1, &g.vao);
    if (g.vbo) glDeleteBuffers(1, &g.vbo);
    if (g.pvao) glDeleteVertexArrays(1, &g.pvao);
    if (g.pvbo) glDeleteBuffers(1, &g.pvbo);
    if (g.fvao) glDeleteVertexArrays(1, &g.fvao);
    if (g.fvbo) glDeleteBuffers(1, &g.fvbo);
    for (auto& b : g.buckets) evictBucket(b);
    geoms.erase(geoms.begin() + idx);
}

void GLBackend::clearLayers() {
    clearRasterLayers();
    for (auto& g : geoms) {
        if (g.vao) glDeleteVertexArrays(1, &g.vao);
        if (g.vbo) glDeleteBuffers(1, &g.vbo);
        if (g.pvao) glDeleteVertexArrays(1, &g.pvao);
        if (g.pvbo) glDeleteBuffers(1, &g.pvbo);
        if (g.fvao) glDeleteVertexArrays(1, &g.fvao);
        if (g.fvbo) glDeleteBuffers(1, &g.fvbo);
        for (auto& b : g.buckets) evictBucket(b);
    }
    geoms.clear();
}

// ===== 矢量分块(显示 CRS 网格, 每块独立 VBO + 视口 LRU) =====

namespace {

// 点 (x,y) 所在格下标(行列 clamp 到 [0,nc))
size_t vecCellIndex(double x, double y, double minx, double miny,
                    double cellW, double cellH, size_t nc) {
    size_t cx = (size_t)std::floor((x - minx) / cellW);
    size_t cy = (size_t)std::floor((y - miny) / cellH);
    if (cx >= nc) cx = nc - 1;
    if (cy >= nc) cy = nc - 1;
    return cy * nc + cx;
}

}  // namespace

size_t GLBackend::uploadBucket(VectorBucket& b, long long frame) {
    if (b.resident) { b.lastUse = frame; return 0; }
    size_t nv = b.v.size() + b.p.size() + b.f.size();
    if (nv == 0) return 0;
    glGenVertexArrays(1, &b.vao);
    glGenBuffers(1, &b.vbo);
    glBindVertexArray(b.vao);
    glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * sizeof(float)), nullptr, GL_STATIC_DRAW);
    size_t off = 0;
    if (!b.v.empty()) {
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(off * sizeof(float)),
                        (GLsizeiptr)(b.v.size() * sizeof(float)), b.v.data());
        off += b.v.size();
    }
    if (!b.p.empty()) {
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(off * sizeof(float)),
                        (GLsizeiptr)(b.p.size() * sizeof(float)), b.p.data());
        off += b.p.size();
    }
    if (!b.f.empty()) {
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(off * sizeof(float)),
                        (GLsizeiptr)(b.f.size() * sizeof(float)), b.f.data());
    }
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);
    b.resident = true;
    b.lastUse = frame;
    return nv * sizeof(float);
}

void GLBackend::evictBucket(VectorBucket& b) {
    if (b.vao) glDeleteVertexArrays(1, &b.vao);
    if (b.vbo) glDeleteBuffers(1, &b.vbo);
    b.vao = 0; b.vbo = 0;
    b.resident = false;
}

// 纯 CPU 分块(无 GL): 可选做 源CRS->targetEpsg 重投影(相等视为已是显示 CRS, 零拷贝引用)。
// 供 finalizeLayer 与后台重建线程共用, 也是可单测的几何核心。
bool GLBackend::buildBucketCpu(const std::vector<float>& V, const std::vector<float>& P,
                               const std::vector<float>& F, int srcEpsg, int targetEpsg,
                               BucketCpu& out) {
    out = BucketCpu{};
    out.gridN = 1;
    vbucket::CpuPartition p;
    if (!vbucket::partitionLayer(V, P, F, srcEpsg, targetEpsg, p)) return false;

    out.gridN = p.gridN;
    out.minx = p.minx; out.miny = p.miny; out.maxx = p.maxx; out.maxy = p.maxy;
    out.cellW = p.cellW; out.cellH = p.cellH;
    out.totalVerts = p.totalVerts;
    out.cellToBucket = std::move(p.cellToBucket);

    // 格 -> GL 块(vao/vbo 由 uploadBucket 时再分配)
    for (size_t bi = 0; bi < p.bucketCell.size(); bi++) {
        size_t ci = p.bucketCell[bi];
        VectorBucket b;
        b.v.swap(p.cellV[ci]);
        b.p.swap(p.cellP[ci]);
        b.f.swap(p.cellF[ci]);
        b.count = (long long)(b.v.size() / 2);
        b.pcount = (long long)(b.p.size() / 2);
        b.fcount = (long long)(b.f.size() / 2);
        b.minx = p.cellMinx[ci]; b.miny = p.cellMiny[ci];
        b.maxx = p.cellMaxx[ci]; b.maxy = p.cellMaxy[ci];
        out.buckets.push_back(std::move(b));
    }
    return true;
}

void GLBackend::finalizeLayer(int idx, int bucketsEpsg) {
    if (idx < 0 || idx >= (int)geoms.size()) return;
    LayerGeom& g = geoms[idx];
    if (g.committed || g.rebuilding) return;
    if (g.cpuV.empty() && g.cpuP.empty() && g.cpuF.empty()) return;   // 空层保持未提交

    BucketCpu r;
    buildBucketCpu(g.cpuV, g.cpuP, g.cpuF, 0, 0, r);   // 已是显示 CRS, 不重投影(零拷贝)
    g.minx = r.minx; g.miny = r.miny; g.maxx = r.maxx; g.maxy = r.maxy;
    g.gridN = r.gridN; g.cellW = r.cellW; g.cellH = r.cellH;
    g.cellToBucket = std::move(r.cellToBucket);
    g.buckets = std::move(r.buckets);

    // 释放 staging GL 缓冲与 CPU 副本(几何已归各块)
    releaseStaging(g);
    g.committed = true;
    g.bucketsEpsg = bucketsEpsg;
    spdlog::info("[vector] finalized layer[{}]: {} buckets ({} verts, grid {}x{})",
                 idx, g.buckets.size(), r.totalVerts, g.gridN, g.gridN);
}

// 显示 CRS 切换: 后台线程重投影+分块(快照源数据), 期间该层隐藏。完成后主线程 pollRebuilds 换桶。
void GLBackend::startLayerRebuild(int gi, std::shared_ptr<const VectorData> snap, int targetEpsg) {
    if (gi < 0 || gi >= (int)geoms.size() || !snap) return;
    LayerGeom& g = geoms[gi];
    if (g.rebuilding) {
        if (g.reTarget == targetEpsg) return;   // 同目标重复请求: 忽略
        // 罕见: 重建中途又切到另一个 CRS → 重启(旧结果由 token 作废)
    }
    uint64_t seq = g.seq;
    uint64_t tok = rebuildTok_++;
    g.reTok = tok;
    g.reTarget = targetEpsg;
    g.rebuilding = true;
    g.committed = false;   // 隐藏, 换桶完成前不渲染
    g.bucketsEpsg = 0;
    for (auto& b : g.buckets) evictBucket(b);
    g.buckets.clear(); g.cellToBucket.clear(); g.gridN = 0;
    releaseStaging(g);

    std::thread([this, snap, tok, seq, gi, targetEpsg]() {
        std::unique_ptr<BucketCpu> res = std::make_unique<BucketCpu>();
        auto tB0 = std::chrono::steady_clock::now();
        buildBucketCpu(snap->vertices, snap->points, snap->triangles, snap->srcEpsg, targetEpsg, *res);
        auto tB1 = std::chrono::steady_clock::now();
        if (getenv("PEEK_TIMING"))
            spdlog::info("[timing] partitionLayer layer[{}] {} verts grid={}x{} in {:.0f}ms",
                         gi, res->totalVerts, res->gridN, res->gridN,
                         std::chrono::duration<double, std::milli>(tB1 - tB0).count());
        Rebuilt rb;
        rb.gi = gi; rb.seq = seq; rb.tok = tok;
        // target==0 表示不重投影(identity, 桶保持源坐标); 记下桶实际所在 CRS 供切换判据
        rb.targetEpsg = (targetEpsg == 0) ? snap->srcEpsg : targetEpsg;
        rb.res = std::move(res);
        std::lock_guard<std::mutex> lk(rm_);
        rebuilt_.push_back(std::move(rb));
    }).detach();
}

// 主线程每帧: 收取已完成的后台重建, 上传各块 GL 缓冲并换桶
void GLBackend::pollRebuilds() {
    std::vector<Rebuilt> ready;
    {
        std::lock_guard<std::mutex> lk(rm_);
        if (!rebuilt_.empty()) ready.swap(rebuilt_);
    }
    for (auto& rb : ready) {
        if (rb.gi < 0 || rb.gi >= (int)geoms.size()) continue;
        LayerGeom& g = geoms[rb.gi];
        if (g.seq != rb.seq || g.reTok != rb.tok || !g.rebuilding) continue;  // 已移除/重启
        long long tv = rb.res ? rb.res->totalVerts : 0;
        g.minx = rb.res->minx; g.miny = rb.res->miny;
        g.maxx = rb.res->maxx; g.maxy = rb.res->maxy;
        g.gridN = rb.res->gridN; g.cellW = rb.res->cellW; g.cellH = rb.res->cellH;
        g.cellToBucket = std::move(rb.res->cellToBucket);
        g.buckets = std::move(rb.res->buckets);
        g.committed = true;
        g.rebuilding = false;
        g.reTarget = 0;
        g.bucketsEpsg = rb.targetEpsg;
        // 不在此一次性上传全部块(几十 MB GPU 突发会卡一帧): 各块保持 resident=false,
        // 由每帧 syncVectorView 按视口逐帧上传(渐进出现)。
        spdlog::info("[CRS] rebuilt layer[{}]: {} buckets ({} verts, grid {}x{})",
                     rb.gi, g.buckets.size(), tv, g.gridN, g.gridN);
    }
}

void GLBackend::mergeChunk(LayerGeom& g, const std::vector<float>& v,
                           const std::vector<float>& pts, const std::vector<float>& fill) {
    if (!g.committed || g.gridN == 0 || g.cellToBucket.empty()) return;
    size_t nc = g.gridN;
    double mnx = g.minx, mny = g.miny;
    std::unordered_set<size_t> touched;

    auto growBucket = [&](VectorBucket& b, const std::vector<float>& src, size_t start, size_t len) {
        for (size_t k = 0; k + 1 < len; k += 2) {
            double x = src[start + k], y = src[start + k + 1];
            b.minx = std::min(b.minx, x); b.miny = std::min(b.miny, y);
            b.maxx = std::max(b.maxx, x); b.maxy = std::max(b.maxy, y);
        }
    };
    auto appendToCell = [&](size_t ci, const std::vector<float>& src, size_t start, size_t len,
                            int stream, long long& vcount) {
        int bi = g.cellToBucket[ci];
        if (bi < 0) {
            // 该格在部分提交时为空(无块): 实时建块, 保证任何几何都不被丢弃
            VectorBucket nb;
            nb.minx = nb.miny = 1e300;
            nb.maxx = nb.maxy = -1e300;
            nb.lastUse = 0;
            bi = (int)g.buckets.size();
            g.cellToBucket[ci] = bi;
            g.buckets.push_back(std::move(nb));
        }
        VectorBucket& b = g.buckets[(size_t)bi];
        std::vector<float>& dst = stream == 0 ? b.v : (stream == 1 ? b.p : b.f);
        dst.insert(dst.end(), src.begin() + start, src.begin() + start + len);
        vcount += (long long)len / 2;
        growBucket(b, src, start, len);
        touched.insert((size_t)bi);
    };

    for (size_t i = 0; i + 3 < v.size(); i += 4) {
        float mx = (v[i] + v[i + 2]) * 0.5f, my = (v[i + 1] + v[i + 3]) * 0.5f;
        size_t ci = vecCellIndex(mx, my, mnx, mny, g.cellW, g.cellH, nc);
        long long dc = 0;
        appendToCell(ci, v, i, 4, 0, dc);
        g.buckets[(size_t)g.cellToBucket[ci]].count += dc;
    }
    for (size_t i = 0; i + 1 < pts.size(); i += 2) {
        size_t ci = vecCellIndex(pts[i], pts[i + 1], mnx, mny, g.cellW, g.cellH, nc);
        long long dc = 0;
        appendToCell(ci, pts, i, 2, 1, dc);
        g.buckets[(size_t)g.cellToBucket[ci]].pcount += dc;
    }
    for (size_t i = 0; i + 5 < fill.size(); i += 6) {
        float mx = (fill[i] + fill[i + 2] + fill[i + 4]) / 3.0f;
        float my = (fill[i + 1] + fill[i + 3] + fill[i + 5]) / 3.0f;
        size_t ci = vecCellIndex(mx, my, mnx, mny, g.cellW, g.cellH, nc);
        long long dc = 0;
        appendToCell(ci, fill, i, 6, 2, dc);
        g.buckets[(size_t)g.cellToBucket[ci]].fcount += dc;
    }

    for (size_t bi : touched)
        evictBucket(g.buckets[bi]);   // 释放旧 GPU 缓冲, 下一帧按需重传
}

void GLBackend::syncVectorView(const MapScene& scene) {
    if (geoms.empty() || texW <= 0 || texH <= 0) return;
    // 每帧上传预算(字节): 限制单帧 GPU 突发, 让大图层块逐帧渐进出现而不冻结 UI。
    // 默认 32MB/帧(约 8M 顶点), 缩放/平移时视图稳定后仍有明显可见的渐进填充感。
    static const size_t kUploadBudget = (size_t)32 << 20;
    size_t budgetUsed = 0;
    double halfW = scene.view.scale * texW * 0.5;
    double halfH = scene.view.scale * texH * 0.5;
    for (size_t i = 0; i < geoms.size(); i++) {
        LayerGeom& g = geoms[i];
        if (!g.committed || g.buckets.empty()) continue;
        if (i >= scene.layers.size() || !scene.layers[i].info.visible) continue;
        // 视口矩形 + 半个格余量(避免小幅平移反复淘汰)
        double vx0 = scene.view.centerX - halfW - g.cellW * 0.5;
        double vx1 = scene.view.centerX + halfW + g.cellW * 0.5;
        double vy0 = scene.view.centerY - halfH - g.cellH * 0.5;
        double vy1 = scene.view.centerY + halfH + g.cellH * 0.5;

        size_t resident = 0;
        size_t wantedCount = 0;
        if (g.wantedScratch.size() < g.buckets.size())
            g.wantedScratch.assign(g.buckets.size(), 0);
        std::vector<uint8_t>& wanted = g.wantedScratch;
        for (size_t bi = 0; bi < g.buckets.size(); bi++) {
            const VectorBucket& b = g.buckets[bi];
            // 块桶层: 全部块想要(整体画出, 不做视口裁剪), 由每帧预算逐块渐进上传
            bool want = g.blockBuckets ? true
                                       : !(b.maxx < vx0 || b.minx > vx1 || b.maxy < vy0 || b.miny > vy1);
            wanted[bi] = want ? 1 : 0;
            if (want) wantedCount++;
            if (b.resident) resident++;
        }
        // 有效上限 = 视口刚好需要多少, 就至少能驻留多少(全图视图必须完整, 缺块会露空);
        // 只有缩放到局部时上限才回落到 bucketCapacity, 由 LRU 淘汰压缩显存。
        size_t cap = std::max(g.bucketCapacity, wantedCount);

        static const bool dbgVec = getenv("PEEK_DEBUG_VEC") != nullptr;
        if (dbgVec) {
            static long long dbgLast = 0;
            if (frameNo - dbgLast > 30) {                dbgLast = frameNo;
                spdlog::info("[VEC] gi={} committed={} buckets={} resident={} wanted={} cap={} "
                             "view=(cx={:.3f},cy={:.3f},sc={:.6f}) vp=({:.0f}x{:.0f}) "
                             "ext=({:.4f},{:.4f},{:.4f},{:.4f}) grid={}x{}",
                             i, (int)g.committed, g.buckets.size(), resident, wantedCount, cap,
                             scene.view.centerX, scene.view.centerY, scene.view.scale,
                             halfW * 2, halfH * 2, g.minx, g.miny, g.maxx, g.maxy, g.gridN, g.gridN);
            }
        }

        // 上传缺失的想看块(受每帧字节预算限制, 渐进上传; 上限满则暂停下帧续传)
        for (size_t bi = 0; bi < g.buckets.size(); bi++) {
            if (!wanted[bi] || g.buckets[bi].resident) continue;
            if (resident >= cap) break;
            if (budgetUsed >= kUploadBudget) break;   // 本帧预算用尽, 其余阻塞续传
            budgetUsed += uploadBucket(g.buckets[bi], frameNo);
            if (g.blockBuckets) {
                // 块桶层上传后立即释放 CPU 副本(整层几何在工作树内存中的唯一驻留点),
                // 释放后 resident 桶不可回传(该层永久全量驻留 GPU, 不做 LRU/重建兜底)。
                VectorBucket& ub = g.buckets[bi];
                ub.v.clear(); ub.p.clear(); ub.f.clear();
                ub.v.shrink_to_fit(); ub.p.shrink_to_fit(); ub.f.shrink_to_fit();
            }
            resident++;
        }

        // 超出上限淘汰: 先不在视口内的最久未用, 仍超再允许视口内最久未用
        while (resident > cap) {
            VectorBucket* victim = nullptr;
            long long oldest = LLONG_MAX;
            for (auto& b : g.buckets) {
                if (!b.resident) continue;
                bool inView = !(b.maxx < vx0 || b.minx > vx1 || b.maxy < vy0 || b.miny > vy1);
                if (inView) continue;
                if (b.lastUse < oldest) { oldest = b.lastUse; victim = &b; }
            }
            if (!victim) {
                for (auto& b : g.buckets) {
                    if (!b.resident) continue;
                    if (b.lastUse < oldest) { oldest = b.lastUse; victim = &b; }
                }
            }
            if (!victim) break;
            evictBucket(*victim);
            resident--;
        }
    }
}

// ===== 栅格(底图 + 细节块 LRU) =====

// 填充 6 顶点四边形的位置槽(x,y 每顶点前 2 分量), uv 槽不变。
static void fillQuad(std::vector<float>& v, double x0, double y0, double x1, double y1) {
    // 顶序与 addRasterLayer 中 uv 槽一致: 左上 右上 右下 | 左上 右下 左下
    float px[6][2] = {
        {(float)x0, (float)y1}, {(float)x1, (float)y1}, {(float)x1, (float)y0},
        {(float)x0, (float)y1}, {(float)x1, (float)y0}, {(float)x0, (float)y0},
    };
    for (int i = 0; i < 6; i++) {
        v[i * 4 + 0] = px[i][0];
        v[i * 4 + 1] = px[i][1];
    }
}

int GLBackend::addRasterLayer(const RasterData& rd) {
    RasterLayer r;
    // 底图纹理
    glGenTextures(1, &r.baseTex);
    glBindTexture(GL_TEXTURE_2D, r.baseTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 模板四边形 VBO(6 顶点 × (x,y,u,v)); 位置每帧按世界矩形重写, uv 固定(0/1)
    glGenVertexArrays(1, &r.tplVao);
    glGenBuffers(1, &r.tplVbo);
    r.tplVerts.resize(6 * 4);
    // uv 槽固定: 左上(0,1) 右上(1,1) 右下(1,0) | 左上 右下 左下(0,0)
    float uv[24] = {
        0,1, 1,1, 1,0,
        0,1, 1,0, 0,0,
    };
    for (int i = 0; i < 6; i++) {
        r.tplVerts[i * 4 + 2] = uv[i * 2];
        r.tplVerts[i * 4 + 3] = uv[i * 2 + 1];
    }
    glBindVertexArray(r.tplVao);
    glBindBuffer(GL_ARRAY_BUFFER, r.tplVbo);
    glBufferData(GL_ARRAY_BUFFER, r.tplVerts.size() * sizeof(float),
                 r.tplVerts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    rasters.push_back(std::move(r));
    return (int)rasters.size() - 1;
}

void GLBackend::setRasterBase(int idx, const std::vector<unsigned char>& rgba, int w, int h) {
    if (idx < 0 || idx >= (int)rasters.size() || rgba.empty() || w <= 0 || h <= 0) return;
    RasterLayer& r = rasters[idx];
    glBindTexture(GL_TEXTURE_2D, r.baseTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    r.baseW = w; r.baseH = h; r.hasBase = true;
}

// 块 key: 编码 (span, tx, ty) 为唯一 long long(span 步长直接区分 LOD 等级)
static long long tileKey(int span, int tx, int ty) {
    // span <= 32 按 2^span? span 是实际源像素步长, 上移 40bit; tx/ty 各 20bit
    return ((long long)span << 40) | ((long long)tx << 20) | ((long long)ty & 0xFFFFF);
}

// 上传一块细节: 块覆盖源像素窗口 [tx*span, ty*span]-[+sw,+sh], 纹理为重采样后的 w×h
void GLBackend::setRasterTile(int idx, int span, int tx, int ty, int sw, int sh,
                              const std::vector<unsigned char>& rgba, int w, int h) {
    if (idx < 0 || idx >= (int)rasters.size() || rgba.empty() || w <= 0 || h <= 0) return;
    RasterLayer& r = rasters[idx];
    long long key = tileKey(span, tx, ty);

    // LRU 淘汰: 超出上限删最久未用(队列头)
    auto it = r.tiles.find(key);
    if (!r.tiles.count(key) && r.tiles.size() >= r.tileLimit) {
        if (!r.tileQueue.empty()) {
            std::vector<int> old = r.tileQueue.front();
            r.tileQueue.erase(r.tileQueue.begin());
            long long okey = tileKey(old[0], old[1], old[2]);
            auto oit = r.tiles.find(okey);
            if (oit != r.tiles.end()) {
                if (oit->second.tex) glDeleteTextures(1, &oit->second.tex);
                if (oit->second.vao) glDeleteVertexArrays(1, &oit->second.vao);
                if (oit->second.vbo) glDeleteBuffers(1, &oit->second.vbo);
                r.tiles.erase(oit);
            }
        }
    }

    // 新建或更新块
    if (!r.tiles.count(key)) {
        RasterTile t;
        glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenVertexArrays(1, &t.vao);
        glGenBuffers(1, &t.vbo);
        glBindVertexArray(t.vao);
        glBindBuffer(GL_ARRAY_BUFFER, t.vbo);
        std::vector<float> v(24, 0.0f);   // 位置每帧填, 先占位
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindVertexArray(0);
        t.span = span; t.tx = tx; t.ty = ty;
        t.sw = sw; t.sh = sh;
        t.w = w; t.h = h;
        r.tiles[key] = t;
    }
    RasterTile& t = r.tiles[key];
    t.w = w; t.h = h;
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // 更新 LRU 队列: 移到队尾
    for (size_t i = 0; i < r.tileQueue.size(); i++) {
        if (r.tileQueue[i][0] == span && r.tileQueue[i][1] == tx && r.tileQueue[i][2] == ty) {
            r.tileQueue.erase(r.tileQueue.begin() + i);
            break;
        }
    }
    r.tileQueue.push_back(std::vector<int>{span, tx, ty});
}

bool GLBackend::hasRasterTile(int idx, int span, int tx, int ty) const {
    if (idx < 0 || idx >= (int)rasters.size()) return false;
    return rasters[idx].tiles.count(tileKey(span, tx, ty)) > 0;
}

void GLBackend::clearRasterTiles(int idx) {
    if (idx < 0 || idx >= (int)rasters.size()) return;
    RasterLayer& r = rasters[idx];
    for (auto& kv : r.tiles) {
        if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
        if (kv.second.vao) glDeleteVertexArrays(1, &kv.second.vao);
        if (kv.second.vbo) glDeleteBuffers(1, &kv.second.vbo);
    }
    r.tiles.clear();
    r.tileQueue.clear();
}

void GLBackend::removeRasterLayer(int idx) {
    if (idx < 0 || idx >= (int)rasters.size()) return;
    RasterLayer& r = rasters[idx];
    if (r.baseTex) glDeleteTextures(1, &r.baseTex);
    if (r.tplVao) glDeleteVertexArrays(1, &r.tplVao);
    if (r.tplVbo) glDeleteBuffers(1, &r.tplVbo);
    for (auto& kv : r.tiles) {
        if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
        if (kv.second.vao) glDeleteVertexArrays(1, &kv.second.vao);
        if (kv.second.vbo) glDeleteBuffers(1, &kv.second.vbo);
    }
    rasters.erase(rasters.begin() + idx);
}

void GLBackend::clearRasterLayers() {
    for (auto& r : rasters) {
        if (r.baseTex) glDeleteTextures(1, &r.baseTex);
        if (r.tplVao) glDeleteVertexArrays(1, &r.tplVao);
        if (r.tplVbo) glDeleteBuffers(1, &r.tplVbo);
        for (auto& kv : r.tiles) {
            if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
            if (kv.second.vao) glDeleteVertexArrays(1, &kv.second.vao);
            if (kv.second.vbo) glDeleteBuffers(1, &kv.second.vbo);
        }
    }
    rasters.clear();
}

void GLBackend::ensureFbo(int w, int h) {
    if (w == texW && h == texH && tex) return;
    texW = w; texH = h;
    if (tex) { glDeleteTextures(1, &tex); glDeleteFramebuffers(1, &fbo); }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLBackend::resize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    ensureFbo(w, h);
}

void GLBackend::render(const MapScene& scene) {
    if (!tex) return;
    frameNo++;
    auto tRenderStart = std::chrono::steady_clock::now();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, texW, texH);
    glClearColor(0.12f, 0.13f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // 矢量分块: 按当前视口同步驻留块(上传缺失 + LRU 淘汰)
    syncVectorView(scene);

    double invx = 2.0 / (scene.view.scale * texW);
    double invy = 2.0 / (scene.view.scale * texH);

    // ===== 栅格 pass: 先铺底图, 再叠加细节块(都在矢量之下) =====
    if (!rasters.empty()) {
        glUseProgram(rProgram);
        glUniform2f(rLocCenter, (float)scene.view.centerX, (float)scene.view.centerY);
        glUniform2f(rLocInv, (float)invx, (float)invy);
        for (size_t i = 0; i < scene.layers.size(); i++) {
            const MapLayer& l = scene.layers[i];
            if (l.kind != LayerKind::Raster) continue;
            if (!l.info.visible) continue;
            if (l.rasterHandle < 0 || l.rasterHandle >= (int)rasters.size()) continue;
            RasterLayer& r = rasters[l.rasterHandle];
            // 底图: 覆盖整栅格世界范围
            if (r.hasBase) {
                // 优先用显示CRS四角(已重投影), 回退源CRS范围
                double bx0 = l.raster.hasDispExtent ? l.raster.dispMinx : l.raster.minx;
                double by0 = l.raster.hasDispExtent ? l.raster.dispMiny : l.raster.miny;
                double bx1 = l.raster.hasDispExtent ? l.raster.dispMaxx : l.raster.maxx;
                double by1 = l.raster.hasDispExtent ? l.raster.dispMaxy : l.raster.maxy;
                fillQuad(r.tplVerts, bx0, by0, bx1, by1);
                // 底图垂直翻转: OpenGL 纹理原点左下 + GDAL 数据 top-down, UV v 反向(临时副本不污染模板)
                std::vector<float> baseVerts = r.tplVerts;
                for (int k = 0; k < 6; k++) baseVerts[k*4+3] = 1.0f - baseVerts[k*4+3];
                glBindVertexArray(r.tplVao);
                glBindBuffer(GL_ARRAY_BUFFER, r.tplVbo);
                glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * baseVerts.size(), baseVerts.data());
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, r.baseTex);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                glBindVertexArray(0);
            }
            // 细节块: 块覆盖源像素 [tx*span, ty*span]-[+sw,+sh], 纹理为重采样结果。
            // 用层 geo 把像素块角映射到世界。倒序遍历 key: span 越大越粗,先画粗再画细(细覆盖粗)。
            for (auto it = r.tiles.rbegin(); it != r.tiles.rend(); ++it) {
                RasterTile& t = it->second;
                double g0 = l.raster.geo[0], g3 = l.raster.geo[3];
                double g1 = l.raster.geo[1], g2 = l.raster.geo[2];
                double g4 = l.raster.geo[4], g5 = l.raster.geo[5];
                // 有效源像素区(边缘块 sw/sh 小于 span)
                double px0 = (double)t.tx * (double)t.span, py0 = (double)t.ty * (double)t.span;
                double px1 = px0 + (double)(t.sw <= 0 ? t.w : t.sw);
                double py1 = py0 + (double)(t.sh <= 0 ? t.h : t.sh);
                if (px0 >= (double)l.raster.width || py0 >= (double)l.raster.height) continue;
                if (px1 > (double)l.raster.width) px1 = (double)l.raster.width;
                if (py1 > (double)l.raster.height) py1 = (double)l.raster.height;
                // 四角世界坐标(源CRS)
                double wx[4] = { g0 + px0*g1 + py0*g2, g0 + px1*g1 + py0*g2,
                                 g0 + px1*g1 + py1*g2, g0 + px0*g1 + py1*g2 };
                double wy[4] = { g3 + px0*g4 + py0*g5, g3 + px1*g4 + py0*g5,
                                 g3 + px1*g4 + py1*g5, g3 + px0*g4 + py1*g5 };
                // 重投影到显示CRS
                if (l.raster.srcEpsg != 0 && scene.displayEpsg != 0 &&
                    l.raster.srcEpsg != scene.displayEpsg) {
                    for (int k = 0; k < 4; k++) {
                        double dx, dy;
                        if (reprojectPoint(wx[k], wy[k], l.raster.srcEpsg, scene.displayEpsg, dx, dy))
                            { wx[k] = dx; wy[k] = dy; }
                    }
                }
                double minx = std::min({wx[0], wx[1], wx[2], wx[3]});
                double maxx = std::max({wx[0], wx[1], wx[2], wx[3]});
                double miny = std::min({wy[0], wy[1], wy[2], wy[3]});
                double maxy = std::max({wy[0], wy[1], wy[2], wy[3]});
                std::vector<float> pos(24);
                fillQuad(pos, minx, miny, maxx, maxy);
                // 垂直翻转每个块的 UV(v -> 1-v): OpenGL 纹理原点左下 + GDAL top-down, 与底图一致
                for (int k = 0; k < 6; k++) {
                    pos[k*4+2] = r.tplVerts[k*4+2];
                    pos[k*4+3] = 1.0f - r.tplVerts[k*4+3];
                }
                glBindVertexArray(t.vao);
                glBindBuffer(GL_ARRAY_BUFFER, t.vbo);
                glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * pos.size(), pos.data());
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, t.tex);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                glBindVertexArray(0);
            }
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(program);   // 切回矢量程序
    }

    glUseProgram(program);
    glUniform2f(locCenter, (float)scene.view.centerX, (float)scene.view.centerY);
    glUniform2f(locInv, (float)invx, (float)invy);
    glUniform3f(locColor, 0.3f, 0.8f, 0.9f);
    glUniform1f(locAlpha, 1.0f);

    if (geoms.empty()) {
        glBindVertexArray(gridVao);
        glDrawArrays(GL_LINES, 0, (GLsizei)gridCount);
        glBindVertexArray(0);
    } else {
        long long totalPts = 0, totalLines = 0, totalFill = 0;
        // 面填充 pass(带 alpha 混合, 半透明): 先画, 以便描边/点压在上面
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (size_t i = 0; i < geoms.size(); i++) {
            if (i < scene.layers.size() && !scene.layers[i].info.visible) continue;
            LayerGeom& g = geoms[i];
            long long fillDraw = 0;
            if (g.committed) {
                for (auto& b : g.buckets)
                    if (b.resident && b.fcount > 0) fillDraw += b.fcount;
            } else {
                fillDraw = g.fcount;
            }
            if (fillDraw <= 0) continue;
            if (i < scene.layers.size())
                glUniform3f(locColor,
                            scene.layers[i].color[0], scene.layers[i].color[1], scene.layers[i].color[2]);
            float a = 0.35f;   // 兜底: 默认半透明
            if (i < scene.layers.size()) a = scene.layers[i].color[3];
            glUniform1f(locAlpha, std::max(0.0f, std::min(1.0f, a)));
            if (g.committed) {
                for (auto& b : g.buckets) {
                    if (!b.resident || b.fcount <= 0) continue;
                    glBindVertexArray(b.vao);
                    glDrawArrays(GL_TRIANGLES, (GLint)(b.count + b.pcount), (GLsizei)b.fcount);
                    glBindVertexArray(0);
                    totalFill += b.fcount;
                }
            } else {
                glBindVertexArray(g.fvao);
                glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g.fcount);
                glBindVertexArray(0);
                totalFill += g.fcount;
            }
        }
        glDisable(GL_BLEND);
        glUniform1f(locAlpha, 1.0f);

        for (size_t i = 0; i < geoms.size(); i++) {
            if (i < scene.layers.size() && !scene.layers[i].info.visible) continue;
            if (i < scene.layers.size())
                glUniform3f(locColor,
                            scene.layers[i].color[0], scene.layers[i].color[1], scene.layers[i].color[2]);
            LayerGeom& g = geoms[i];
            if (g.committed) {
                // 分块: 线/点按块各自 (first,count) 分段绘制
                for (auto& b : g.buckets) {
                    if (!b.resident) continue;
                    if (b.count > 0) {
                        glBindVertexArray(b.vao);
                        glDrawArrays(GL_LINES, 0, (GLsizei)b.count);
                        glBindVertexArray(0);
                        totalLines += b.count;
                    }
                    if (b.pcount > 0) {
                        glBindVertexArray(b.vao);
                        glDrawArrays(GL_POINTS, (GLint)b.count, (GLsizei)b.pcount);
                        glBindVertexArray(0);
                        totalPts += b.pcount;
                    }
                }
            } else {
                if (g.count > 0) {
                    glBindVertexArray(g.vao);
                    glDrawArrays(GL_LINES, 0, (GLsizei)g.count);
                    glBindVertexArray(0);
                    totalLines += g.count;
                }
                if (g.pcount > 0) {
                    glBindVertexArray(g.pvao);
                    glDrawArrays(GL_POINTS, 0, (GLsizei)g.pcount);
                    glBindVertexArray(0);
                    totalPts += g.pcount;
                }
            }
        }
        static int frameCount = 0;
        lastRenderMs_ = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tRenderStart).count();
        if (++frameCount % 120 == 0 && (totalPts > 0 || totalLines > 0 || totalFill > 0))
            spdlog::debug("[render] layers={} lines={} points={} fillVerts={}", geoms.size(),
                          totalLines, totalPts, totalFill);
    }

    // PEEK_DEBUG_FRAME=1: 每 240 帧打印渲染耗时、绘制量、驻留块
    static bool dbgFrame = getenv("PEEK_DEBUG_FRAME") != nullptr;
    if (dbgFrame) {
        static long long dbFrameCnt = 0;
        if (dbFrameCnt % 240 == 0 && !geoms.empty()) {
            long long resident = 0, totalBuckets = 0;
            for (const auto& g : geoms)
                if (g.committed) {
                    totalBuckets += (long long)g.buckets.size();
                    for (const auto& b : g.buckets) if (b.resident) resident++;
                }
            spdlog::info("[perf] render={:.2f}ms layers={} buckets={} resident={}",
                         lastRenderMs_, geoms.size(), totalBuckets, resident);
        }
        dbFrameCnt++;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// main.cpp 每帧调用: 打印帧间 dt（非 render 本体）
void GLBackend::debugFrameStats() {
    static bool enable = getenv("PEEK_DEBUG_FRAME") != nullptr;
    if (!enable) return;
    static auto tPrev = std::chrono::steady_clock::now();
    static long long cnt = 0;
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double, std::milli>(now - tPrev).count();
    tPrev = now;
    if (cnt % 240 == 0)
        spdlog::info("[perf] frame_dt={:.2f}ms fps={:.1f}", dt, dt > 0 ? 1000.0 / dt : 0.0);
    cnt++;
}
