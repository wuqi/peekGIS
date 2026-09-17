#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // 必须先于 GLFW/glfw3.h, 避免 APIENTRY 宏重定义(C4005)
#endif
#include "ui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "data/reproject.h"
#include "data/gdal_common.h"
#include "data/geom_cache.h"
#include "vt/vt_cache.h"
#include "platform/file_dialog.h"
#include "platform/exe_path.h"
#include "app/panels.h"
#include "app/toolbox_panel.h"
#include "glad/glad.h"
#include "stb_image.h"
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

using namespace peekg::data;

namespace fs = std::filesystem;

static ImVec2 g_mapRectMin = {0, 0};
static double g_mouseMapX = 0, g_mouseMapY = 0;
static bool g_dragging = false;
static double g_prevMX = 0, g_prevMY = 0;

// 把源 CRS 的顶点数组重投影到显示 CRS(失败返回原样), 供高亮叠加用
static void toDisplayCrs(const std::vector<float>& src, int srcEpsg, int dispEpsg,
                         std::vector<float>& dst) {
    if (src.empty()) { dst.clear(); return; }
    if (dispEpsg == 0 || srcEpsg == 0 || srcEpsg == dispEpsg || !reprojectVertices(src, srcEpsg, dispEpsg, dst)) {
        dst = src;
    }
}

// 世界坐标(显示CRS) -> 地图窗口内的像素坐标(可越界, 由 ImGui 裁剪)
static void worldToPixel(const MapScene& scene, double wx, double wy, float& sx, float& sy) {
    sx = (float)((wx - scene.view.centerX) / scene.view.scale + scene.view.vpW * 0.5);
    sy = (float)(scene.view.vpH * 0.5 - (wy - scene.view.centerY) / scene.view.scale);
}

// 地图上高亮选中要素的几何 + 双击点的即时标记(叠加在渲染纹理之上)
static void drawIdentifyHighlight(const MapScene& scene, const UIState& ui) {
    // 几何先到就画几何; 否则画完整命中(属性阶段完成)
    const std::vector<IdentifyHit>* hits = &ui.identify.geo;
    if (hits->empty()) hits = &ui.identify.full;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    static const ImU32 kFill = IM_COL32(255, 196, 0, 55);
    static const ImU32 kLine = IM_COL32(255, 176, 0, 255);
    static const ImU32 kMark = IM_COL32(255, 210, 80, 255);
    const float ox = g_mapRectMin.x, oy = g_mapRectMin.y;
    const int dispEpsg = scene.displayEpsg;
    auto P = [&](double wx, double wy) -> ImVec2 {
        float x, y;
        worldToPixel(scene, wx, wy, x, y);
        return ImVec2(ox + x, oy + y);
    };
    for (const auto& h : *hits) {
        bool hasGeom = !h.outline.empty() || !h.points.empty() || !h.fillTris.empty();

        // 栅格识别: 无几何, 用记录的显示坐标画持久高亮符号(十字+圈), 下次点击/清空替换删除
        if (h.hasRasterPoint) {
            ImVec2 c = P(h.rasterX, h.rasterY);
            const float r = 8.0f;
            dl->AddCircle(c, r, kLine, 24, 2.0f);
            dl->AddCircleFilled(c, 2.5f, kMark, 12);
            dl->AddLine(ImVec2(c.x - r - 7, c.y), ImVec2(c.x - 2, c.y), kLine, 1.8f);
            dl->AddLine(ImVec2(c.x + 2, c.y), ImVec2(c.x + r + 7, c.y), kLine, 1.8f);
            dl->AddLine(ImVec2(c.x, c.y - r - 7), ImVec2(c.x, c.y - 2), kLine, 1.8f);
            dl->AddLine(ImVec2(c.x, c.y + 2), ImVec2(c.x, c.y + r + 7), kLine, 1.8f);
            continue;   // 栅格命中无几何, 无需画后续几何
        }
        if (!hasGeom) continue;

        // 面填充(半透明, 先画垫底)
        if (!h.fillTris.empty()) {
            std::vector<float> tv;
            toDisplayCrs(h.fillTris, h.srcEpsg, dispEpsg, tv);
            for (size_t i = 0; i + 5 < tv.size(); i += 6) {
                dl->AddTriangleFilled(P(tv[i], tv[i + 1]), P(tv[i + 2], tv[i + 3]),
                                      P(tv[i + 4], tv[i + 5]), kFill);
            }
        }
        // 描边
        if (!h.outline.empty()) {
            std::vector<float> lv;
            toDisplayCrs(h.outline, h.srcEpsg, dispEpsg, lv);
            for (size_t i = 0; i + 3 < lv.size(); i += 4) {
                dl->AddLine(P(lv[i], lv[i + 1]), P(lv[i + 2], lv[i + 3]), kLine, 2.4f);
            }
        }
        // 点标记
        if (!h.points.empty()) {
            std::vector<float> pv;
            toDisplayCrs(h.points, h.srcEpsg, dispEpsg, pv);
            for (size_t i = 0; i + 1 < pv.size(); i += 2) {
                dl->AddCircleFilled(P(pv[i], pv[i + 1]), 4.5f, kMark, 14);
                dl->AddCircle(P(pv[i], pv[i + 1]), 8.5f, kLine, 24, 1.8f);
            }
        }
    }
    // 查询进行中且尚无几何: 在双击点画即时十字标记
    if (ui.identify.pending && hits->empty()) {
        ImVec2 c = P(ui.identify.x, ui.identify.y);
        const float r = 9.0f;
        dl->AddCircle(c, r, kLine, 24, 1.8f);
        dl->AddLine(ImVec2(c.x - r - 6, c.y), ImVec2(c.x - 1, c.y), kLine, 1.6f);
        dl->AddLine(ImVec2(c.x + 1, c.y), ImVec2(c.x + r + 6, c.y), kLine, 1.6f);
        dl->AddLine(ImVec2(c.x, c.y - r - 6), ImVec2(c.x, c.y - 1), kLine, 1.6f);
        dl->AddLine(ImVec2(c.x, c.y + 1), ImVec2(c.x, c.y + r + 6), kLine, 1.6f);
    }
}

