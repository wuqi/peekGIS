#include "app/panels.h"

#include "app/ui.h"
#include "data/spatial_index.h"
#include "platform/file_dialog.h"

#include "imgui.h"

using namespace peekg::data;

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

// ---- 空间索引能力位缓存 ----
// 判定"该矢量源有没有空间索引"要开一次源文件, 不能每帧做(否则每次右键都卡一下)。
// 菜单只在弹出时才需要该值, 故按 (路径, 源图层) 缓存; 建完索引后清掉对应条目。
struct QixProbeKey {
    std::string path;
    int layer = 0;
    bool operator<(const QixProbeKey& o) const {
        if (path != o.path) return path < o.path;
        return layer < o.layer;
    }
};

static std::map<QixProbeKey, bool>& qixProbeCache() {
    static std::map<QixProbeKey, bool> c;
    return c;
}

static bool layerHasSpatialIndex(const MapLayer& l) {
    if (l.kind != LayerKind::Vector || l.sourcePath.empty()) return true;   // 无从判定, 不显示菜单
    const std::string& p = l.sourcePath;
    auto lowerEndsWith = [&p](const char* ext) {
        size_t n = std::strlen(ext);
        if (p.size() <= n) return false;
        for (size_t i = 0; i < n; i++) {
            char c = (char)std::tolower((unsigned char)p[p.size() - n + i]);
            if (c != ext[i]) return false;
        }
        return true;
    };
    // 只对支持原地建索引的源类型显示菜单: .shp / .gpkg / PG 连接串(docs/空间索引.md)
    bool supported = p.rfind("postgresql://", 0) == 0 || p.rfind("PG:", 0) == 0 ||
                     lowerEndsWith(".shp") || lowerEndsWith(".gpkg");
    if (!supported) return true;   // fgb/vtk 等: 不显示
    auto& cache = qixProbeCache();
    QixProbeKey k{p, l.sourceLayerIdx};
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    bool has = peekg::data::vectorHasSpatialIndex(p, l.sourceLayerIdx);
    cache[k] = has;
    return has;
}

// 建完索引后让缓存失效, 下次右键重新探测(供 app.cpp 调用; 声明见 ui.h)
void invalidateSpatialIndexProbe(const MapLayer& l) {
    qixProbeCache().erase(QixProbeKey{l.sourcePath, l.sourceLayerIdx});
}

// 栅格图层无属性表: 右键 → 栅格元数据, 填充到右侧 Attributes 面板
static void showRasterMetadata(UIState& ui, const MapLayer& l) {
    ui.identify.full.clear();
    ui.identify.geo.clear();
    ui.identify.pending = false;
    const RasterData& r = l.raster;
    IdentifyHit h;
    h.layerName = l.info.name;
    h.geomType = "RASTER";
    auto add = [&h](const char* name, const std::string& value) {
        IdentifyAttr a;
        a.name = name;
        a.value = value;
        h.attrs.push_back(std::move(a));
    };
    add("坐标系", r.sourceCrs);
    if (r.srcEpsg) add("EPSG", std::to_string(r.srcEpsg));
    add("尺寸", std::to_string(r.width) + " × " + std::to_string(r.height));
    add("波段数", std::to_string(r.bandCount));
    char buf[128];
    if (r.hasDispExtent)
        std::snprintf(buf, sizeof buf, "%.3f, %.3f ~ %.3f, %.3f",
                      r.dispMinx, r.dispMiny, r.dispMaxx, r.dispMaxy);
    else
        std::snprintf(buf, sizeof buf, "%.3f, %.3f ~ %.3f, %.3f",
                      r.minx, r.miny, r.maxx, r.maxy);
    add("范围", buf);
    add("像素", std::to_string(r.bands.empty() ? 0 : (int)std::lround(std::fabs(r.geo[1]))));
    add("金字塔", r.hasRasterPyramids ? "有" : "无");
    for (const auto& b : r.bands) {
        std::string name = "波段" + std::to_string(b.index) + " " + b.name;
        std::string val = b.hasMinMax ? "min=" + std::to_string(b.min) + " max=" + std::to_string(b.max)
                                      : "";
        if (b.isColorTable) val += (val.empty() ? "" : ", ") + std::string("调色板");
        add(name.c_str(), val.empty() ? "-" : val);
    }
    ui.identify.full.push_back(std::move(h));
}

