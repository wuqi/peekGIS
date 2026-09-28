#include "app/panels.h"

#include "app/ui.h"
#include "data/attr_table.h"
#include "data/encoding.h"
#include "data/vector_reader.h"

#include "imgui.h"

using namespace peekg::data;

#include <cstdio>
#include <vector>

// 把一条识别要素的所有属性格式化为 "字段: 值" 逐行文本写入剪贴板
static void copyIdentifyToClipboard(const IdentifyHit& h) {
    std::string text;
    text.reserve(128);
    text += h.layerName;
    if (!h.geomType.empty()) { text += " ("; text += h.geomType; text += ")"; }
    text += "\n";
    for (const auto& a : h.attrs) {
        text += a.name;
        text += ": ";
        text += a.value;
        text += "\n";
    }
    ImGui::SetClipboardText(text.c_str());
}

// 按 FID 回源重读要素几何, 复制其 WKT(拿真实环结构, 不是界面上的扁平化缓冲)
static bool copyFeatureWkt(UIState& ui, const std::string& path, int layerIdx,
                           long long fid, int srcEpsg) {
    std::string wkt;
    if (!featureWkt(path, layerIdx, fid, wkt)) {
        ui.status = "该要素无几何或读取失败, 无法导出 WKT";
        return false;
    }
    ImGui::SetClipboardText(wkt.c_str());
    char buf[160];
    if (srcEpsg > 0)
        std::snprintf(buf, sizeof buf, "已复制 FID %lld 的 WKT (%d 字节, EPSG:%d)",
                      fid, (int)wkt.size(), srcEpsg);
    else
        std::snprintf(buf, sizeof buf, "已复制 FID %lld 的 WKT (%d 字节)", fid, (int)wkt.size());
    ui.status = buf;
    return true;
}

