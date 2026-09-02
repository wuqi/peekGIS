#include "gl_backend.h"
#include "map/map_scene.h"
#include "util/logger.h"
#include <glad/glad.h>
#include <vector>
#include <cmath>

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

void GLBackend::init() {
    program = buildProgram();
    glEnable(GL_PROGRAM_POINT_SIZE);   // 允许 VS 用 gl_PointSize 控制点大小(否则固定 1px)
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
    appendGeom(geoms[idx], v, pts, fill);
}

int GLBackend::addLayer(const std::vector<float>& v, const std::vector<float>& pts,
                        const std::vector<float>& fill) {
    int idx = addLayerPlaceholder();
    appendGeom(geoms[idx], v, pts, fill);
    return idx;
}

void GLBackend::updateLayer(int idx, const std::vector<float>& v, const std::vector<float>& pts,
                            const std::vector<float>& fill) {
    if (idx < 0 || idx >= (int)geoms.size()) return;
    LayerGeom& g = geoms[idx];
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
    geoms.erase(geoms.begin() + idx);
}

void GLBackend::clearLayers() {
    for (auto& g : geoms) {
        if (g.vao) glDeleteVertexArrays(1, &g.vao);
        if (g.vbo) glDeleteBuffers(1, &g.vbo);
        if (g.pvao) glDeleteVertexArrays(1, &g.pvao);
        if (g.pvbo) glDeleteBuffers(1, &g.pvbo);
        if (g.fvao) glDeleteVertexArrays(1, &g.fvao);
        if (g.fvbo) glDeleteBuffers(1, &g.fvbo);
    }
    geoms.clear();
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
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, texW, texH);
    glClearColor(0.12f, 0.13f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    double invx = 2.0 / (scene.view.scale * texW);
    double invy = 2.0 / (scene.view.scale * texH);

    glUseProgram(program);
    glUniform2f(glGetUniformLocation(program, "uCenter"),
                (float)scene.view.centerX, (float)scene.view.centerY);
    glUniform2f(glGetUniformLocation(program, "uInv"), (float)invx, (float)invy);
    glUniform3f(glGetUniformLocation(program, "uColor"), 0.3f, 0.8f, 0.9f);
    glUniform1f(glGetUniformLocation(program, "uAlpha"), 1.0f);

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
            if (geoms[i].fcount <= 0) continue;
            if (i < scene.layers.size())
                glUniform3f(glGetUniformLocation(program, "uColor"),
                            scene.layers[i].color[0], scene.layers[i].color[1], scene.layers[i].color[2]);
            float a = 0.35f;   // 兜底: 默认半透明
            if (i < scene.layers.size()) a = scene.layers[i].color[3];
            glUniform1f(glGetUniformLocation(program, "uAlpha"),
                        std::max(0.0f, std::min(1.0f, a)));
            glBindVertexArray(geoms[i].fvao);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)geoms[i].fcount);
            glBindVertexArray(0);
            totalFill += geoms[i].fcount;
        }
        glDisable(GL_BLEND);
        glUniform1f(glGetUniformLocation(program, "uAlpha"), 1.0f);

        for (size_t i = 0; i < geoms.size(); i++) {
            if (i < scene.layers.size() && !scene.layers[i].info.visible) continue;
            if (i < scene.layers.size())
                glUniform3f(glGetUniformLocation(program, "uColor"),
                            scene.layers[i].color[0], scene.layers[i].color[1], scene.layers[i].color[2]);
            if (geoms[i].count > 0) {
                glBindVertexArray(geoms[i].vao);
                glDrawArrays(GL_LINES, 0, (GLsizei)geoms[i].count);
                glBindVertexArray(0);
                totalLines += geoms[i].count;
            }
            if (geoms[i].pcount > 0) {
                glBindVertexArray(geoms[i].pvao);
                glDrawArrays(GL_POINTS, 0, (GLsizei)geoms[i].pcount);
                glBindVertexArray(0);
                totalPts += geoms[i].pcount;
            }
        }
        static int frameCount = 0;
        if (++frameCount % 120 == 0 && (totalPts > 0 || totalLines > 0 || totalFill > 0))
            spdlog::debug("[render] layers={} lines={} points={} fillVerts={}", geoms.size(),
                          totalLines, totalPts, totalFill);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