// 属性表双击定位到要素: 面板上高亮该行几何(源 CRS -> 显示 CRS), 仅居中不平移缩放
static void drawAttrLocateHighlight(const MapScene& scene, const UIState& ui) {
    if (!ui.attr.hlActive) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    static const ImU32 kFill = IM_COL32(120, 220, 120, 60);
    static const ImU32 kLine = IM_COL32(100, 210, 100, 255);
    static const ImU32 kMark = IM_COL32(140, 240, 140, 255);
    const float ox = g_mapRectMin.x, oy = g_mapRectMin.y;
    const int dispEpsg = scene.displayEpsg;
    auto P = [&](double wx, double wy) -> ImVec2 {
        float x, y;
        worldToPixel(scene, wx, wy, x, y);
        return ImVec2(ox + x, oy + y);
    };
    std::vector<float> tv, lv, pv;
    if (!ui.attr.hlTris.empty()) {
        toDisplayCrs(ui.attr.hlTris, ui.attr.hlSrcEpsg, dispEpsg, tv);
        for (size_t i = 0; i + 5 < tv.size(); i += 6)
            dl->AddTriangleFilled(P(tv[i], tv[i + 1]), P(tv[i + 2], tv[i + 3]), P(tv[i + 4], tv[i + 5]), kFill);
    }
    if (!ui.attr.hlOutline.empty()) {
        toDisplayCrs(ui.attr.hlOutline, ui.attr.hlSrcEpsg, dispEpsg, lv);
        for (size_t i = 0; i + 3 < lv.size(); i += 4)
            dl->AddLine(P(lv[i], lv[i + 1]), P(lv[i + 2], lv[i + 3]), kLine, 2.4f);
    }
    if (!ui.attr.hlPoints.empty()) {
        toDisplayCrs(ui.attr.hlPoints, ui.attr.hlSrcEpsg, dispEpsg, pv);
        for (size_t i = 0; i + 1 < pv.size(); i += 2) {
            dl->AddCircleFilled(P(pv[i], pv[i + 1]), 5.0f, kMark, 16);
            dl->AddCircle(P(pv[i], pv[i + 1]), 9.0f, kLine, 24, 1.8f);
        }
    }
}

static void handleMapInput(MapScene& scene, int w, int h, UIState& ui) {
    ImGuiIO& io = ImGui::GetIO();
    // 鼠标相对地图 rect 的坐标
    g_mouseMapX = io.MousePos.x - g_mapRectMin.x;
    g_mouseMapY = io.MousePos.y - g_mapRectMin.y;

    bool inside = g_mouseMapX >= 0 && g_mouseMapY >= 0 &&
                  g_mouseMapX <= w && g_mouseMapY <= h;
    // 只有光标真正停在地图窗口上方时才响应交互; 否则悬停在弹窗/属性表等其它窗口时
    // 滚轮/双击/拖动都会"穿透"到地图(io.MouseWheel 等是全局量, 位置判断不足以区分)。
    bool hover = ImGui::IsWindowHovered();
    bool active = inside && hover;

    if (active && io.MouseDown[0]) {
        if (!g_dragging) { g_dragging = true; g_prevMX = g_mouseMapX; g_prevMY = g_mouseMapY; }
        double dx = g_mouseMapX - g_prevMX;
        double dy = g_mouseMapY - g_prevMY;
        scene.pan(dx, dy);
        ui.viewTouched = true;
        g_prevMX = g_mouseMapX; g_prevMY = g_mouseMapY;
    } else {
        g_dragging = false;
    }

    if (active && io.MouseWheel != 0.0f) {
        double factor = (io.MouseWheel > 0) ? 0.9 : 1.1;
        scene.zoomAt(factor, g_mouseMapX, g_mouseMapY);
        ui.viewTouched = true;
    }

    // 双击: 属性识别(点到显示坐标后由 main 查询各图层)
    if (active && io.MouseDoubleClicked[0]) {
        double wx, wy;
        scene.screenToWorld(g_mouseMapX, g_mouseMapY, wx, wy);
        ui.identify.requested = true;
        ui.identify.x = wx;
        ui.identify.y = wy;
    }
}

