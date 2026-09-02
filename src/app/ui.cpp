#include "ui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "data/reproject.h"
#include "data/geom_cache.h"
#include "platform/file_dialog.h"
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <ctime>

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
    const std::vector<IdentifyHit>* hits = &ui.identifyGeo;
    if (hits->empty()) hits = &ui.identify;
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
    if (ui.identifyPending && hits->empty()) {
        ImVec2 c = P(ui.identifyX, ui.identifyY);
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
    if (!ui.attrHlActive) return;
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
    if (!ui.attrHlTris.empty()) {
        toDisplayCrs(ui.attrHlTris, ui.attrHlSrcEpsg, dispEpsg, tv);
        for (size_t i = 0; i + 5 < tv.size(); i += 6)
            dl->AddTriangleFilled(P(tv[i], tv[i + 1]), P(tv[i + 2], tv[i + 3]), P(tv[i + 4], tv[i + 5]), kFill);
    }
    if (!ui.attrHlOutline.empty()) {
        toDisplayCrs(ui.attrHlOutline, ui.attrHlSrcEpsg, dispEpsg, lv);
        for (size_t i = 0; i + 3 < lv.size(); i += 4)
            dl->AddLine(P(lv[i], lv[i + 1]), P(lv[i + 2], lv[i + 3]), kLine, 2.4f);
    }
    if (!ui.attrHlPoints.empty()) {
        toDisplayCrs(ui.attrHlPoints, ui.attrHlSrcEpsg, dispEpsg, pv);
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

    if (inside && io.MouseDown[0]) {
        if (!g_dragging) { g_dragging = true; g_prevMX = g_mouseMapX; g_prevMY = g_mouseMapY; }
        double dx = g_mouseMapX - g_prevMX;
        double dy = g_mouseMapY - g_prevMY;
        scene.pan(dx, dy);
        ui.viewTouched = true;
        g_prevMX = g_mouseMapX; g_prevMY = g_mouseMapY;
    } else {
        g_dragging = false;
    }

    if (inside && io.MouseWheel != 0.0f) {
        double factor = (io.MouseWheel > 0) ? 0.9 : 1.1;
        scene.zoomAt(factor, g_mouseMapX, g_mouseMapY);
        ui.viewTouched = true;
    }

    // 双击: 属性识别(点到显示坐标后由 main 查询各图层)
    if (inside && io.MouseDoubleClicked[0]) {
        double wx, wy;
        scene.screenToWorld(g_mouseMapX, g_mouseMapY, wx, wy);
        ui.identifyRequested = true;
        ui.identifyX = wx;
        ui.identifyY = wy;
    }
}

// 固定布局: 左:Layers / 中:Map / 下:StatusBar(仅首次启动无 ini 时构建)
static void buildDockLayout() {
    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

    ImGuiID remaining = dockspace_id;
    ImGuiID left = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Left, 0.18f, nullptr, &remaining);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Down, 0.07f, nullptr, &remaining);
    ImGuiID attrs = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Down, 0.30f, nullptr, &remaining);
    ImGuiID right = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Right, 0.24f, nullptr, &remaining);
    ImGuiID map = remaining;

    ImGui::DockBuilderDockWindow("Layers", left);
    ImGui::DockBuilderDockWindow("Map", map);
    ImGui::DockBuilderDockWindow("StatusBar", bottom);
    ImGui::DockBuilderDockWindow("Attributes", right);
    ImGui::DockBuilderDockWindow("属性表", attrs);
    ImGui::DockBuilderFinish(dockspace_id);
}