// 图层面板(左栏): 图层列表 + 显隐 + 右键菜单 + 配色。窗口归属在 renderUI。
void drawLayerPanel(MapScene& scene, UIState& ui) {
    if (ImGui::Button("打开...")) {
        std::string p = openFileDialog();
        if (!p.empty()) { ui.openPaths.push_back(p); ui.openRequested = true; }
    }
    ImGui::SameLine();
    if (ImGui::Button("适配视图")) {
        scene.fitToView(scene.view.vpW, scene.view.vpH);
        ui.viewTouched = true;
    }
    ImGui::Separator();
    if (scene.layers.empty()) {
        ImGui::Text("(暂无图层)");
    }
    for (size_t li = 0; li < scene.layers.size(); li++) {
        auto& l = scene.layers[li];
        ImGui::PushID((int)li);
        // 颜色色块(仅矢量图层: 点击展开编辑器, 含 alpha 即面填充透明度; 栅格无意义, 不显示)
        if (l.kind == LayerKind::Vector) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(l.color[0], l.color[1], l.color[2], l.color[3]));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(l.color[0]*0.8f, l.color[1]*0.8f, l.color[2]*0.8f, l.color[3]));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(l.color[0]*0.6f, l.color[1]*0.6f, l.color[2]*0.6f, l.color[3]));
            if (ImGui::Button("##c", ImVec2(18, 18)))
                ImGui::OpenPopup("colorpick");
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
        } else {
            // 栅格无颜色块: 占位与矢量色块等宽, 保证勾选框与名称列对齐
            ImGui::Dummy(ImVec2(18, 18));
            ImGui::SameLine();
        }
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
            if (l.kind == LayerKind::Raster && ImGui::MenuItem("栅格设置")) {
                ui.rasterSettingsLayer = (int)li;
                ui.rasterSettingsOpen = true;
            }
            if (l.kind == LayerKind::Vector) {
                if (ImGui::MenuItem("打开属性表")) {
                    ui.attr.openLayerIdx = (int)li;
                    ui.attr.openRequested = true;
                }
                // 要素查询(按 FID/OID 回源; 浮窗, 与属性表各自独立打开)
                if (ImGui::MenuItem("要素查询...")) {
                    ui.query.openLayerIdx = (int)li;
                    ui.query.openRequested = true;
                    ui.query.open = true;
                }
                // 仅对"无空间索引"的矢量图层显示: vt 超 Lmax 直读靠空间过滤,
                // 无索引的大表会被直接判死(vt_build.cpp RawRegionStream::open)。
                if (!l.sourcePath.empty() && !layerHasSpatialIndex(l)) {
                    ImGui::Separator();
                    if (ui.qixBuildBusy) {
                        ImGui::MenuItem("新建空间索引中...", nullptr, false, false);
                    } else if (ImGui::MenuItem("新建空间索引")) {
                        ui.qixBuildRequested = true;
                        ui.qixBuildLayerIdx = (int)li;
                    }
                }
            } else if (ImGui::MenuItem("栅格元数据")) {
                showRasterMetadata(ui, l);
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

// 栅格渲染设置弹窗(图层右键 → 栅格设置): 渲染模式/波段/拉伸/色带
void drawRasterSettingsDialog(MapScene& scene, UIState& ui) {
    if (!ui.rasterSettingsOpen || ui.rasterSettingsLayer < 0 ||
        ui.rasterSettingsLayer >= (int)scene.layers.size() ||
        scene.layers[ui.rasterSettingsLayer].kind != LayerKind::Raster)
        return;
    MapLayer& rl = scene.layers[ui.rasterSettingsLayer];
    RasterRenderOptions& ro = rl.rastOpts;
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    bool visible = true;
    if (ImGui::Begin("栅格设置", &visible, ImGuiWindowFlags_NoCollapse)) {
        if (!visible) ui.rasterSettingsOpen = false;
        ImGui::TextUnformatted(rl.info.name.c_str());
        ImGui::Separator();

        const int nBand = std::max(1, rl.raster.bandCount);
        const char* kModes[] = {"单波段灰度拉伸", "伪彩色", "多波段 RGB"};
        if (ImGui::Combo("渲染模式", (int*)&ro.mode, kModes, 3)) ui.rasterSettingsDirty = true;
        ImGui::TextDisabled("波段数: %d", nBand);

        if (ro.mode == RasterRenderMode::RGB) {
            bool c1 = ImGui::SliderInt("R 波段", &ro.rBand, 1, nBand);
            bool c2 = ImGui::SliderInt("G 波段", &ro.gBand, 1, nBand);
            bool c3 = ImGui::SliderInt("B 波段", &ro.bBand, 1, nBand);
            if (c1 || c2 || c3) ui.rasterSettingsDirty = true;
        } else {
            bool c1 = ImGui::SliderInt("灰度波段", &ro.grayBand, 1, nBand);
            if (c1) ui.rasterSettingsDirty = true;
            if (ro.mode == RasterRenderMode::Pseudocolor) {
                const char* kMaps[] = {"灰度", "Viridis", "Jet", "Turbo"};
                if (ImGui::Combo("色带", &ro.colorMap, kMaps, 4)) ui.rasterSettingsDirty = true;
            }
            // 拉伸范围
            bool autoMM = ImGui::Checkbox("自动拉伸 min/max", &ro.useAutoMinMax);
            if (autoMM) ui.rasterSettingsDirty = true;
            if (!ro.useAutoMinMax) {
                bool cmin = ImGui::InputDouble("拉伸 min", &ro.minRaw);
                bool cmax = ImGui::InputDouble("拉伸 max", &ro.maxRaw);
                if (cmin || cmax) ui.rasterSettingsDirty = true;
            }
            bool cg = ImGui::InputDouble("Gamma", &ro.gamma, 0.1, 0.5);
            if (cg) ui.rasterSettingsDirty = true;
        }

        ImGui::Separator();
        if (ImGui::Button("应用并重渲染")) {
            ui.rasterSettingsDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("关闭")) ui.rasterSettingsOpen = false;
    }
    ImGui::End();
}