// 固定布局: 左:Layers / 中:Map / 底:属性表 / 右:Attributes (状态栏为 DockSpace 外普通渲染区)
// 按当前面板显隐动态构建: 隐藏的面板不占 dock 节点, 其空间交还给地图
static void buildDockLayout(const UIState& ui) {
    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

    ImGuiID remaining = dockspace_id;
    // 左: 图层树(0) 或 工具箱(1), 二选一(类似 VS Code 侧栏)
    const char* leftWin = ui.leftPanel == 0 ? "图层" : "工具箱";
    {
        ImGuiID left = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Left, 0.18f, nullptr, &remaining);
        ImGui::DockBuilderDockWindow(leftWin, left);
    }
    // 底: 属性表(可显隐)
    if (ui.showAttrTablePanel) {
        ImGuiID attrs = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Down, 0.30f, nullptr, &remaining);
        ImGui::DockBuilderDockWindow("属性表", attrs);
    }
    // 右: 属性识别(可显隐)
    if (ui.showAttributesPanel) {
        ImGuiID right = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Right, 0.24f, nullptr, &remaining);
        ImGui::DockBuilderDockWindow("属性识别", right);
    }
    // 剩余全部给地图
    ImGui::DockBuilderDockWindow("地图", remaining);

    ImGui::DockBuilderFinish(dockspace_id);
}

// ===== 左侧图标栏 =====
// 从 PNG 加载为 GL 纹理(供 ImGui::Image 显示)。惰性加载, 失败返回 0。
// 候选顺序: 传入路径(基础 fallback: exe 目录/assets/<同名> / assets/<同名>) -> 图层图标 -> 应用图标。
static GLuint g_iconLayersTex = 0;
static GLuint g_iconToolboxTex = 0;
static GLuint loadIconTexture(const char* path) {
    int w = 0, h = 0, n = 0;
    std::string fname = path;
    auto slash = fname.find_last_of("/\\");
    if (slash != std::string::npos) fname = fname.substr(slash + 1);
    std::vector<std::string> candv{ path };
    std::string exeDirStr = exeDir();
    if (!exeDirStr.empty()) {
        candv.push_back(exeDirStr + "/assets/" + fname);
        candv.push_back(exeDirStr + "/assets/layer-group-solid.png");
        candv.push_back(exeDirStr + "/assets/icon.png");
    }
    candv.push_back("assets/" + fname);
    candv.push_back("assets/layer-group-solid.png");
    candv.push_back("layer-group-solid.png");
    candv.push_back("assets/icon.png");
    for (const std::string& c : candv) {
        unsigned char* data = stbi_load(c.c_str(), &w, &h, &n, 4);
        if (!data) continue;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
        stbi_image_free(data);
        return tex;
    }
    return 0;
}

// 用 DrawList 手绘一个"图层堆叠"图标(三条平行四边形层, 类似 VS Code 图层面板图标)。
// 中心位于 (cx,cy), 大小为 half 半径。col 为颜色。
static void drawLayersVectorIcon(float cx, float cy, float half, ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int n = 3;                             // 层数
    const float shear = half * 0.85f;            // 平行四边形斜切偏移
    const float layerH = half * 2.0f / (float)n; // 每层高度
    // 自上而下绘制, 每层顶部短、底部宽, 形成透视堆叠
    for (int i = n - 1; i >= 0; i--) {
        float yTop = cy - half + i * layerH;
        float yBot = yTop + layerH;
        // 倾斜度随层加深
        float t = shear * (i + 1) / (float)n;
        ImVec2 p0(cx - half + t, yTop);
        ImVec2 p1(cx + half + t, yTop);
        ImVec2 p2(cx + half, yBot);
        ImVec2 p3(cx - half, yBot);
        dl->AddConvexPolyFilled(&p0, 4, col);
        dl->AddLine(p0, p1, col, 1.6f);
        dl->AddLine(p1, p2, col, 1.6f);
        dl->AddLine(p3, p2, col, 1.6f);
        dl->AddLine(p0, p3, col, 1.6f);
    }
    // 最外层完整轮廓加粗
    {
        float t = shear;
        ImVec2 p0(cx - half + t, cy - half);
        ImVec2 p1(cx + half + t, cy - half);
        ImVec2 p2(cx + half, cy + half);
        ImVec2 p3(cx - half, cy + half);
        dl->AddLine(p0, p1, col, 2.2f);
        dl->AddLine(p1, p2, col, 2.2f);
        dl->AddLine(p3, p2, col, 2.2f);
        dl->AddLine(p0, p3, col, 2.2f);
    }
}