// 切换显示 CRS: 对每个图层的源 CRS 顶点用 PROJ 单遍重投影后重传 VBO
static void applyDisplayCrs(MapScene& scene, GLBackend& backend, int dstEpsg) {
    if (scene.layers.empty()) return;
    // Source = 以第一个图层的源 CRS 作为统一显示基准
    int baseEpsg = scene.layers[0].data.srcEpsg;
    int target = (dstEpsg == 0) ? baseEpsg : dstEpsg;

    double gminx = 1e300, gminy = 1e300, gmaxx = -1e300, gmaxy = -1e300;
    for (size_t i = 0; i < scene.layers.size(); i++) {
        const VectorData& src = scene.layers[i].data;
        std::vector<float> disp, dispPts, dispTris;
        if (target != 0 && src.srcEpsg != target) {
            if (!reprojectVertices(src.vertices, src.srcEpsg, target, disp))
                disp = src.vertices;  // 重投影失败回退源坐标
            if (!reprojectVertices(src.points, src.srcEpsg, target, dispPts))
                dispPts = src.points;
            if (!reprojectVertices(src.triangles, src.srcEpsg, target, dispTris))
                dispTris = src.triangles;
        } else {
            disp = src.vertices; dispPts = src.points; dispTris = src.triangles;
        }
        backend.updateLayer((int)i, disp, dispPts, dispTris);
        double a, b, c, d;
        computeExtent(disp, a, b, c, d);
        gminx = std::min(gminx, a); gminy = std::min(gminy, b);
        gmaxx = std::max(gmaxx, c); gmaxy = std::max(gmaxy, d);
        if (!dispPts.empty()) {
            computeExtent(dispPts, a, b, c, d);
            gminx = std::min(gminx, a); gminy = std::min(gminy, b);
            gmaxx = std::max(gmaxx, c); gmaxy = std::max(gmaxy, d);
        }
        if (!dispTris.empty()) {
            computeExtent(dispTris, a, b, c, d);
            gminx = std::min(gminx, a); gminy = std::min(gminy, b);
            gmaxx = std::max(gmaxx, c); gmaxy = std::max(gmaxy, d);
        }
    }
    scene.setExtent(gminx, gminy, gmaxx, gmaxy);
    scene.displayEpsg = target;
    scene.needRefit = true;
}

