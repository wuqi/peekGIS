#include "app/panels.h"

#include "app/ui.h"
#include "data/attr_table.h"
#include "data/encoding.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

using namespace peekg::data;

// 要素查询(浮动窗口): 按 FID/OID 回源查一个要素 -> 显示属性 + 定位高亮。
//
// 为什么走"回源"而不是查 v2 缓存: 缓存只服务渲染, VtRing/VtTile 都不带 FID
// (见 src/vt/vt_types.h 头注释), 按 FID 取几何只能走 OGR。OGR_L_GetFeature 直接按
// FID 定位不做全表扫描, 通常毫秒级; 大表也是这个成本(与属性表翻页同源)。
//
// 只做"一个要素"是刻意的: 属性条件查询要设 OGR_L_SetAttributeFilter, 而属性表分页读
// 复用同一个 per-path 共享句柄, 一设过滤就会把属性表正在读的图层污染掉。要做属性条件
// 查询必须先复制一份"按私有连接走"的查询通道(像 attrFeatureCount 那样绕开共享锁)。
void drawFeatureQueryPanel(UIState& ui) {
    if (!ImGui::Begin("要素查询", &ui.query.open)) { ImGui::End(); return; }

    if (ui.query.bindLayer < 0 || ui.query.path.empty()) {
        ImGui::TextDisabled("在左侧图层面板 右键图层 → 要素查询");
        ImGui::End();
        return;
    }

    TextEncoding enc = (TextEncoding)ui.query.encoding;

    // ---- 顶部: 图层名 + 源 CRS + 编码下拉 ----
    ImGui::TextUnformatted(ui.query.info.layerName.empty() ? "(读取中...)"
                                                            : ui.query.info.layerName.c_str());
    if (ui.query.srcEpsg > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("EPSG:%d", ui.query.srcEpsg);
    }
    ImGui::SameLine(0.0f, 16.0f);
    if (ui.query.info.canEncode) {
        if (ImGui::Combo("编码", &ui.query.encoding, kEncodingNames, kEncodingCount)) {
            enc = (TextEncoding)ui.query.encoding;
            // 固定编码解释器: 切换只重转已有结果, 不重读文件(与属性表同一语义)
            if (!ui.query.row.cells.empty()) {
                AttrPageData tmp;
                tmp.rows.push_back(ui.query.row);
                attrReencodePage(tmp, enc);
                ui.query.row = std::move(tmp.rows[0]);
            }
        }
    } else if (!ui.query.info.layerName.empty()) {
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("(UTF-8)");
    }
    ImGui::Separator();

    // ---- FID 输入 + 查询按钮 ----
    ImGui::SetNextItemWidth(140.0f);
    bool submit = ImGui::InputText("##fidin", ui.query.fidBuf, sizeof(ui.query.fidBuf),
                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    ImGui::BeginDisabled(ui.query.busy || !ui.query.infoOk);
    if (ImGui::Button("查询", ImVec2(70, 0)) || submit) ui.query.searchRequested = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && !ui.query.infoOk)
        ImGui::SetTooltip("正在读取该图层的字段定义, 稍候");
    else if (ui.query.busy && ImGui::IsItemHovered())
        ImGui::SetTooltip("查询中...");
    else
        ImGui::SameLine();
    if (ui.query.busy || ui.query.pending) {
        ImGui::SameLine();
        ImGui::TextDisabled("查询中...");
    }

    // ---- 结果状态 ----
    switch (ui.query.statusCode) {
        case 2: ImGui::TextColored(ImVec4(1, 0.65f, 0.2f, 1), "未找到该 FID 的要素"); break;
        case 3: ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "读取失败(图层打不开或字段无效)"); break;
        case 4: ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "FID 必须是整数"); break;
        default: break;
    }

    if (ui.query.statusCode != 1) { ImGui::End(); return; }

    // ---- 结果: FID + 操作按钮 ----
    ImGui::Text("FID %lld", (long long)ui.query.row.fid);
    if (!ui.query.row.hasGeom) ImGui::SameLine(), ImGui::TextDisabled("(无几何, 无法定位)");
    ImGui::BeginDisabled(!ui.query.row.hasGeom);
    if (ImGui::SmallButton("定位到地图")) locateFeatureOnMap(ui, ui.query.row, ui.query.srcEpsg);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("平移到该要素并高亮(仅居中, 不改缩放)");
    ImGui::SameLine(0.0f, 6.0f);
    if (ImGui::SmallButton("复制属性")) {
        std::string text = "FID: " + std::to_string((long long)ui.query.row.fid) + "\n";
        for (const auto& c : ui.query.row.cells) {
            text += c.name;
            text += ": ";
            text += c.text;
            text += "\n";
        }
        ImGui::SetClipboardText(text.c_str());
        ui.status = "已复制该要素的属性";
        ui.statusErr = false;
    }
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::BeginDisabled(!ui.query.row.hasGeom);
    if (ImGui::SmallButton("复制WKT"))
        copyFeatureWkt(ui, ui.query.path, ui.query.fileLayerIdx, ui.query.row.fid, ui.query.srcEpsg);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("回源按 FID 读取原始环结构的 WKT");
    // 取消高亮: 高亮态是属性表/查询面板共用的(ui.hl), 不主动清就一直画到下一次定位为止
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::BeginDisabled(!ui.hl.active);
    if (ImGui::SmallButton("取消高亮")) ui.hl.active = false;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && ui.hl.active)
        ImGui::SetTooltip("清除地图上的绿色高亮(属性表双击定位留下的高亮也一并清掉)");
    ImGui::Separator();

    // ---- 属性表(两列: 字段 / 值), 与属性表同一套编码显示 ----
    if (ImGui::BeginTable("queryattrs", 2,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp,
        ImVec2(0, -1))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("字段", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t k = 0; k < ui.query.row.cells.size(); k++) {
            const auto& c = ui.query.row.cells[k];
            ImGui::PushID((int)k);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(c.name.empty() ? "(无名字段)" : c.name.c_str());
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Selectable(c.text.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick))
                ImGui::SetClipboardText(c.text.c_str());   // 双击复制该值
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}