// 绘制左侧图标条(类似 VS Code activity bar): 纵向堆叠按钮, 点击切换左侧面板。
static void drawIconBar(UIState& ui) {
    if (g_iconLayersTex == 0) g_iconLayersTex = loadIconTexture("assets/layer-group-solid.png");
    if (g_iconToolboxTex == 0) g_iconToolboxTex = loadIconTexture("assets/toolbox-solid.png");
    const float barW = 40.0f;
    const float iconSize = 30.0f;
    const float pad = (barW - iconSize) * 0.5f;
    const float y0 = 18.0f;   // 略低于菜单栏(退让菜单栏高度)
    const float gap = 8.0f;

    // 单个活动栏按钮: 图标 + active 高亮(左侧竖条 + 背景)
    auto drawBtn = [&](const char* id, GLuint tex, bool active, const char* tip, float y) {
        ImGui::SetCursorPos(ImVec2(0, y));
        ImGui::PushID(id);
        if (active) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 winMin = ImGui::GetWindowPos();
            dl->AddRectFilled(ImVec2(winMin.x, winMin.y + y), ImVec2(winMin.x + barW, winMin.y + y + iconSize + gap),
                              ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
            dl->AddRectFilled(ImVec2(winMin.x, winMin.y + y), ImVec2(winMin.x + 3, winMin.y + y + iconSize + gap),
                              ImGui::GetColorU32(ImGuiCol_ButtonActive));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, active ? 1.0f : 0.5f);
        ImGui::SetCursorPos(ImVec2(pad, y + 1));
        std::string lbl = "##" + std::string(id);
        bool clicked = false;
        if (tex) {
            // stb_image 行序自上而下, glTexImage2D 上传后 V=0 即图像顶行;
            // 这里不翻转 UV, 否则图标上下颠倒(与地图帧缓冲纹理相反)。
            clicked = ImGui::ImageButton(lbl.c_str(), (ImTextureID)(uintptr_t)tex,
                                         ImVec2(iconSize, iconSize), ImVec2(0, 0), ImVec2(1, 1));
        } else {
            ImGui::InvisibleButton(lbl.c_str(), ImVec2(iconSize, iconSize));
            ImU32 col = active ? ImGui::GetColorU32(ImGuiCol_Text)
                               : ImGui::GetColorU32(ImGuiCol_TextDisabled);
            if (ImGui::IsItemHovered()) col = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
            drawLayersVectorIcon(ImGui::GetItemRectMin().x + iconSize * 0.5f,
                                 ImGui::GetItemRectMin().y + iconSize * 0.5f, iconSize * 0.38f, col);
            clicked = ImGui::IsItemClicked();
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::PopID();
        return clicked;
    };

    const bool layersActive = (ui.leftPanel == 0);
    const bool toolboxActive = (ui.leftPanel == 1);
    if (drawBtn("__side_layers", g_iconLayersTex, layersActive, "图层面板", y0)) ui.leftPanel = 0;
    if (drawBtn("__side_toolbox", g_iconToolboxTex, toolboxActive, "工具箱", y0 + iconSize + gap)) ui.leftPanel = 1;
}

// 工具箱面板(工具目录/脚本工具/运行): 状态与实现收敛在 ToolboxPanel 类
ToolboxPanel g_toolboxPanel;

// 切换显示 CRS: 对每个图层的源 CRS 顶点用 PROJ 单遍重投影后重传 VBO
void applyDisplayCrs(MapScene& scene, GLBackend& backend, int dstEpsg) {
    if (scene.layers.empty()) return;
    auto tBegin0 = std::chrono::steady_clock::now();
    // Source(0) = 自动选择显示基准: 优先取地理坐标系(经纬度,度)作为统一显示 CRS。
    // 原因: 各图层的源 CRS 往往分属不同投影带甚至不同半球(如 UTM 55S 南半球栅格 +
    // CGCS2000 地理矢量/北半球), 若取首个投影系(带号归属南半球)为基准, 另一个半球的
    // 图层会被重投影到千万米外的错误位置而不可见。地理系所有图层都能落在合理范围内。
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
    int target = (dstEpsg == 0) ? baseEpsg : dstEpsg;

    char dbg[512];
    snprintf(dbg, sizeof(dbg), "[CRS] applyDisplayCrs dst=%d -> base=%d target=%d", dstEpsg, baseEpsg, target);
    for (size_t k = 0; k < scene.layers.size(); k++) {
        int e = (scene.layers[k].kind == LayerKind::Raster)
                    ? scene.layers[k].raster.srcEpsg
                    : scene.layers[k].data.srcEpsg;
        char t[128];
        snprintf(t, sizeof(t), "  L[%d] srcEpsg=%d", (int)k, e);
        strncat(dbg, t, sizeof(dbg) - strlen(dbg) - 1);
    }
    spdlog::info("{}", dbg);

    double gminx = 1e300, gminy = 1e300, gmaxx = -1e300, gmaxy = -1e300;
    for (size_t i = 0; i < scene.layers.size(); i++) {
        const VectorData& src = scene.layers[i].data;
        if (scene.layers[i].kind == LayerKind::Raster) {
            // 栅格: 四角重投影, 范围并入全局, 不改 VBO(纹理四边形在 render 时动态计算)
            reprojectRasterExtent(scene.layers[i].raster, target);
            if (scene.layers[i].raster.hasDispExtent) {
                gminx = std::min(gminx, scene.layers[i].raster.dispMinx);
                gminy = std::min(gminy, scene.layers[i].raster.dispMiny);
                gmaxx = std::max(gmaxx, scene.layers[i].raster.dispMaxx);
                gmaxy = std::max(gmaxy, scene.layers[i].raster.dispMaxy);
            } else {
                // 回退: 用源范围
                gminx = std::min(gminx, scene.layers[i].raster.minx);
                gminy = std::min(gminy, scene.layers[i].raster.miny);
                gmaxx = std::max(gmaxx, scene.layers[i].raster.maxx);
                gmaxy = std::max(gmaxy, scene.layers[i].raster.maxy);
            }
            continue;
        }
        // 已分块的矢量层: 后台重投影+重建(主线程不阻塞), 期间该层隐藏, 完成后换桶。
        // 判据是"已提交桶实际所在 CRS(bucketsEpsg)≠ 目标", 而不是源 CRS: 否则缓存命中且
        // 源==目标 的层, 桶却被旧显示 CRS 重建过(如后台重建中途显示切走), 会永久错过重建。
        // 仍在后台重建中的层, 若重建目标已过期则重启(旧结果由 token 作废)。
        // 仍在流式加载(未分块)的层跳过: 块到达已按当前 displayEpsg 处理, 完成时统一分块。
        if (i < backend.geoms.size()) {
            const auto& gg = backend.geoms[i];
            bool needRebuild = false;
            if (gg.blockRebuilding)
                needRebuild = (gg.reTarget != target);
            else if (gg.rebuilding)
                needRebuild = (gg.reTarget != target);
            else if (gg.committed)
                needRebuild = (gg.bucketsEpsg != target);
            if (needRebuild) {
                if (backend.isBlockBucketLayer((int)i)) {
                    // 块桶层: 无整层内存几何, CRS 切换=从缓存逐块重读重投影重建(块=桶)。
                    // 只置重建态(清旧桶); 实际 enqueueRebuild 由 App::update 每帧检测触发。
                    if (!gg.blockRebuilding || gg.reTarget != target) {
                        backend.startBlockRebuild((int)i, target);
                        spdlog::info("[CRS] layer[{}] 块桶层切 CRS -> {}: 从缓存重读重投影", (int)i, target);
                    }
                    continue;
                }
                auto snap = std::make_shared<VectorData>(src);
                backend.startLayerRebuild((int)i, snap, target);
            }
        }
        // 范围: 源 bbox 四角重投影到 target(仅 4 点, 便宜), 供即时适配视图
        if (src.minx <= src.maxx) {
            double mnx = src.minx, mny = src.miny, mxx = src.maxx, mxy = src.maxy;
            if (target != 0 && src.srcEpsg != 0 && src.srcEpsg != target) {
                const double cxx[4] = {src.minx, src.maxx, src.minx, src.maxx};
                const double cyy[4] = {src.miny, src.miny, src.maxy, src.maxy};
                double rx[4], ry[4];
                bool ok = true;
                for (int q = 0; q < 4; q++)
                    if (!reprojectPoint(cxx[q], cyy[q], src.srcEpsg, target, rx[q], ry[q])) { ok = false; break; }
                if (ok) {
                    mnx = *std::min_element(rx, rx + 4); mxx = *std::max_element(rx, rx + 4);
                    mny = *std::min_element(ry, ry + 4); mxy = *std::max_element(ry, ry + 4);
                }
            }
            gminx = std::min(gminx, mnx); gminy = std::min(gminy, mny);
            gmaxx = std::max(gmaxx, mxx); gmaxy = std::max(gmaxy, mxy);
        }
    }
    // 仅当确实算出了有效范围才 setExtent; 否则保持现状(避免哨兵/空的
    // (0,0,0,0) 范围把"适配视图"缩到原点, 使已就绪的图层不可见)。
    if (gminx <= gmaxx && gminy <= gmaxy && std::isfinite(gminx) && std::isfinite(gmaxx))
        scene.setExtent(gminx, gminy, gmaxx, gmaxy);
    scene.displayEpsg = target;
    scene.needRefit = true;
    spdlog::info("[CRS] applyDisplayCrs total {:.1f}ms", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tBegin0).count());
}