void renderUI(MapScene& scene, GLBackend& backend, AppConfig& cfg, UIState& ui) {
    static bool dockInit = false;
    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    if (!dockInit) {
        dockInit = true;
        bool haveIni = (ImGui::GetIO().IniFilename != nullptr) &&
                       fs::exists(ImGui::GetIO().IniFilename);
        if (!haveIni) buildDockLayout();
    }
    ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport());

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...")) {
                std::string p = openFileDialog();
                if (!p.empty()) { ui.openPaths.push_back(p); ui.openRequested = true; }
            }
            if (ImGui::MenuItem("Clear Layers")) {
                ui.clearRequested = true;
            }
            if (ImGui::MenuItem("Render WKT...")) {
                ui.showWktDialog = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Cache Manager...")) {
                ui.showCacheManager = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // 左栏: 图层
    if (ImGui::Begin("Layers")) {
        if (ImGui::Button("Open...")) {
            std::string p = openFileDialog();
            if (!p.empty()) { ui.openPaths.push_back(p); ui.openRequested = true; }
        }
        ImGui::SameLine();
        if (ImGui::Button("Fit View")) {
            scene.fitToView(scene.view.vpW, scene.view.vpH);
            ui.viewTouched = true;
        }
        ImGui::Separator();
        if (scene.layers.empty()) {
            ImGui::Text("(no layers)");
        }
        for (size_t li = 0; li < scene.layers.size(); li++) {
            auto& l = scene.layers[li];
            ImGui::PushID((int)li);
            // 颜色色块(点击展开编辑器, 含 alpha 即面填充透明度)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(l.color[0], l.color[1], l.color[2], l.color[3]));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(l.color[0]*0.8f, l.color[1]*0.8f, l.color[2]*0.8f, l.color[3]));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(l.color[0]*0.6f, l.color[1]*0.6f, l.color[2]*0.6f, l.color[3]));
            if (ImGui::Button("##c", ImVec2(18, 18)))
                ImGui::OpenPopup("colorpick");
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            ImGui::Checkbox(l.info.name.c_str(), &l.info.visible);
            if (ImGui::BeginPopup("colorpick")) {
                ImGui::ColorEdit4("##color", l.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                ImGui::EndPopup();
            }
            if (ImGui::BeginPopupContextItem("layerctx")) {
                if (ImGui::MenuItem("缩放到图层")) {
                    scene.zoomToLayer((int)li);
                    ui.viewTouched = true;
                }
                if (ImGui::MenuItem("打开属性表")) {
                    ui.attrOpenLayerIdx = (int)li;
                    ui.attrOpenRequested = true;
                }
                if (ImGui::MenuItem("卸载图层")) {
                    ui.removeLayerRequested = true;
                    ui.removeLayerIdx = (int)li;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::End();

    // 右栏: 要素属性识别结果
    if (ImGui::Begin("Attributes")) {
        if (ui.identifyPending) {
            ImGui::TextDisabled("查询中...");
            ImGui::SameLine();
            ImGui::TextWrapped("(后台线程遍历, 大文件需数秒)");
        } else if (ui.identify.empty()) {
            ImGui::TextDisabled("双击地图查询要素属性");
        } else {
            if (ImGui::Button("清空结果")) { ui.identify.clear(); ui.identifyGeo.clear(); }
            ImGui::SameLine();
            // 固定编码解释器: 用户选什么, 该面板字符串属性就按什么解码(不重查/不重读)
            if (ImGui::Combo("编码", &ui.identifyEncoding, kEncodingNames, kEncodingCount)) {
                TextEncoding e = (TextEncoding)ui.identifyEncoding;
                for (auto& h : ui.identify) h.applyEncoding(e);
                for (auto& h : ui.identifyGeo) h.applyEncoding(e);
            }
            ImGui::Separator();
            for (size_t i = 0; i < ui.identify.size(); i++) {
                auto& h = ui.identify[i];
                ImGui::PushID((int)i);
                ImGui::TextUnformatted(h.layerName.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", h.geomType.c_str());
                ImGui::Separator();
                for (const auto& a : h.attrs) {
                    ImGui::TextUnformatted(a.name.c_str());
                    ImGui::SameLine(0.0f, 16.0f);
                    ImGui::TextWrapped("%s", a.value.c_str());
                }
                ImGui::PopID();
                ImGui::Spacing();
            }
        }
    }
    ImGui::End();

    // 中栏: 地图
    if (ImGui::Begin("Map")) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        int w = (int)avail.x, h = (int)avail.y;
        g_mapRectMin = ImGui::GetCursorScreenPos();
        if (w > 0 && h > 0) {
            scene.view.vpW = w; scene.view.vpH = h;
            backend.resize(w, h);
            handleMapInput(scene, w, h, ui);
            backend.render(scene);
            ImGui::Image((ImTextureID)(uintptr_t)backend.texture(), avail,
                         ImVec2(0, 1), ImVec2(1, 0));
            drawIdentifyHighlight(scene, ui);   // 高亮选中的识别要素(几何先发)
            drawAttrLocateHighlight(scene, ui); // 属性表双击定位到的要素高亮
        }

        // 多图层选择对话框: OpenPopup 与 BeginPopupModal 必须在同一窗口 ID 作用域
        if (ui.showLayerDialog) {
            ImGui::OpenPopup("选择要导入的图层");
            if (ImGui::BeginPopupModal("选择要导入的图层", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("文件: %s", ui.layerDialogPath.c_str());
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
                    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
                    ImGui::TextDisabled("%lld 要素", m.featureCount);
                    ImGui::PopID();
                }
                ImGui::EndChild();

                ImGui::Separator();
                int selCount = 0;
                for (auto& m : ui.layerMeta) if (m.selected) selCount++;

                if (ImGui::Button("加载选中图层", ImVec2(140, 0)) && selCount > 0) {
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

    // 底栏: 状态栏(坐标 + 显示CRS切换)
    if (ImGui::Begin("StatusBar")) {
        static const char* crsItems[] = { "Source", "EPSG:4326", "EPSG:3857", "EPSG:4490" };
        if (ImGui::Combo("Display CRS", &ui.displayCrsChoice, crsItems, IM_ARRAYSIZE(crsItems))) {
            static const int crsEpsg[] = { 0, 4326, 3857, 4490 };
            applyDisplayCrs(scene, backend, crsEpsg[ui.displayCrsChoice]);
        }
        ImGui::SameLine();

        double wx = 0, wy = 0;
        scene.screenToWorld(g_mouseMapX, g_mouseMapY, wx, wy);
        int shownEpsg = scene.displayEpsg;
        const char* srcCrs = scene.layers.empty() ? "?" : scene.layers[0].data.sourceCrs.c_str();
        if (shownEpsg == 0 && !scene.layers.empty()) shownEpsg = scene.layers[0].data.srcEpsg;
        ImGui::Text("x: %.6f  y: %.6f", wx, wy);
        ImGui::SameLine();
        ImGui::Text("| disp: EPSG:%d  src: %s", shownEpsg, srcCrs);
        ImGui::SameLine();
        ImGui::Text("| scale: %.4f  layers: %d", scene.view.scale, (int)scene.layers.size());
        ImGui::SameLine();
        ImGui::Text("| cache: %s (%lld MB)", cfg.cache_dir.c_str(), cfg.cache_max_mb);

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
    }
    ImGui::End();

    // 底部: 属性表面板(双击行可居中定位 + 高亮; 编码下拉可固定解释编码)
    if (ImGui::Begin("属性表")) {
        if (!ui.attrTableOpen || !ui.attrInfo.ok) {
            ImGui::TextDisabled("在左侧图层面板 右键图层 → 打开属性表");
            ImGui::End();
        } else {
            TextEncoding enc = (TextEncoding)ui.attrEncoding;

            // 顶栏: 图层名 + 编码下拉(固定解释, 切换只重转已缓存页)
            ImGui::TextUnformatted(ui.attrInfo.layerName.c_str());
            ImGui::SameLine(0.0f, 16.0f);
            if (ui.attrInfo.canEncode) {
                if (ImGui::Combo("编码", &ui.attrEncoding, kEncodingNames, kEncodingCount)) {
                    enc = (TextEncoding)ui.attrEncoding;
                    for (auto& p : ui.attrPages) attrReencodePage(p, enc);
                }
            } else {
                ImGui::TextDisabled("(UTF-8)");
            }
            ImGui::SameLine(0.0f, 24.0f);
            if (ImGui::Button("选择列")) ImGui::OpenPopup("attr_cols");
            if (ImGui::BeginPopup("attr_cols")) {
                int nf = (int)ui.attrInfo.fields.size();
                if ((int)ui.attrShowCol.size() != nf) ui.attrShowCol.assign(nf, 1);
                bool anyOn = false;
                for (int k = 0; k < nf; k++) {
                    std::string nm = decodeRawToUtf8(ui.attrInfo.fields[k].rawName, enc);
                    if (nm.empty()) nm = "(字段" + std::to_string(k) + ")";
                    ImGui::Checkbox(nm.c_str(), (bool*)&ui.attrShowCol[k]);
                    if (ui.attrShowCol[k]) anyOn = true;
                }
                ImGui::Separator();
                if (ImGui::Button("全部显示")) ui.attrShowCol.assign(nf, 1);
                ImGui::SameLine();
                if (ImGui::Button("只留 FID")) ui.attrShowCol.assign(nf, 0);
                if (!anyOn) ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "至少保留一列");
                ImGui::EndPopup();
            }
            ImGui::Separator();

            // 翻页控件(总数未取到时先渲染行, 总页数/总行数就绪后再显示)
            long long total = ui.attrInfo.total;
            bool countKnown = total >= 0;
            int totalPages = 1;
            if (countKnown) {
                totalPages = (int)((total + ui.attrRowsPerPage - 1) / ui.attrRowsPerPage);
                if (totalPages < 1) totalPages = 1;
                if (ui.attrCurrentPage >= totalPages) ui.attrCurrentPage = totalPages - 1;
                if (ui.attrCurrentPage < 0) ui.attrCurrentPage = 0;
            }

            if (ImGui::Button("上一页") && ui.attrCurrentPage > 0) {
                ui.attrCurrentPage--;
                ui.attrGotoRequested = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("下一页") && (!countKnown || ui.attrCurrentPage + 1 < totalPages)) {
                ui.attrCurrentPage++;
                ui.attrGotoRequested = true;
            }
            ImGui::SameLine();
            if (countKnown)
                ImGui::Text("页 %d/%d    共 %lld 行", ui.attrCurrentPage + 1, totalPages, total);
            else
                ImGui::TextDisabled("页 %d/?    统计总数中...", ui.attrCurrentPage + 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            static int jumpPage = 1;
            ImGui::InputInt("跳页", &jumpPage, 0, 0);
            if (jumpPage < 1) jumpPage = 1;
            ImGui::SameLine();
            if (ImGui::Button("Go") && jumpPage - 1 != ui.attrCurrentPage) {
                ui.attrCurrentPage = jumpPage - 1;
                ui.attrGotoRequested = true;
            }
            if (ui.attrLoading) { ImGui::SameLine(); ImGui::TextDisabled("加载中..."); }
            ImGui::Separator();

            // 表格: FID 列 + 各字段列(可选列, 见顶部"选择列")
            const AttrPageData* cur = nullptr;
            for (const auto& p : ui.attrPages)
                if (p.page == ui.attrCurrentPage) { cur = &p; break; }
            int nField = (int)ui.attrInfo.fields.size();
            if ((int)ui.attrShowCol.size() != nField) ui.attrShowCol.assign(nField, 1);
            std::vector<int> vis;              // 可见字段的"新列序号"
            vis.reserve(nField);
            for (int k = 0; k < nField; k++) if (ui.attrShowCol[k]) vis.push_back(k);
            if (vis.empty()) vis.push_back(-1);  // 占位, 至少保留 FID
            int nCol = 1 + (int)vis.size();

            if (ImGui::BeginTable("attrtable", nCol,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("FID", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                for (int idx : vis) {
                    if (idx < 0) continue;
                    ImGui::TableSetupColumn(decodeRawToUtf8(ui.attrInfo.fields[idx].rawName, enc).c_str(),
                                            ImGuiTableColumnFlags_WidthStretch);
                }
                ImGui::TableHeadersRow();

                if (cur) {
                    for (size_t ri = 0; ri < cur->rows.size(); ri++) {
                        const AttrRow& row = cur->rows[ri];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        char fidbuf[32];
                        snprintf(fidbuf, sizeof(fidbuf), "%lld", (long long)row.fid);
                        ImGui::Selectable(fidbuf, false, ImGuiSelectableFlags_SpanAllColumns);
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            // 双击定位: 拷贝几何 + 计算源CRS质心, 交由 main 居中+高亮
                            if (row.hasGeom) {
                                ui.attrHlOutline = row.outline;
                                ui.attrHlPoints = row.points;
                                ui.attrHlTris = row.fillTris;
                                ui.attrHlSrcEpsg = ui.attrSrcEpsg;
                                ui.attrHlActive = true;
                                double gx = 0, gy = 0;
                                const std::vector<float>* g = nullptr;
                                if (!row.points.empty()) g = &row.points;
                                else if (!row.outline.empty()) g = &row.outline;
                                else if (!row.fillTris.empty()) g = &row.fillTris;
                                if (g && !g->empty()) {
                                    size_t cnt = g->size() / 2;
                                    for (size_t i = 0; i + 1 < g->size(); i += 2) { gx += (*g)[i]; gy += (*g)[i + 1]; }
                                    gx /= (double)cnt; gy /= (double)cnt;
                                    ui.attrLocateSrcX = gx; ui.attrLocateSrcY = gy;
                                    ui.attrLocateRequested = true;
                                }
                            }
                        }
                        for (int cidx = 0; cidx < (int)vis.size(); cidx++) {
                            int k = vis[cidx];
                            if (k < 0) continue;
                            ImGui::TableSetColumnIndex(cidx + 1);
                            if (k < (int)row.cells.size())
                                ImGui::TextUnformatted(row.cells[k].text.c_str());
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::End();
        }
    }
    // 缓存管理窗口
    if (ui.showCacheManager) {
        if (ImGui::Begin("Cache Manager", &ui.showCacheManager)) {
            auto entries = listCacheEntries(cfg);
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
                    clearAllCache(cfg);
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
                        deleteCacheEntry(e.sourceId, e.layerIdx, cfg);
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
        ImGui::OpenPopup("Render WKT");
        if (ImGui::BeginPopupModal("Render WKT", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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