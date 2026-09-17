#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "toolbox/script_host.h"
#include "toolbox/tool_registry.h"

class MapScene;
struct UIState;

// 工具箱面板: 工具目录(后台一次性导入, 供脚本拼参/定位) + 脚本工具列表/表单/运行 + 输出加载。
// 状态全部内聚在本类中(取代原 ui.cpp 的画布级 static 变量与自由函数)。
// 生命周期随应用: 程序启动构造一次、每帧调用 draw(), 运行期间伴随应用结束。
class ToolboxPanel {
public:
    // 渲染整个面板(调用方需已开启 ImGui "工具箱" 窗口)。
    void draw(MapScene& scene, UIState& ui);

private:
    // ---- 工具目录(后台导入; 脚本通过 gdal:* fullPath 引用它) ----
    ToolRegistry registry_;
    std::atomic<bool> loading_{false};
    std::atomic<bool> ready_{false};
    std::string error_;

    // ---- 脚本工具列表/表单(仅 UI 线程访问) ----
    ScriptHost lua_;
    bool luaInit_ = false;
    bool scriptsLoaded_ = false;
    std::string scriptsError_;
    std::vector<ScriptToolDef> scripts_;
    char searchBuf_[128] = {};
    std::string selection_;                    // 选中项 "script:<name>"
    std::map<std::string, std::string> form_;  // 表单值(参数名 -> 文本)

    // ---- 运行结果(worker 线程写, UI 线程读; 互斥量保护) ----
    std::mutex runMtx_;
    bool running_ = false;
    int exit_ = -1;
    std::string out_, err_;
    std::vector<std::string> loadable_;        // load=true 的输出路径快照

    void ensureStarted();
    void ensureScriptsLoaded();
    void runScript(const std::string& exe, ToolRegistry reg, ScriptToolDef sd,
                   std::map<std::string, std::string> opts);
    void initScriptForm(const ScriptToolDef& s);
    const ScriptToolDef* findScript(const std::string& sel) const;
    void drawList(const std::string& query);
    void drawForm(const ScriptToolDef& s);
    void drawResult(UIState& ui);
    void fileRow(const ScriptParam& p);        // input/output: 路径 + 浏览按钮
    void paramRow(const ScriptParam& p);       // param: 文本/布尔复选框
};