void renderUI(MapScene& scene, GLBackend& backend, AppConfig& cfg, UIState& ui) {
    static bool dockInit = false;
    static int prevShowLayers = 0;
    static bool prevShowAttr = true, prevShowTable = true;
    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    if (!dockInit) {
        dockInit = true;
        bool haveIni = (ImGui::GetIO().IniFilename != nullptr) &&
                       fs::exists(ImGui::GetIO().IniFilename);
        if (!haveIni) buildDockLayout(ui);  // 仅首次无 ini 时按默认显隐构建
    } else if (ui.leftPanel != prevShowLayers ||
               ui.showAttributesPanel != prevShowAttr ||
               ui.showAttrTablePanel != prevShowTable) {
        // 面板显隐/切换变化: 重建 dock 布局, 让地图接管/让出空间
        prevShowLayers = ui.leftPanel;
        prevShowAttr = ui.showAttributesPanel;
        prevShowTable = ui.showAttrTablePanel;
        buildDockLayout(ui);
    }

    // 左侧图标条(含菜单栏)占位后的 DockSpace。底部让出状态栏高度。
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float barW = 40.0f;
    const float statusH = ImGui::GetFrameHeight() + 6.0f;   // 状态栏普通渲染区高度
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + barW, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x - barW, vp->WorkSize.y - statusH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##DockHost", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::DockSpace(dockspace_id, ImVec2(0, 0));
    ImGui::End();

    // 底部状态栏(普通渲染区, 不参与 dock, 永无标题栏; 始终贴近窗口底)
    {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - statusH));
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, statusH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 3.0f));
        ImGui::Begin("##StatusBarHost", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        // ---- 状态栏内容 ----
        double wx = 0, wy = 0;
        scene.screenToWorld(g_mouseMapX, g_mouseMapY, wx, wy);
        int shownEpsg = scene.displayEpsg;
        const char* srcCrs = "?";
        int srcEpsg = 0;
        if (!scene.layers.empty()) {
            const MapLayer& l0 = scene.layers[0];
            srcCrs = l0.info.sourceCrs.c_str();
            srcEpsg = l0.kind == LayerKind::Raster ? l0.raster.srcEpsg : l0.data.srcEpsg;
        }
        if (shownEpsg == 0) shownEpsg = srcEpsg;
        {
            static int lastEpsg = -1;
            static int lastSrcEpsg = -1;
            if (shownEpsg != lastEpsg || srcEpsg != lastSrcEpsg) {
                lastEpsg = shownEpsg;
                lastSrcEpsg = srcEpsg;
                spdlog::info("[statusbar] dispEpsg={} layer0.sourceCrs={} layer0.srcEpsg={}",
                             shownEpsg, srcCrs, srcEpsg);
            }
        }
        ImGui::Text("x: %.4f  y: %.4f", wx, wy);
        ImGui::SameLine();
        ImGui::Text("| 显示坐标: EPSG:%d  源坐标: %s", shownEpsg, srcCrs);
        ImGui::SameLine();
        ImGui::Text("| 缩放比: %.4f  图层数: %d", scene.view.scale, (int)scene.layers.size());
        ImGui::SameLine();
        {
            int vl = backend.vtRenderer().displayLevel();
            if (vl >= 0) ImGui::Text("| 瓦片层: L%d", vl);
            else ImGui::Text("| 瓦片层: -");
        }
        ImGui::SameLine();
        ImGui::Text("| 帧率: %.0f FPS", ImGui::GetIO().Framerate);
        ImGui::SameLine();
        ImGui::Text("| 缓存上限: %lld MB", cfg.cache_max_mb);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cfg.cache_dir.c_str());

        // 加载进度(放状态栏, 避免浮在地图上方被遮住)
        ImGui::SameLine();
        if (ui.loadActive) {
            char pbuf[96];
            const char* pbase = ui.loadFraction >= 0.0f
                ? "| 加载 %d/%d (%d%%)"
                : "| 加载 %d/%d (扫描中...)";
            snprintf(pbuf, sizeof(pbuf), pbase,
                     ui.loadDoneFiles, ui.loadTotalFiles,
                     (int)(ui.loadFraction * 100.0f));
            ImGui::TextUnformatted(pbuf);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.30f, 0.68f, 0.90f, 0.9f));
            float frac = ui.loadFraction >= 0.0f ? ui.loadFraction : 0.0f;
            ImGui::ProgressBar(frac, ImVec2(90, 0), "");
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        if (!ui.status.empty()) {
            if (ui.statusErr) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", ui.status.c_str());
            else ImGui::Text("%s", ui.status.c_str());
        }
        ImGui::End();
    }

    // 左上部图标条(独立浮窗, 与底部属性/右侧属性无关)
    {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(barW, vp->WorkSize.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##IconBar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar(3);
        drawIconBar(ui);
        ImGui::End();
    }

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("打开...")) {
                std::string p = openFileDialog();
                if (!p.empty()) { ui.openPaths.push_back(p); ui.openRequested = true; }
            }
            if (ImGui::MenuItem("清空图层")) {
                ui.clearRequested = true;
            }
            if (ImGui::MenuItem("渲染 WKT...")) {
                ui.showWktDialog = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("缓存管理...")) {
                ui.showCacheManager = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图")) {
            if (ImGui::MenuItem("图层面板", nullptr, ui.leftPanel == 0)) ui.leftPanel = 0;
            if (ImGui::MenuItem("工具箱", nullptr, ui.leftPanel == 1)) ui.leftPanel = 1;
            ImGui::MenuItem("属性识别", nullptr, &ui.showAttributesPanel);
            ImGui::MenuItem("属性表", nullptr, &ui.showAttrTablePanel);
            ImGui::Separator();
            if (ImGui::BeginMenu("显示坐标系")) {
                static const char* crsNames[] = { "源坐标系", "EPSG:4326", "EPSG:3857", "EPSG:4490" };
                static const int crsEpsg[] = { 0, 4326, 3857, 4490 };
                for (int k = 0; k < IM_ARRAYSIZE(crsNames); k++) {
                    if (ImGui::MenuItem(crsNames[k], nullptr, ui.displayCrsChoice == k)) {
                        ui.displayCrsChoice = k;
                        applyDisplayCrs(scene, backend, crsEpsg[k]);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // 左栏: 图层树(leftPanel==0) 或 工具箱(leftPanel==1), 二选一
    if (ui.leftPanel == 0) {
        if (ImGui::Begin("图层")) {
            drawLayerPanel(scene, ui);
        }
        ImGui::End();
    }

    // 工具箱面板(leftPanel==1): 工具目录/脚本/表单(阶段 2/3 逐步填充)
    if (ui.leftPanel == 1) {
        if (ImGui::Begin("工具箱")) {
            g_toolboxPanel.draw(scene, ui);
        }
        ImGui::End();
    }

    // 栅格图层渲染设置弹窗(右键图层面板栅格图层 → 栅格设置)
    drawRasterSettingsDialog(scene, ui);

    // 右栏: 要素属性识别结果
    if (ui.showAttributesPanel) drawIdentifyPanel(ui);

    // 中栏: 地图
    if (ImGui::Begin("地图")) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        int w = (int)avail.x, h = (int)avail.y;
        g_mapRectMin = ImGui::GetCursorScreenPos();
        if (w > 0 && h > 0) {
            scene.view.vpW = w; scene.view.vpH = h;
            backend.resize(w, h);
            handleMapInput(scene, w, h, ui);
            // 调试钩子: PEEK_TEST_ZOOM=1 每 120 帧放大一点(验证选层/瓦片加载)
            static const bool testZoom = std::getenv("PEEK_TEST_ZOOM") != nullptr;
            if (testZoom) {
                static long long zf = 0;
                if (++zf % 120 == 0 && scene.view.scale > 2e-5) {
                    scene.zoomAt(0.8, w * 0.5, h * 0.5);
                    fprintf(stderr, "[testzoom] scale=%.6g\n", scene.view.scale);
                }
            }
            backend.render(scene);
            ImGui::Image((ImTextureID)(uintptr_t)backend.texture(), avail,
                         ImVec2(0, 1), ImVec2(1, 0));
            drawIdentifyHighlight(scene, ui);   // 高亮选中的识别要素(几何先发)
            drawAttrLocateHighlight(scene, ui); // 属性表双击定位到的要素高亮
        }

        // 多图层选择对话框: OpenPopup 与 BeginPopupModal 必须在同一窗口 ID 作用域
        if (ui.showLayerDialog) {
            const char* dlgTitle = ui.sdsDialog ? "选择要导入的子数据集" : "选择要导入的图层";
            ImGui::OpenPopup(dlgTitle);
            if (ImGui::BeginPopupModal(dlgTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("文件: %s", ui.layerDialogPath.c_str());
                if (ui.sdsDialog)
                    ImGui::Text("共 %d 个子数据集:", (int)ui.layerMeta.size());
                else
                    ImGui::Text("共 %d 个图层:", (int)ui.layerMeta.size());
                ImGui::Separator();

                if (ImGui::Button("全选")) {
                    for (auto& m : ui.layerMeta) m.selected = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("全不选")) {
                    for (auto& m : ui.layerMeta) m.selected = false;
                }
                ImGui::Separator();

                ImGui::BeginChild("layerlist", ImVec2(400, 250), ImGuiChildFlags_Borders);
                for (size_t i = 0; i < ui.layerMeta.size(); i++) {
                    auto& m = ui.layerMeta[i];
                    ImGui::PushID((int)i);
                    ImGui::Checkbox("##sel", &m.selected);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(m.name.c_str());
                    if (ui.sdsDialog && m.isGeolocVar) {
                        // geolocation 数据变量: 首维(时次)>1 时给时次选择; 默认第一个
                        if (m.auxCount > 1) {
                            ImGui::SameLine(ImGui::GetWindowWidth() - 96);
                            ImGui::SetNextItemWidth(68);
                            std::string items;
                            items.reserve((size_t)m.auxCount * 6);
                            for (int t = 0; t < m.auxCount; t++) {
                                items += "T" + std::to_string(t + 1);
                                items += '\0';
                            }
                            items += '\0';   // 双结尾
                            if (ImGui::Combo(("##t" + std::to_string(i)).c_str(),
                                             &m.auxSel, items.c_str())) {
                            }
                        } else {
                            ImGui::SameLine(ImGui::GetWindowWidth() - 80);
                            ImGui::TextDisabled("2D");
                        }
                    } else {
                        ImGui::SameLine(ImGui::GetWindowWidth() - 80);
                        if (ui.sdsDialog)
                            ImGui::TextDisabled("%lld 波段", m.featureCount);
                        else
                            ImGui::TextDisabled("%lld 要素", m.featureCount);
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();

                ImGui::Separator();
                int selCount = 0;
                for (auto& m : ui.layerMeta) if (m.selected) selCount++;

                if (ImGui::Button(ui.sdsDialog ? "加载选中子数据集" : "加载选中图层", ImVec2(160, 0)) && selCount > 0) {
                    ui.loadFilteredPath = ui.layerDialogPath;
                    ui.loadFilteredIndices.clear();
                    ui.loadFilteredMeta.clear();
                    for (size_t i = 0; i < ui.layerMeta.size(); i++) {
                        if (ui.layerMeta[i].selected) {
                            ui.loadFilteredIndices.push_back((int)i);
                            ui.loadFilteredMeta.push_back(ui.layerMeta[i]);
                        }
                    }
                    ui.loadFilteredRequested = true;
                    ui.showLayerDialog = false;
                    ui.layerMeta.clear();
                }
                ImGui::SameLine();
                if (ImGui::Button("取消", ImVec2(80, 0))) {
                    ui.showLayerDialog = false;
                    ui.layerMeta.clear();
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::End();

    // 底部: 属性表面板(双击行可居中定位 + 高亮; 编码下拉可固定解释编码)
    if (ui.showAttrTablePanel) drawAttrTablePanel(ui);
    // 缓存管理窗口
    if (ui.showCacheManager) {
        if (ImGui::Begin("缓存管理", &ui.showCacheManager)) {
            // 磁盘扫描成本高, 只在窗口打开的首帧及各操作后刷新, 不在每帧重扫
            static std::vector<CacheEntry> entries;
            static bool fresh = false;
            if (!fresh) {
                entries = GeomCache::listCacheEntries(cfg);
                fresh = true;
            }
            // 统计
            int64_t totalBytes = 0;
            for (auto& e : entries) totalBytes += e.bytes;
            ImGui::Text("缓存条目: %d    总大小: %.1f MB", (int)entries.size(), totalBytes / (1024.0 * 1024.0));
            ImGui::SameLine();
            if (ImGui::Button("全部清除")) {
                ImGui::OpenPopup("确认清除全部缓存");
            }
            if (ImGui::BeginPopupModal("确认清除全部缓存", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("确定要删除所有 %d 个缓存条目? 此操作不可撤销.", (int)entries.size());
                ImGui::Separator();
                if (ImGui::Button("删除全部", ImVec2(120, 0))) {
                    GeomCache::clearAllCache(cfg);
                    fresh = false;   // 清空后下次刷新
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("取消", ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::Separator();

            // 列表
            if (ImGui::BeginTable("cachetable", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("源路径", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                ImGui::TableSetupColumn("图层", ImGuiTableColumnFlags_WidthStretch, 0.2f);
                ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("最后访问", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < entries.size(); i++) {
                    auto& e = entries[i];
                    ImGui::TableNextRow();
                    // 源路径
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(e.sourcePath.c_str());
                    // 图层名
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(e.layerName.c_str());
                    // 大小
                    ImGui::TableNextColumn();
                    if (e.bytes >= 1024 * 1024)
                        ImGui::Text("%.1f MB", e.bytes / (1024.0 * 1024.0));
                    else
                        ImGui::Text("%.0f KB", e.bytes / 1024.0);
                    // 最后访问
                    ImGui::TableNextColumn();
                    if (e.lastAccess > 0) {
                        time_t sec = (time_t)(e.lastAccess / 1000);
                        struct tm ti; localtime_s(&ti, &sec);
                        char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &ti);
                        ImGui::TextUnformatted(buf);
                    } else {
                        ImGui::TextUnformatted("-");
                    }
                    // 删除按钮
                    ImGui::TableNextColumn();
                    ImGui::PushID((int)i);
                    if (ImGui::SmallButton("X")) {
                        GeomCache::deleteCacheEntry(e.sourceId, e.layerIdx, cfg);
                        fresh = false;   // 删除后下次刷新列表
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            // ---- v2 矢量瓦片缓存 ----
            ImGui::Separator();
            ImGui::TextUnformatted("矢量瓦片缓存 (v2)");
            static std::vector<peekg::vt::VtCacheEntry> vtEntries;
            static bool vtFresh = false;
            if (!vtFresh) {
                vtEntries = peekg::vt::listVtCaches(cfg.cache_dir);
                vtFresh = true;
            }
            int64_t vtBytes = 0;
            for (auto& e : vtEntries) vtBytes += (int64_t)e.bytes;
            ImGui::Text("瓦片缓存: %d 个    总大小: %.1f MB", (int)vtEntries.size(),
                        vtBytes / (1024.0 * 1024.0));
            ImGui::SameLine();
            if (ImGui::Button("全部清除##vt")) ImGui::OpenPopup("确认清除全部瓦片缓存");
            if (ImGui::BeginPopupModal("确认清除全部瓦片缓存", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("确定删除所有 %d 个瓦片缓存文件? 此操作不可撤销.", (int)vtEntries.size());
                ImGui::Separator();
                if (ImGui::Button("删除全部", ImVec2(120, 0))) {
                    peekg::vt::clearVtCaches(cfg.cache_dir);
                    vtFresh = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("取消", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            if (ImGui::BeginTable("vtcachetable", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("文件", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                ImGui::TableSetupColumn("EPSG(源/显示)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableSetupColumn("层", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < vtEntries.size(); i++) {
                    auto& e = vtEntries[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(e.path.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%d / %d", e.srcEpsg, e.dstEpsg);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", e.maxLevel);
                    ImGui::TableNextColumn();
                    if (e.bytes >= 1024 * 1024) ImGui::Text("%.1f MB", e.bytes / (1024.0 * 1024.0));
                    else ImGui::Text("%.0f KB", e.bytes / 1024.0);
                    ImGui::TableNextColumn();
                    ImGui::PushID((int)(i + 100000));
                    if (ImGui::SmallButton("X")) {
                        peekg::vt::deleteVtCache(e.path);
                        vtFresh = false;
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    // WKT 渲染对话框: 粘贴 WKT → 解析为新图层(随机 8 位字母数字名)
    if (ui.showWktDialog) {
        int base = scene.displayEpsg;
        if (base == 0 && !scene.layers.empty()) base = scene.layers[0].data.srcEpsg;
        if (base == 0) base = 4326;
        ui.wktCrsHint = "EPSG:" + std::to_string(base);
        ImGui::OpenPopup("渲染 WKT");
        if (ImGui::BeginPopupModal("渲染 WKT", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("粘贴 WKT (Point/LineString/Polygon/Multi*/GeometryCollection), 确定后新建图层。");
            ImGui::TextWrapped("坐标系默认取当前显示 CRS (%s); 无图层时默认 EPSG:4326.",
                               ui.wktCrsHint.c_str());
            ImGui::Separator();
            ImGui::InputTextMultiline("##wktinput", ui.wktBuf, sizeof(ui.wktBuf),
                                      ImVec2(520, 180),
                                      ImGuiInputTextFlags_AutoSelectAll);
            ImGui::Separator();
            if (ImGui::Button("确定", ImVec2(120, 0))) {
                ui.wktText = ui.wktBuf;
                ui.wktRequested = true;
                ui.showWktDialog = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(120, 0))) {
                ui.showWktDialog = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}