// 属性识别面板(右栏): 双击识别的要素列表/编码切换/复制
void drawIdentifyPanel(UIState& ui) {
    if (ImGui::Begin("属性识别")) {
        if (ui.identify.pending) {
            ImGui::TextDisabled("查询中...");
            ImGui::SameLine();
            ImGui::TextWrapped("(后台线程遍历, 大文件需数秒)");
        } else if (ui.identify.full.empty()) {
            ImGui::TextDisabled("双击地图查询要素属性");
        } else {
            if (ImGui::Button("清空结果")) { ui.identify.full.clear(); ui.identify.geo.clear(); }
            ImGui::SameLine();
            // 固定编码解释器: 用户选什么, 该面板字符串属性就按什么解码(不重查/不重读)
            if (ImGui::Combo("编码", &ui.identify.encoding, kEncodingNames, kEncodingCount)) {
                TextEncoding e = (TextEncoding)ui.identify.encoding;
                for (auto& h : ui.identify.full) h.applyEncoding(e);
                for (auto& h : ui.identify.geo) h.applyEncoding(e);
            }
            ImGui::Separator();
            for (size_t i = 0; i < ui.identify.full.size(); i++) {
                auto& h = ui.identify.full[i];
                ImGui::PushID((int)i);
                // 按钮独占一行靠左: 跟在图层名后面会被顶到右侧, 一眼扫不到
                if (ImGui::SmallButton("复制属性"))
                    copyIdentifyToClipboard(h);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("复制该要素全部属性");
                if (h.canFetchGeom()) {
                    ImGui::SameLine(0.0f, 6.0f);
                    if (ImGui::SmallButton("复制WKT"))
                        copyFeatureWkt(ui, h.srcPath, h.fileLayerIdx, h.fid, h.srcEpsg);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("复制该要素几何的 WKT(回源按 FID 读取原始环结构)");
                }
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
}

// 把属性表一行(按可见字段)格式化为 "字段: 值\tFID" 写剪贴板
static void copyAttrRowToClipboard(const AttrRow& row,
                                   const std::vector<int>& vis,
                                   const std::string& fidText) {
    std::string text;
    if (!fidText.empty()) { text += "FID: "; text += fidText; text += "\n"; }
    for (int cidx = 0; cidx < (int)vis.size(); cidx++) {
        int k = vis[cidx];
        if (k < 0 || k >= (int)row.cells.size()) continue;
        const auto& cell = row.cells[k];
        text += cell.name;
        text += ": ";
        text += cell.text;
        text += "\n";
    }
    ImGui::SetClipboardText(text.c_str());
}

// 属性表面板(底栏): 分页表格/列选择/双击定位/复制
void drawAttrTablePanel(UIState& ui) {
    if (ImGui::Begin("属性表")) {
        if (!ui.attr.isOpen || !ui.attr.info.ok) {
            ImGui::TextDisabled("在左侧图层面板 右键图层 → 打开属性表");
        } else {
            TextEncoding enc = (TextEncoding)ui.attr.encoding;

            // 当前页内选中行(顶栏"复制WKT"与表格选中态共用; 翻页时清空)
            auto selRowOf = [&]() -> const AttrRow* {
                if (ui.attr.selRow < 0) return nullptr;
                for (const auto& p : ui.attr.pages) {
                    if (p.page != ui.attr.currentPage) continue;
                    return (ui.attr.selRow < (int)p.rows.size()) ? &p.rows[ui.attr.selRow] : nullptr;
                }
                return nullptr;
            };

            // 顶栏: 图层名 + 编码下拉(固定解释, 切换只重转已缓存页)
            ImGui::TextUnformatted(ui.attr.info.layerName.c_str());
            ImGui::SameLine(0.0f, 16.0f);
            if (ui.attr.info.canEncode) {
                if (ImGui::Combo("编码", &ui.attr.encoding, kEncodingNames, kEncodingCount)) {
                    enc = (TextEncoding)ui.attr.encoding;
                    for (auto& p : ui.attr.pages) attrReencodePage(p, enc);
                }
            } else {
                ImGui::TextDisabled("(UTF-8)");
            }
            ImGui::SameLine(0.0f, 24.0f);
            if (ImGui::Button("选择列")) ImGui::OpenPopup("attr_cols");
            if (ImGui::BeginPopup("attr_cols")) {
                int nf = (int)ui.attr.info.fields.size();
                if ((int)ui.attr.showCol.size() != nf) ui.attr.showCol.assign(nf, 1);
                bool anyOn = false;
                for (int k = 0; k < nf; k++) {
                    std::string nm = decodeRawToUtf8(ui.attr.info.fields[k].rawName, enc);
                    if (nm.empty()) nm = "(字段" + std::to_string(k) + ")";
                    ImGui::Checkbox(nm.c_str(), (bool*)&ui.attr.showCol[k]);
                    if (ui.attr.showCol[k]) anyOn = true;
                }
                ImGui::Separator();
                if (ImGui::Button("全部显示")) ui.attr.showCol.assign(nf, 1);
                ImGui::SameLine();
                if (ImGui::Button("只留 FID")) ui.attr.showCol.assign(nf, 0);
                if (!anyOn) ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "至少保留一列");
                ImGui::EndPopup();
            }
            ImGui::SameLine(0.0f, 8.0f);
            {
                // 复制选中行的几何 WKT(先点一行选中, 再点此按钮; 右键行也有同一入口)
                const AttrRow* sel = selRowOf();
                ImGui::BeginDisabled(sel == nullptr);
                if (ImGui::Button("复制WKT"))
                    copyFeatureWkt(ui, ui.attr.path, ui.attr.fileLayerIdx, sel->fid, ui.attr.srcEpsg);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(sel ? "复制选中行几何的 WKT(回源按 FID 读取原始环结构)"
                                          : "先点一行选中, 再复制其 WKT");
            }
            ImGui::Separator();

            // 翻页控件(总数未取到时先渲染行, 总页数/总行数就绪后再显示)
            long long total = ui.attr.info.total;
            bool countKnown = total >= 0;
            int totalPages = 1;
            if (countKnown) {
                totalPages = (int)((total + ui.attr.rowsPerPage - 1) / ui.attr.rowsPerPage);
                if (totalPages < 1) totalPages = 1;
                if (ui.attr.currentPage >= totalPages) ui.attr.currentPage = totalPages - 1;
                if (ui.attr.currentPage < 0) ui.attr.currentPage = 0;
            }

            if (ImGui::Button("上一页") && ui.attr.currentPage > 0) {
                ui.attr.currentPage--;
                ui.attr.selRow = -1;
                ui.attr.gotoRequested = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("下一页") && (!countKnown || ui.attr.currentPage + 1 < totalPages)) {
                ui.attr.currentPage++;
                ui.attr.selRow = -1;
                ui.attr.gotoRequested = true;
            }
            ImGui::SameLine();
            if (countKnown)
                ImGui::Text("页 %d/%d    共 %lld 行", ui.attr.currentPage + 1, totalPages, total);
            else if (ui.attr.loading)
                ImGui::TextDisabled("页 %d/?    统计总数中...", ui.attr.currentPage + 1);
            else
                ImGui::TextDisabled("页 %d/?    总数未知", ui.attr.currentPage + 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            static int jumpPage = 1;
            ImGui::InputInt("跳页", &jumpPage, 0, 0);
            if (jumpPage < 1) jumpPage = 1;
            ImGui::SameLine();
            if (ImGui::Button("跳转") && jumpPage - 1 != ui.attr.currentPage) {
                ui.attr.currentPage = jumpPage - 1;
                ui.attr.selRow = -1;
                ui.attr.gotoRequested = true;
            }
            if (ui.attr.loading) { ImGui::SameLine(); ImGui::TextDisabled("加载中..."); }
            ImGui::Separator();

            // 表格: FID 列 + 各字段列(可选列, 见顶部"选择列")
            const AttrPageData* cur = nullptr;
            for (const auto& p : ui.attr.pages)
                if (p.page == ui.attr.currentPage) { cur = &p; break; }
            int nField = (int)ui.attr.info.fields.size();
            if ((int)ui.attr.showCol.size() != nField) ui.attr.showCol.assign(nField, 1);
            std::vector<int> vis;              // 可见字段的"新列序号"
            vis.reserve(nField);
            for (int k = 0; k < nField; k++) if (ui.attr.showCol[k]) vis.push_back(k);
            if (vis.empty()) vis.push_back(-1);  // 占位, 至少保留 FID
            int nCol = 1 + (int)vis.size();

            if (ImGui::BeginTable("attrtable", nCol,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("FID", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                for (int idx : vis) {
                    if (idx < 0) continue;
                    ImGui::TableSetupColumn(decodeRawToUtf8(ui.attr.info.fields[idx].rawName, enc).c_str(),
                                            ImGuiTableColumnFlags_WidthStretch);
                }
                ImGui::TableHeadersRow();

                // 定位某行: 拷贝几何(源 CRS) + 求质心, 交由 main 居中+高亮
                auto locateRow = [&](const AttrRow& row) {
                    if (!row.hasGeom) return;
                    ui.attr.hlOutline = row.outline;
                    ui.attr.hlPoints = row.points;
                    ui.attr.hlTris = row.fillTris;
                    ui.attr.hlSrcEpsg = ui.attr.srcEpsg;
                    ui.attr.hlActive = true;
                    const std::vector<float>* g = nullptr;
                    if (!row.points.empty()) g = &row.points;
                    else if (!row.outline.empty()) g = &row.outline;
                    else if (!row.fillTris.empty()) g = &row.fillTris;
                    if (!g || g->empty()) return;
                    size_t cnt = g->size() / 2;
                    double gx = 0, gy = 0;
                    for (size_t i = 0; i + 1 < g->size(); i += 2) { gx += (*g)[i]; gy += (*g)[i + 1]; }
                    gx /= (double)cnt; gy /= (double)cnt;
                    ui.attr.locateSrcX = gx; ui.attr.locateSrcY = gy;
                    ui.attr.locateRequested = true;
                };

                if (cur) {
                    for (size_t ri = 0; ri < cur->rows.size(); ri++) {
                        const AttrRow& row = cur->rows[ri];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        char fidbuf[32];
                        snprintf(fidbuf, sizeof(fidbuf), "%lld", (long long)row.fid);
                        // FID 格横跨整行: 点击复制该行全部可见字段并选中, 双击定位, 右键更多
                        ImGui::PushID((int)ri);
                        bool rowSelected = ImGui::Selectable(fidbuf, ui.attr.selRow == (int)ri,
                                                            ImGuiSelectableFlags_SpanAllColumns);
                        if (rowSelected) {
                            ui.attr.selRow = (int)ri;
                            copyAttrRowToClipboard(row, vis, fidbuf);
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("点击复制该行并选中; 双击定位到地图; 右键可复制 WKT");
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            ui.attr.selRow = (int)ri;
                            locateRow(row);
                        }
                        if (ImGui::BeginPopupContextItem("attrrowctx", ImGuiPopupFlags_MouseButtonRight)) {
                            if (ImGui::MenuItem("复制该行属性"))
                                copyAttrRowToClipboard(row, vis, fidbuf);
                            if (ImGui::MenuItem("复制几何 WKT"))
                                copyFeatureWkt(ui, ui.attr.path, ui.attr.fileLayerIdx, row.fid, ui.attr.srcEpsg);
                            if (ImGui::MenuItem("定位到地图", nullptr, false, row.hasGeom))
                                locateRow(row);
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
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
        }
    }
    ImGui::End();
}