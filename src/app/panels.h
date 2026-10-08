#pragma once

#include <string>

#include "data/attr_table.h"   // AttrRow(下面 locateFeatureOnMap 要用它的引用)

class MapScene;
struct UIState;

// 各面板的窗口内容。窗口的 Begin/End 与 Dock 归属仍由 renderUI 编排(见 ui.cpp);
// 这里只负责每个面板内部的渲染逻辑与状态读写。
// 各函数内部自行判断显隐条件(ui.leftPanel / ui.show*Panel / ui.rasterSettingsOpen), 可直接无条件调用。

// 图层面板(左栏): 图层列表/显隐/右键菜单/配色色块 + 打开/适配视图
void drawLayerPanel(MapScene& scene, UIState& ui);

// 栅格渲染设置弹窗(图层右键 → 栅格设置)
void drawRasterSettingsDialog(MapScene& scene, UIState& ui);

// 属性识别面板(右栏): 双击识别的要素列表/复制/编码切换
void drawIdentifyPanel(UIState& ui);

// 属性表面板(底栏): 分页表格/列选择/双击定位/复制
void drawAttrTablePanel(UIState& ui);

// 要素查询浮动窗口(按 FID/OID 回源查一个要素, 显示属性并定位高亮)
void drawFeatureQueryPanel(UIState& ui);

// ---- 面板间共用的小工具(实现在 attr_panel.cpp) ----

// 按 FID 回源读该要素几何的 WKT 并复制到剪贴板; 失败时写 ui.status
bool copyFeatureWkt(UIState& ui, const std::string& path, int layerIdx,
                    long long fid, int srcEpsg);

// 定位到要素: 拷几何到共用高亮态 + 求质心 + 置居中请求(由 main 消费)。
// srcEpsg 是 row 里几何的坐标系。属性表双击与要素查询面板共用。
void locateFeatureOnMap(UIState& ui, const peekg::data::AttrRow& row, int srcEpsg);