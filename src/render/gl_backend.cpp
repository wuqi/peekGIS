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
#include <fstream>
#include <filesystem>
#include <zstd.h>

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

// 烘焙贴图: 片段 shader —— 采样 R8 覆盖度, 乘图层色(颜色可调)
static const char* BFS = R"(
#version 330 core
out vec4 fragColor;
uniform sampler2D uImage;
uniform vec4 uColor;
in vec2 UV;
void main(){ fragColor = vec4(uColor.rgb, texture(uImage, UV).r); })";

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

GLuint GLBackend::buildBakeProgram() {
    uint32_t vs = compileShader(GL_VERTEX_SHADER, RVS);
    uint32_t fs = compileShader(GL_FRAGMENT_SHADER, BFS);
    uint32_t p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    int ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetProgramInfoLog(p, 512, nullptr, buf);
        spdlog::error("bake program link error: {}", buf);
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

void GLBackend::init() {
    program = buildProgram();
    glEnable(GL_PROGRAM_POINT_SIZE);   // 允许 VS 用 gl_PointSize 控制点大小(否则固定 1px)
    rProgram = buildRasterProgram();
    bakeProgram = buildBakeProgram();

    locCenter = glGetUniformLocation(program, "uCenter");
    locInv = glGetUniformLocation(program, "uInv");
    locColor = glGetUniformLocation(program, "uColor");
    locAlpha = glGetUniformLocation(program, "uAlpha");
    rLocCenter = glGetUniformLocation(rProgram, "uCenter");
    rLocInv = glGetUniformLocation(rProgram, "uInv");
    rLocImage = glGetUniformLocation(rProgram, "uImage");
    bLocCenter = glGetUniformLocation(bakeProgram, "uCenter");
    bLocInv = glGetUniformLocation(bakeProgram, "uInv");
    bLocImage = glGetUniformLocation(bakeProgram, "uImage");
    bLocColor = glGetUniformLocation(bakeProgram, "uColor");

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
    g.blockRebuilding = false;   // 块桶层重建: 首个新桶块到达即视为进入新坐标系
    g.rebuilding = false;
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
    // 块=桶的块范围(供渲染视口裁剪): 块=源文件读取顺序, 但每块顶点仍是连续空间子集。
    // CPU 副本随后释放, 范围在 addBucket 此刻一次算清(O(n) 单层几毫秒)。
    {
        double bx0 = DBL_MAX, by0 = DBL_MAX, bx1 = -DBL_MAX, by1 = -DBL_MAX;
        auto growB = [&](const std::vector<float>& arr) {
            for (size_t q = 0; q + 1 < arr.size(); q += 2) {
                double x = arr[q], y = arr[q + 1];
                if (x < bx0) bx0 = x;
                if (y < by0) by0 = y;
                if (x > bx1) bx1 = x;
                if (y > by1) by1 = y;
            }
        };
        growB(b.v); growB(b.p); growB(b.f);
        b.minx = (bx0 <= bx1) ? bx0 : 0;
        b.miny = (by0 <= by1) ? by0 : 0;
        b.maxx = (bx0 <= bx1) ? bx1 : 0;
        b.maxy = (by0 <= by1) ? by1 : 0;
    }
    // 块桶层 GPU-first: 进桶即上传(正式加载/重建都随块流逐块走), 随即释放 CPU 副本。
    // 上传不再等渲染帧的 wanted/预算, 是唯一驻留点(全量驻留, 不做 LRU 兜底)。
    uploadBucket(b, frameNo);
    b.v.clear(); b.p.clear(); b.f.clear();
    b.v.shrink_to_fit(); b.p.shrink_to_fit(); b.f.shrink_to_fit();
    g.buckets.push_back(std::move(b));
}

bool GLBackend::isBlockBucketLayer(int idx) const {
    return idx >= 0 && idx < (int)geoms.size() && geoms[idx].blockBuckets;
}

bool GLBackend::isBlockRebuilding(int idx) const {
    return idx >= 0 && idx < (int)geoms.size() && geoms[idx].blockBuckets &&
           geoms[idx].blockRebuilding;
}

// 块桶层 CRS 切换: 清旧桶(旧坐标系), 进入重建态。新桶块随 enqueueRebuild 流回、
// 主线程 consume 首块时调 beginBucketLayer 结束重建态并开始边到边成桶。
void GLBackend::startBlockRebuild(int idx, int targetEpsg) {
    if (!isBlockBucketLayer(idx)) return;
    LayerGeom& g = geoms[idx];
    if (g.rebuilding) return;
    for (auto& b : g.buckets) evictBucket(b);
    g.buckets.clear();
    g.cellToBucket.clear();
    g.gridN = 0;
    g.blockRebuilding = true;
    g.reTarget = targetEpsg;
    g.bucketsEpsg = 0;
    glFlush();   // 尽早触发驱动回收旧桶显存, 缩短新旧桶交接重叠期
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

        // 验证/诊断: 块桶层假设滚动的裁剪收益(按视图中心各档放缩余维口结算残余定点)
        static const bool dbgBlock = getenv("PEEK_DEBUG_BLOCK") != nullptr;
        if (dbgBlock && g.blockBuckets) {
            static long long dbgBt = 0;
            if (frameNo - dbgBt > 30) {
                dbgBt = frameNo;
double cx = 0, cy = 0;   // 整层 bbox 中心(供采样)
                double ex0 = DBL_MAX, ey0 = DBL_MAX, ex1 = -DBL_MAX, ey1 = -DBL_MAX;
                long long totV = 0, totR = 0;
                for (auto& b : g.buckets) {
                    if (b.resident) totR++;
                    totV += b.count + b.pcount + b.fcount;
                    if (b.minx <= b.maxx) {
                        if (b.minx < ex0) ex0 = b.minx;
                        if (b.miny < ey0) ey0 = b.miny;
                        if (b.maxx > ex1) ex1 = b.maxx;
                        if (b.maxy > ey1) ey1 = b.maxy;
                    }
                }
                if (ex0 <= ex1) { cx = (ex0 + ex1) / 2; cy = (ey0 + ey1) / 2; }
                double ehw = (ex0 <= ex1) ? (ex1 - ex0) / 2 : 1, ehh = (ey0 <= ey1) ? (ey1 - ey0) / 2 : 1;
                std::string o;
                for (double z : {1.0, 8.0, 32.0, 128.0, 512.0}) {
                    double zhw = ehw / z, zhh = ehh / z;
                    long long iv = 0, nb = 0;
                    for (auto& b : g.buckets) {
                        if (!b.resident) continue;
                        if (b.maxx < cx - zhw || b.minx > cx + zhw ||
                            b.maxy < cy - zhh || b.miny > cy + zhh) continue;
                        nb++; iv += b.count + b.pcount + b.fcount;
                    }
                    o += " z" + std::to_string((long long)z) + "=" + std::to_string(nb) + "b/" +
                         std::to_string(iv / 1000000) + "M;";
                }
                spdlog::info("[BLK] gi={} buckets={} resident={} totV={}M | lyr=({:.4f},{:.4f},{:.4f},{:.4f}) | 裁剪后{}",
                             i, (int)g.buckets.size(), totR, totV / 1000000,
                             ex0, ey0, ex1, ey1, o);
            }
        }

        // 上传缺失的想看块(受每帧字节预算限制, 渐进上传; 上限满则暂停下帧续传)
        for (size_t bi = 0; bi < g.buckets.size(); bi++) {
            if (!wanted[bi] || g.buckets[bi].resident) continue;
            if (resident >= cap) break;
            // 块桶层不吃逐帧 32MB 预算: 整层本就要全量驻留, 一块到就传,
            // 避免块 CPU 副本在主线程堆积(加载/重建时内存高位)。
            if (!g.blockBuckets && budgetUsed >= kUploadBudget) break;   // 本帧预算用尽, 其余阻塞续传
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
        // 块桶层整层本就全量驻留 GPU(CPU 已释放, 无兜底可回传), 不做 LRU 淘汰。
        if (g.blockBuckets) break;
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

// ===== 烘焙 LOD =====
// 烘焙级别候选(分辨率, 升序); 实际按 GL_MAX_TEXTURE_SIZE 裁剪
static const int kAllBakeRes[] = {1024, 2048, 4096, 8192, 16384};
static const int kAllBakeN = 5;

uint64_t GLBackend::tileKey(int level, int tx, int ty) const {
    return ((uint64_t)level << 40) | ((uint64_t)(ty & 0xFFFFF) << 20) | (uint64_t)(tx & 0xFFFFF);
}

void GLBackend::setBakeBounds(int idx, double minx, double miny, double maxx, double maxy, int maxLevel) {
    if (idx < 0) return;
    if (bakes.size() < (size_t)idx + 1) bakes.resize(idx + 1);
    BakeLayer& bk = bakes[idx];
    if (bk.hasBounds && std::fabs(bk.minx - minx) < 1e-12 && std::fabs(bk.maxy - maxy) < 1e-12)
        return;
    removeBake(idx);
    bk.minx = minx; bk.miny = miny; bk.maxx = maxx; bk.maxy = maxy;
    bk.maxLevel = maxLevel;
    bk.hasBounds = true;
    glGenVertexArrays(1, &bk.vao);
    glGenBuffers(1, &bk.vbo);
    glBindVertexArray(bk.vao);
    glBindBuffer(GL_ARRAY_BUFFER, bk.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    glBindVertexArray(0);
    glGenVertexArrays(1, &bk.qvao);
    glGenBuffers(1, &bk.qvbo);
    {
        float uv[24] = {0,1, 1,1, 1,0, 0,1, 1,0, 0,0};
        std::vector<float> tpl(24, 0.0f);
        for (int i = 0; i < 6; i++) { tpl[i*4+2] = uv[i*2]; tpl[i*4+3] = uv[i*2+1]; }
        glBindVertexArray(bk.qvao);
        glBindBuffer(GL_ARRAY_BUFFER, bk.qvbo);
        glBufferData(GL_ARRAY_BUFFER, tpl.size()*sizeof(float), tpl.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
        glBindVertexArray(0);
    }
}

void GLBackend::markBakeCoverage(int idx, int level, int tx, int ty) {
    if (idx < 0 || idx >= (int)bakes.size()) return;
    BakeLayer& bk = bakes[idx];
    if (!bk.hasBounds) return;
    if (bk.covN == 0) { bk.covN = 256; bk.cov.assign((size_t)bk.covN * bk.covN, 0); }
    int nn = 1 << bk.maxLevel;
    if (nn <= 0) return;
    int cx = (int)(((double)tx + 0.5) / nn * bk.covN);
    int cy = (int)(((double)ty + 0.5) / nn * bk.covN);
    if (cx < 0) cx = 0; if (cx >= bk.covN) cx = bk.covN - 1;
    if (cy < 0) cy = 0; if (cy >= bk.covN) cy = bk.covN - 1;
    if (!bk.cov[(size_t)cy * bk.covN + cx]) { bk.cov[(size_t)cy * bk.covN + cx] = 1; bk.covDirty = true; }
}

void GLBackend::setBakeBuilding(int idx, bool b) {
    if (idx < 0 || idx >= (int)bakes.size()) return;
    BakeLayer& bk = bakes[idx];
    bk.buildingPyramid = b;
    if (!b) {
        bk.cov.clear(); bk.covN = 0; bk.covDirty = false;
        if (bk.covVao) { glDeleteVertexArrays(1, &bk.covVao); bk.covVao = 0; }
        if (bk.covVbo) { glDeleteBuffers(1, &bk.covVbo); bk.covVbo = 0; }
        bk.covVerts = 0;
    }
}

bool GLBackend::hasBakeBounds(int idx) const {
    return idx >= 0 && idx < (int)bakes.size() && bakes[idx].hasBounds;
}

int GLBackend::bakeLevelFor(int idx, const MapScene& scene) const {
    if (idx < 0 || idx >= (int)bakes.size()) return -1;
    const BakeLayer& bk = bakes[idx];
    if (!bk.hasBounds || bk.maxx <= bk.minx || scene.view.scale <= 0) return -1;
    double W = bk.maxx - bk.minx;
    double need = W / ((double)kTileRes * scene.view.scale);   // 需要的片数(1D)
    int L = 0;
    while ((1 << L) < need && L < 24) L++;
    return (L > bk.maxLevel) ? -1 : L;
}

bool GLBackend::bakeTileRange(int idx, int level, const MapScene& scene,
                              int& tx0, int& ty0, int& tx1, int& ty1) const {
    if (idx < 0 || idx >= (int)bakes.size()) return false;
    const BakeLayer& bk = bakes[idx];
    if (!bk.hasBounds) return false;
    double W = bk.maxx - bk.minx, H = bk.maxy - bk.miny;
    if (W <= 0 || H <= 0) return false;
    int n = 1 << level;
    double vx0 = scene.view.centerX - scene.view.vpW * 0.5 * scene.view.scale;
    double vx1 = scene.view.centerX + scene.view.vpW * 0.5 * scene.view.scale;
    double vy0 = scene.view.centerY - scene.view.vpH * 0.5 * scene.view.scale;
    double vy1 = scene.view.centerY + scene.view.vpH * 0.5 * scene.view.scale;
    tx0 = (int)std::floor((vx0 - bk.minx) / W * n);
    tx1 = (int)std::floor((vx1 - bk.minx) / W * n);
    ty0 = (int)std::floor((vy0 - bk.miny) / H * n);
    ty1 = (int)std::floor((vy1 - bk.miny) / H * n);
    tx0 = std::max(0, tx0); ty0 = std::max(0, ty0);
    tx1 = std::min(n - 1, tx1); ty1 = std::min(n - 1, ty1);
    return tx0 <= tx1 && ty0 <= ty1;
}

bool GLBackend::hasBakeTile(int idx, int level, int tx, int ty) const {
    if (idx < 0 || idx >= (int)bakes.size()) return false;
    const BakeLayer& bk = bakes[idx];
    auto it = bk.tiles.find(tileKey(level, tx, ty));
    return it != bk.tiles.end() && it->second.tex != 0;
}

void GLBackend::bakeTileBegin(int idx, int level, int tx, int ty) {
    if (idx < 0) return;
    if (bakes.size() < (size_t)idx + 1) bakes.resize(idx + 1);
    BakeLayer& bk = bakes[idx];
    uint64_t k = tileKey(level, tx, ty);
    BakeTile& t = bk.tiles[k];
    if (!t.tex) {
        glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kTileRes, kTileRes, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        glGenRenderbuffers(1, &t.rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, t.rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, kTileRes, kTileRes);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.rbo);
        static bool chk = false;
        if (!chk) { chk = true; GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            spdlog::info("[bake] FBO status=0x{:x} ({})", (unsigned)st,
                         st == GL_FRAMEBUFFER_COMPLETE ? "COMPLETE" : "INCOMPLETE"); }
        glViewport(0, 0, kTileRes, kTileRes);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    double W = bk.maxx - bk.minx, H = bk.maxy - bk.miny;
    int n = 1 << level;
    double x0 = bk.minx + (double)tx / n * W, x1 = bk.minx + (double)(tx + 1) / n * W;
    double y0 = bk.miny + (double)ty / n * H, y1 = bk.miny + (double)(ty + 1) / n * H;
    bk.curCx = (x0 + x1) * 0.5; bk.curCy = (y0 + y1) * 0.5;
    bk.curInvX = (x1 > x0) ? 2.0 / (x1 - x0) : 0;
    bk.curInvY = (y1 > y0) ? 2.0 / (y1 - y0) : 0;
    bk.curLevel = level; bk.curTx = tx; bk.curTy = ty;
    bk.baking = true;
    t.lastUse = frameNo;
}

void GLBackend::bakeTileAppend(int idx, std::vector<float>& v, std::vector<float>& p, std::vector<float>& f) {
    if (idx < 0 || idx >= (int)bakes.size()) return;
    BakeLayer& bk = bakes[idx];
    if (!bk.baking) return;
    long long nv = (long long)v.size()/2, np = (long long)p.size()/2, nf = (long long)f.size()/2;
    if (nv + np + nf == 0) return;
    std::vector<float> all;
    all.reserve(v.size() + p.size() + f.size());
    all.insert(all.end(), v.begin(), v.end());
    all.insert(all.end(), p.begin(), p.end());
    all.insert(all.end(), f.begin(), f.end());
    auto it = bk.tiles.find(tileKey(bk.curLevel, bk.curTx, bk.curTy));
    if (it == bk.tiles.end() || !it->second.fbo) return;
    glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
    glViewport(0, 0, kTileRes, kTileRes);
    glUseProgram(program);
    glUniform2f(locCenter, (float)bk.curCx, (float)bk.curCy);
    glUniform2f(locInv, (float)bk.curInvX, (float)bk.curInvY);
    glUniform3f(locColor, 1.0f, 1.0f, 1.0f);
    glBindVertexArray(bk.vao);
    glBindBuffer(GL_ARRAY_BUFFER, bk.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(all.size() * sizeof(float)), all.data(), GL_STREAM_DRAW);
    if (nf > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUniform1f(locAlpha, std::max(0.0f, std::min(1.0f, bk.color[3])));   // 面: 覆盖度=层透明度
        glDrawArrays(GL_TRIANGLES, (GLint)(nv + np), (GLsizei)nf);
        glDisable(GL_BLEND);
    }
    glUniform1f(locAlpha, 1.0f);   // 线/点: 覆盖度=1(不透明)
    if (nv > 0) {
        for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++) {
                glViewport(ox, oy, kTileRes, kTileRes);
                glDrawArrays(GL_LINES, 0, (GLsizei)nv);
            }
        glViewport(0, 0, kTileRes, kTileRes);
    }
    if (np > 0) glDrawArrays(GL_POINTS, (GLint)nv, (GLsizei)np);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// 按环模板(奇偶)填充: 环画三角扇翻模板, 再覆盖矩形过模板测试 -> 填充。无需耳切。
void GLBackend::bakeTileAppendRings(int idx, const std::vector<float>& lines,
                                    const std::vector<float>& points,
                                    const std::vector<std::vector<float>>& rings) {
    if (idx < 0 || idx >= (int)bakes.size()) return;
    BakeLayer& bk = bakes[idx];
    if (!bk.baking) return;
    if (getenv("PEEK_DEBUG_DRAW")) {
        static int dbg = 0;
        if (dbg < 3) { ++dbg; size_t nv = 0; for (auto& r : rings) nv += r.size(); spdlog::info("[bake-rings] rings={} ringFloats={} lines={} pts={}", rings.size(), nv, lines.size(), points.size()); }
    }
    auto it = bk.tiles.find(tileKey(bk.curLevel, bk.curTx, bk.curTy));
    if (it == bk.tiles.end() || !it->second.fbo) return;
    glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
    glViewport(0, 0, kTileRes, kTileRes);
    glUseProgram(program);
    glUniform2f(locCenter, (float)bk.curCx, (float)bk.curCy);
    glUniform2f(locInv, (float)bk.curInvX, (float)bk.curInvY);
    glUniform3f(locColor, 1.0f, 1.0f, 1.0f);
    glUniform1f(locAlpha, 1.0f);   // 翻模板的扇不能被 discard
    glBindVertexArray(bk.vao);
    glBindBuffer(GL_ARRAY_BUFFER, bk.vbo);

    // 1) 模板奇偶填充(只翻模板不写色)
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xff);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 1, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    for (const auto& ring : rings) {
        if (ring.size() < 6) continue;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(ring.size() * sizeof(float)), ring.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)(ring.size() / 2));
    }
    // 2) 覆盖矩形 + 模板测试 -> 填充
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(getenv("PEEK_STENCIL_OFF") ? GL_ALWAYS : GL_EQUAL, getenv("PEEK_STENCIL_OFF") ? 0 : 1, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniform1f(locAlpha, std::max(0.0f, std::min(1.0f, bk.color[3])));
    double hw = bk.curInvX != 0 ? 1.0 / bk.curInvX : 0, hh = bk.curInvY != 0 ? 1.0 / bk.curInvY : 0;
    float x0 = (float)(bk.curCx - hw), y0 = (float)(bk.curCy - hh);
    float x1 = (float)(bk.curCx + hw), y1 = (float)(bk.curCy + hh);
    float cq[12] = {x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1};
    glBufferData(GL_ARRAY_BUFFER, sizeof(cq), cq, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glUniform1f(locAlpha, 1.0f);

    // 3) 线/点
    if (!lines.empty()) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(lines.size() * sizeof(float)), lines.data(), GL_STREAM_DRAW);
        for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++) {
                glViewport(ox, oy, kTileRes, kTileRes);
                glDrawArrays(GL_LINES, 0, (GLsizei)(lines.size() / 2));
            }
        glViewport(0, 0, kTileRes, kTileRes);
    }
    if (!points.empty()) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(points.size() * sizeof(float)), points.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_POINTS, 0, (GLsizei)(points.size() / 2));
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLBackend::bakeTileEnd(int idx) {
    if (idx < 0 || idx >= (int)bakes.size()) return;
    BakeLayer& bk = bakes[idx];
    if (getenv("PEEK_DUMP_BAKE")) {
        auto it = bk.tiles.find(tileKey(bk.curLevel, bk.curTx, bk.curTy));
        if (it != bk.tiles.end() && it->second.fbo) {
            std::vector<unsigned char> px((size_t)kTileRes * kTileRes);
            glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, kTileRes, kTileRes, GL_RED, GL_UNSIGNED_BYTE, px.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            std::error_code ec;
            std::filesystem::create_directories("cache/debug", ec);
            char fn[96];
            std::snprintf(fn, sizeof(fn), "cache/debug/tile_%d_%d_%d.raw", bk.curLevel, bk.curTx, bk.curTy);
            std::ofstream of(fn, std::ios::binary);
            of.write((const char*)px.data(), (std::streamsize)px.size());
        }
    }
    bk.baking = false;
}

void GLBackend::uploadBakeTile(int idx, int level, int tx, int ty, const unsigned char* px, int res) {
    if (idx < 0) return;
    if (bakes.size() < (size_t)idx + 1) bakes.resize(idx + 1);
    BakeLayer& bk = bakes[idx];
    uint64_t k = tileKey(level, tx, ty);
    BakeTile& t = bk.tiles[k];
    if (!t.tex) {
        glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, res, res, 0, GL_RED, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, 0);
    t.lastUse = frameNo;
}

bool GLBackend::dumpBakeTile(int idx, int level, int tx, int ty, std::vector<unsigned char>& out) {
    if (idx < 0 || idx >= (int)bakes.size()) return false;
    BakeLayer& bk = bakes[idx];
    auto it = bk.tiles.find(tileKey(level, tx, ty));
    if (it == bk.tiles.end() || !it->second.fbo) return false;
    out.resize((size_t)kTileRes * kTileRes);
    glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, kTileRes, kTileRes, GL_RED, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void GLBackend::evictBakeTiles(int idx, size_t maxTiles) {
    if (idx < 0 || idx >= (int)bakes.size()) return;
    BakeLayer& bk = bakes[idx];
    while (bk.tiles.size() > maxTiles && !bk.tiles.empty()) {
        auto victim = bk.tiles.begin();
        for (auto it = bk.tiles.begin(); it != bk.tiles.end(); ++it)
            if (it->second.lastUse < victim->second.lastUse) victim = it;
        if (victim->second.fbo) glDeleteFramebuffers(1, &victim->second.fbo);
        if (victim->second.tex) glDeleteTextures(1, &victim->second.tex);
        if (victim->second.rbo) glDeleteRenderbuffers(1, &victim->second.rbo);
        bk.tiles.erase(victim);
    }
}

void GLBackend::removeBake(int idx) {
    if (idx < 0 || idx >= (int)bakes.size()) return;
    BakeLayer& bk = bakes[idx];
    for (auto& kv : bk.tiles) {
        if (kv.second.fbo) glDeleteFramebuffers(1, &kv.second.fbo);
        if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
        if (kv.second.rbo) glDeleteRenderbuffers(1, &kv.second.rbo);
    }
    if (bk.vao) glDeleteVertexArrays(1, &bk.vao);
    if (bk.vbo) glDeleteBuffers(1, &bk.vbo);
    if (bk.qvao) glDeleteVertexArrays(1, &bk.qvao);
    if (bk.qvbo) glDeleteBuffers(1, &bk.qvbo);
    bk = BakeLayer{};
}

void GLBackend::setOverZoom(int idx, const std::vector<float>& v, const std::vector<float>& p,
                            const std::vector<float>& f, const float color[4]) {
    if (idx < 0) return;
    if (overzooms.size() < (size_t)idx + 1) overzooms.resize(idx + 1);
    OverZoom& oz = overzooms[idx];
    if (!oz.vao) {
        glGenVertexArrays(1, &oz.vao);
        glGenBuffers(1, &oz.vbo);
        glBindVertexArray(oz.vao);
        glBindBuffer(GL_ARRAY_BUFFER, oz.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
        glBindVertexArray(0);
    }
    std::vector<float> all;
    all.reserve(v.size() + p.size() + f.size());
    all.insert(all.end(), v.begin(), v.end());
    all.insert(all.end(), p.begin(), p.end());
    all.insert(all.end(), f.begin(), f.end());
    glBindVertexArray(oz.vao);
    glBindBuffer(GL_ARRAY_BUFFER, oz.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(all.size() * sizeof(float)),
                 all.empty() ? nullptr : all.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    oz.count = (long long)v.size() / 2;
    oz.pcount = (long long)p.size() / 2;
    oz.fcount = (long long)f.size() / 2;
    for (int i = 0; i < 4; i++) oz.color[i] = color[i];
    oz.has = !all.empty();
}

void GLBackend::clearOverZoom(int idx) {
    if (idx < 0 || idx >= (int)overzooms.size()) return;
    OverZoom& oz = overzooms[idx];
    if (oz.vao) glDeleteVertexArrays(1, &oz.vao);
    if (oz.vbo) glDeleteBuffers(1, &oz.vbo);
    oz = OverZoom{};
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

    // ===== 烘焙占位 pass: 视口内未烘好的片画半透明框("正在烘"), 烘好再真画 =====
    if (!bakes.empty()) {
        glUseProgram(program);
        glUniform2f(locCenter, (float)scene.view.centerX, (float)scene.view.centerY);
        glUniform2f(locInv, (float)invx, (float)invy);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        int phN = 0;
        // 构建期覆盖网格(随构建长出来, 与矢量同款)
        for (size_t i = 0; i < scene.layers.size() && i < bakes.size(); i++) {
            if (!scene.layers[i].info.visible) continue;
            BakeLayer& bk = bakes[i];
            if (!bk.buildingPyramid || bk.covN == 0) continue;
            const float* lc = scene.layers[i].color;
            glUniform3f(locColor, lc[0], lc[1], lc[2]);
            glUniform1f(locAlpha, 0.5f);
            if (bk.covDirty) {
                std::vector<float> v;
                double cw = (bk.maxx - bk.minx) / bk.covN, ch = (bk.maxy - bk.miny) / bk.covN;
                for (int cy = 0; cy < bk.covN; ++cy)
                    for (int cx = 0; cx < bk.covN; ++cx) {
                        if (!bk.cov[(size_t)cy * bk.covN + cx]) continue;
                        float x0 = (float)(bk.minx + cx * cw), y0 = (float)(bk.miny + cy * ch);
                        float x1 = (float)(bk.minx + (cx + 1) * cw), y1 = (float)(bk.miny + (cy + 1) * ch);
                        float q[12] = {x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1};
                        v.insert(v.end(), q, q + 12);
                    }
                if (!bk.covVao) {
                    glGenVertexArrays(1, &bk.covVao);
                    glGenBuffers(1, &bk.covVbo);
                    glBindVertexArray(bk.covVao);
                    glBindBuffer(GL_ARRAY_BUFFER, bk.covVbo);
                    glEnableVertexAttribArray(0);
                    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
                    glBindVertexArray(0);
                }
                glBindVertexArray(bk.covVao);
                glBindBuffer(GL_ARRAY_BUFFER, bk.covVbo);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(float)), v.data(), GL_DYNAMIC_DRAW);
                bk.covVerts = (long long)(v.size() / 2);
                bk.covDirty = false;
            }
            if (bk.covVerts > 0) {
                glBindVertexArray(bk.covVao);
                glDrawArrays(GL_TRIANGLES, 0, (GLsizei)bk.covVerts);
                glBindVertexArray(0);
                if (getenv("PEEK_DEBUG_DRAW")) {
                    static long long cc = 0;
                    if (cc++ % 30 == 0) spdlog::info("[BAKE] 覆盖网格 {} 格", bk.covVerts / 6);
                }
            }
        }
        for (size_t i = 0; i < scene.layers.size() && i < bakes.size(); i++) {
            if (!scene.layers[i].info.visible) continue;
            int L = bakeLevelFor((int)i, scene);
            if (L < 0) L = bakes[i].maxLevel;
            if (L < 0) continue;
            int tx0, ty0, tx1, ty1;
            if (!bakeTileRange((int)i, L, scene, tx0, ty0, tx1, ty1)) continue;
            BakeLayer& bk = bakes[i];
            if (bk.buildingPyramid) continue;   // 构建中: 用覆盖网格表示, 不画逐片占位框
            const float* lc = scene.layers[i].color;
            glUniform3f(locColor, lc[0], lc[1], lc[2]);
            int n = 1 << L;
            double W = bk.maxx - bk.minx, H = bk.maxy - bk.miny;
            for (int ty = ty0; ty <= ty1; ty++)
                for (int tx = tx0; tx <= tx1; tx++) {
                    auto it = bk.tiles.find(tileKey(L, tx, ty));
                    if (it != bk.tiles.end() && it->second.tex) continue;   // 已烘好, 不用占位
                    double x0 = bk.minx + (double)tx / n * W, x1 = bk.minx + (double)(tx + 1) / n * W;
                    double y0 = bk.miny + (double)ty / n * H, y1 = bk.miny + (double)(ty + 1) / n * H;
                    float q[12] = {(float)x0,(float)y0, (float)x1,(float)y0, (float)x1,(float)y1,
                                   (float)x0,(float)y0, (float)x1,(float)y1, (float)x0,(float)y1};
                    float e[16] = {(float)x0,(float)y0, (float)x1,(float)y0, (float)x1,(float)y0, (float)x1,(float)y1,
                                   (float)x1,(float)y1, (float)x0,(float)y1, (float)x0,(float)y1, (float)x0,(float)y0};
                    glBindVertexArray(bk.vao);
                    glBindBuffer(GL_ARRAY_BUFFER, bk.vbo);
                    glUniform1f(locAlpha, 0.22f);   // 半透明填充
                    glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_DYNAMIC_DRAW);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                    glUniform1f(locAlpha, 0.7f);    // 明显边框
                    glBufferData(GL_ARRAY_BUFFER, sizeof(e), e, GL_DYNAMIC_DRAW);
                    glDrawArrays(GL_LINES, 0, 8);
                    glBindVertexArray(0);
                    ++phN;
                }
        }
        glDisable(GL_BLEND);
        if (getenv("PEEK_DEBUG_DRAW")) {
            static long long pc = 0;
            if (pc++ % 30 == 0) spdlog::info("[BAKE] 占位框 {} 片 (scale={:.6f})", phN, scene.view.scale);
        }
    }

    // ===== 烘焙 LOD pass: 贴视口内已烘好的片(R8 覆盖度 × 图层色) =====
    if (!bakes.empty()) {
        glUseProgram(bakeProgram);
        glUniform2f(bLocCenter, (float)scene.view.centerX, (float)scene.view.centerY);
        glUniform2f(bLocInv, (float)invx, (float)invy);
        glUniform1i(bLocImage, 0);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        static const float uv[24] = {0,1, 1,1, 1,0, 0,1, 1,0, 0,0};
        int bakedN = 0;
        for (size_t i = 0; i < scene.layers.size() && i < bakes.size(); i++) {
            if (!scene.layers[i].info.visible) continue;
            int L = bakeLevelFor((int)i, scene);
            if (L < 0) L = bakes[i].maxLevel;
            if (L < 0) continue;
            int tx0, ty0, tx1, ty1;
            if (!bakeTileRange((int)i, L, scene, tx0, ty0, tx1, ty1)) continue;
            BakeLayer& bk = bakes[i];
            const float* lc = scene.layers[i].color;
            glUniform4f(bLocColor, lc[0], lc[1], lc[2], lc[3]);
            int n = 1 << L;
            double W = bk.maxx - bk.minx, H = bk.maxy - bk.miny;
            for (int ty = ty0; ty <= ty1; ty++)
                for (int tx = tx0; tx <= tx1; tx++) {
                    auto it = bk.tiles.find(tileKey(L, tx, ty));
                    if (it == bk.tiles.end() || !it->second.tex) continue;   // 未烘好: 由占位框表示
                    it->second.lastUse = frameNo;
                    double x0 = bk.minx + (double)tx / n * W, x1 = bk.minx + (double)(tx + 1) / n * W;
                    double y0 = bk.miny + (double)ty / n * H, y1 = bk.miny + (double)(ty + 1) / n * H;
                    std::vector<float> pos(24, 0.0f);
                    fillQuad(pos, x0, y0, x1, y1);
                    for (int k = 0; k < 6; k++) { pos[k*4+2] = uv[k*2]; pos[k*4+3] = uv[k*2+1]; }
                    glBindVertexArray(bk.qvao);
                    glBindBuffer(GL_ARRAY_BUFFER, bk.qvbo);
                    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float)*pos.size(), pos.data());
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, it->second.tex);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                    glBindVertexArray(0);
                    bakedN++;
                }
        }
        glDisable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(program);
        if (getenv("PEEK_DEBUG_DRAW")) {
            static long long dc = 0;
            if (dc++ % 30 == 0)
                spdlog::info("[BAKE] 本帧贴烘焙瓦片 {} 片 (scale={:.6f})", bakedN, scene.view.scale);
        }
    }
    // 显存片 LRU: 每层限制片数(淘汰最久未用, 本帧用过的 lastUse 最大不会被淘汰)
    for (size_t i = 0; i < bakes.size(); i++) evictBakeTiles((int)i, 128);

    // ===== over-zoom pass: 放大超过烘焙精度时画查询到的矢量几何 =====
    if (!overzooms.empty()) {
        glUseProgram(program);
        glUniform2f(locCenter, (float)scene.view.centerX, (float)scene.view.centerY);
        glUniform2f(locInv, (float)invx, (float)invy);
        int ozN = 0;
        for (size_t i = 0; i < overzooms.size(); i++) {
            OverZoom& oz = overzooms[i];
            if (!oz.has) continue;
            if (i < scene.layers.size() && !scene.layers[i].info.visible) continue;
            ozN++;
            if (oz.fcount > 0) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glUniform3f(locColor, oz.color[0], oz.color[1], oz.color[2]);
                glUniform1f(locAlpha, std::max(0.0f, std::min(1.0f, oz.color[3])));
                glBindVertexArray(oz.vao);
                glDrawArrays(GL_TRIANGLES, (GLint)(oz.count + oz.pcount), (GLsizei)oz.fcount);
                glBindVertexArray(0);
                glDisable(GL_BLEND);
            }
            glUniform1f(locAlpha, 1.0f);
            glBindVertexArray(oz.vao);
            if (oz.count > 0) glDrawArrays(GL_LINES, 0, (GLsizei)oz.count);
            if (oz.pcount > 0) glDrawArrays(GL_POINTS, (GLint)oz.count, (GLsizei)oz.pcount);
            glBindVertexArray(0);
        }
        if (getenv("PEEK_DEBUG_DRAW")) {
            static long long oc = 0;
            if (oc++ % 30 == 0)
                spdlog::info("[OVERZOOM] 本帧画 over-zoom {} 层 (scale={:.6f})", ozN, scene.view.scale);
        }
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
        // 视口裁剪: 只把与当前屏幕窗口相交的桶送 GPU(块桶层范围在 addBucket 算好;
        // 未知范围(空块)保守画)。块=文件顺序, 但每块仍是连续空间子集 => 收益显著。
        const double vx0 = scene.view.centerX - (double)texW * 0.5 * scene.view.scale;
        const double vx1 = scene.view.centerX + (double)texW * 0.5 * scene.view.scale;
        const double vy0 = scene.view.centerY - (double)texH * 0.5 * scene.view.scale;
        const double vy1 = scene.view.centerY + (double)texH * 0.5 * scene.view.scale;
        auto bucketVisible = [&](const VectorBucket& b) {
            if (b.minx > b.maxx) return true;   // 无范围(空/未算)不裁
            return !(b.maxx < vx0 || b.minx > vx1 || b.maxy < vy0 || b.miny > vy1);
        };
        auto cullStat = [&](const std::vector<VectorBucket>& bs) {
            long long dv = 0, db = 0;
            for (const auto& b : bs)
                if (b.resident && bucketVisible(b)) { db++; dv += b.count + b.pcount + b.fcount; }
            return std::pair<long long, long long>{db, dv};
        };
        long long lFill = 0, lLines = 0, lPts = 0;
        // 面填充 pass(带 alpha 混合, 半透明): 先画, 以便描边/点压在上面
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (size_t i = 0; i < geoms.size(); i++) {
            if (i < scene.layers.size() && !scene.layers[i].info.visible) continue;
            if (bakeLevelFor((int)i, scene) >= 0) continue;   // 该层已贴烘焙图, 跳过矢量
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
                    if (!b.resident || b.fcount <= 0 || !bucketVisible(b)) continue;
                    glBindVertexArray(b.vao);
                    glDrawArrays(GL_TRIANGLES, (GLint)(b.count + b.pcount), (GLsizei)b.fcount);
                    glBindVertexArray(0);
                    totalFill += b.fcount;
                    lFill += b.fcount;
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
            if (bakeLevelFor((int)i, scene) >= 0) continue;   // 该层已贴烘焙图, 跳过矢量
            if (i < scene.layers.size())
                glUniform3f(locColor,
                            scene.layers[i].color[0], scene.layers[i].color[1], scene.layers[i].color[2]);
            LayerGeom& g = geoms[i];
            if (g.committed) {
                // 分块: 线/点按块各自 (first,count) 分段绘制(视口裁剪)
                for (auto& b : g.buckets) {
                    if (!b.resident || !bucketVisible(b)) continue;
                    if (b.count > 0) {
                        glBindVertexArray(b.vao);
                        glDrawArrays(GL_LINES, 0, (GLsizei)b.count);
                        glBindVertexArray(0);
                        totalLines += b.count;
                        lLines += b.count;
                    }
                    if (b.pcount > 0) {
                        glBindVertexArray(b.vao);
                        glDrawArrays(GL_POINTS, (GLint)b.count, (GLsizei)b.pcount);
                        glBindVertexArray(0);
                        totalPts += b.pcount;
                        lPts += b.pcount;
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

        // PEEK_DEBUG_DRAW=1: 每 30 帧打印实际送 GPU 的桶/顶点 vs 驻留总量(裁剪正确性 + 收益)
        static const bool dbgDraw = getenv("PEEK_DEBUG_DRAW") != nullptr;
        if (dbgDraw) {
            static long long dbgDc = 0;
            if (dbgDc++ % 30 == 0) {
                for (size_t i = 0; i < geoms.size(); i++) {
                    LayerGeom& g = geoms[i];
                    if (!g.committed) continue;
                    long long totR = 0, totV = 0;
                    for (const auto& b : g.buckets)
                        if (b.resident) { totR++; totV += b.count + b.pcount + b.fcount; }
                    auto c = cullStat(g.buckets);
                    spdlog::info("[DRAW] gi={} committed buckets={} resident={} totV={}M | "
                                 "被裁剪画: buckets={} verts={}M (lines={}M fill={}M pts={}M) | view=({:.4f},{:.4f},{:.7f}) vp={}x{}",
                                 i, (long long)g.buckets.size(), totR, totV / 1000000,
                                 c.first, (c.second) / 1000000, lLines / 1000000, lFill / 1000000, lPts / 1000000,
                                 scene.view.centerX, scene.view.centerY, scene.view.scale, texW, texH);
                }
            }
        }
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
