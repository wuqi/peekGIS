#include "app/toolbox_panel.h"

#include "app/ui.h"
#include "platform/exe_path.h"
#include "platform/file_dialog.h"
#include "platform/path_util.h"
#include "toolbox/gdal_proc.h"
#include "util/logger.h"

#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

// 首次进入面板时后台导入工具目录(只做一次, 失败可重试)。
// 成功后在主线程把整个注册表收编为成员(ready_ 原子保证可见性)。
void ToolboxPanel::ensureStarted() {
    if (ready_ || loading_) return;
    loading_ = true;
    std::thread([this]() {
        ToolRegistry r;
        bool ok = r.ensureLoaded();
        if (ok && !ready_) {
            registry_ = std::move(r);
            ready_ = true;
        } else if (!ok) {
            error_ = r.lastError();
        }
        loading_ = false;
    }).detach();
}

// 脚本工具首次进入面板时加载一次(scandir 级别的速度, 同步做即可)。
void ToolboxPanel::ensureScriptsLoaded() {
    if (scriptsLoaded_) return;
    scriptsLoaded_ = true;
    if (!luaInit_) {
        luaInit_ = lua_.init();
        if (!luaInit_) scriptsError_ = "脚本宿主初始化失败";
    }
    if (luaInit_ && scriptsError_.empty()) {
        std::string dir = exeDir();
        if (!dir.empty()) dir += "/scripts";
        if (!fs::exists(toFsPath(dir))) {
            scriptsError_ = "scripts/ 目录不存在";
        } else {
            std::string serr;
            if (!lua_.collectScriptTools(dir, &scripts_, &serr))
                scriptsError_ = serr;
        }
    }
}

// 后台线程: 脚本运行 = 先调 on_run 钩子(可改参数), 再跑 gdal 子进程,
// 成功且 outputs 带 load=true 时记录可加载路径。
void ToolboxPanel::runScript(const std::string& exe, ToolRegistry reg, ScriptToolDef sd,
                             std::map<std::string, std::string> opts) {
    std::string luaErr;
    lua_.runOnRun(sd, &opts, &luaErr);
    std::string out, eout, err;
    std::vector<std::string> argv;
    int code = -1;
    const ToolDef* td = reg.find(sd.tool);
    if (!td) {
        eout = "脚本引用的工具不存在: " + sd.tool;
    } else if (!reg.runArgs(*td, opts, &argv, &err)) {
        eout = err;
    } else {
        GdalCli::run(exe, argv, &out, &eout, &code);
    }
    std::vector<std::string> loadable;
    if (code == 0) {
        for (const ScriptParam& p : sd.outputs) {
            if (!p.load) continue;
            auto it = opts.find(p.name);
            if (it != opts.end() && fs::exists(toFsPath(it->second))) loadable.push_back(it->second);
        }
    }
    std::lock_guard<std::mutex> g(runMtx_);
    exit_ = code;
    out_ = std::move(out);
    err_ = std::move(eout);
    loadable_ = std::move(loadable);
    running_ = false;
    if (!luaErr.empty()) spdlog::warn("[lua] on_run: {}", luaErr);
}

// 选中脚本工具时初始化表单缺项(param 用默认值, input/output 留空)
void ToolboxPanel::initScriptForm(const ScriptToolDef& s) {
    for (const auto& p : s.params)
        if (!form_.count(p.name)) form_[p.name] = p.defValue;
    for (const auto& p : s.inputs)
        if (!form_.count(p.name)) form_[p.name] = "";
    for (const auto& p : s.outputs)
        if (!form_.count(p.name)) form_[p.name] = "";
}

// 按 "script:<name>" 反查脚本工具定义
const ScriptToolDef* ToolboxPanel::findScript(const std::string& sel) const {
    std::string name = (sel.rfind("script:", 0) == 0) ? sel.substr(7) : sel;
    for (const auto& s : scripts_)
        if (s.name == name) return &s;
    return nullptr;
}

// 脚本工具展示列表(支持小写搜索)
void ToolboxPanel::drawList(const std::string& query) {
    ImGui::BeginChild("##tboxList", ImVec2(0, 200), ImGuiChildFlags_Borders);
    if (scripts_.empty()) {
        ImGui::TextDisabled("(脚本:%s)", scriptsError_.empty() ? "暂无" : scriptsError_.c_str());
    } else if (query.empty()) {
        for (const auto& s : scripts_) {
            std::string id = "script:" + s.name;
            bool sel = (selection_ == id);
            if (ImGui::Selectable(s.label.c_str(), sel)) {
                selection_ = id;
                initScriptForm(s);
            }
            if (ImGui::IsItemHovered() && !s.description.empty())
                ImGui::SetTooltip("%s", s.description.c_str());
        }
    } else {
        int nMatch = 0;
        for (const auto& s : scripts_) {
            std::string name = s.label.empty() ? s.name : s.label;
            for (auto& c : name) c = (char)tolower((unsigned char)c);
            if (name.find(query) == std::string::npos) continue;
            std::string id = "script:" + s.name;
            bool sel = (selection_ == id);
            if (ImGui::Selectable(s.label.c_str(), sel)) {
                selection_ = id;
                initScriptForm(s);
            }
            nMatch++;
        }
        if (nMatch == 0) ImGui::TextDisabled("(无匹配工具)");
    }
    ImGui::EndChild();
}

// input/output 行: 文件路径 + 浏览按钮
void ToolboxPanel::fileRow(const ScriptParam& p) {
    std::string& v = form_[p.name];
    std::string id = "##arg_" + p.name;
    std::string label = p.title.empty() ? p.name : p.title;
    if (p.required) label += " *";
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine();
    if (ImGui::Button("浏览...")) {
        std::string fp = openFileDialog();
        if (!fp.empty()) v = fp;
    }
    ImGui::SameLine();
    char buf[1024];
    snprintf(buf, sizeof(buf), "%.990s", v.c_str());
    ImGui::InputText(id.c_str(), buf, sizeof(buf));
    v = buf;
}

// param 行: 布尔值渲染为复选框, 其余为文本输入
void ToolboxPanel::paramRow(const ScriptParam& p) {
    std::string& v = form_[p.name];
    std::string id = "##arg_" + p.name;
    std::string label = p.title.empty() ? p.name : p.title;
    if (v == "true" || v == "false") {
        bool on = (v == "true");
        if (ImGui::Checkbox(label.c_str(), &on)) v = on ? "true" : "false";
    } else {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%.990s", v.c_str());
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine();
        ImGui::InputText(id.c_str(), buf, sizeof(buf));
        v = buf;
        if (ImGui::IsItemHovered() && !p.filter.empty())
            ImGui::SetTooltip("%s", p.filter.c_str());
    }
}

// 参数表单 + 运行按钮
void ToolboxPanel::drawForm(const ScriptToolDef& s) {
    ImGui::Separator();
    ImGui::TextWrapped("%s", s.description.empty() ? s.label.c_str() : s.description.c_str());
    ImGui::TextDisabled("来源: %s", s.tool.c_str());
    ImGui::Separator();

    bool missingReq = false;
    for (const auto& p : s.inputs)  if (p.required && form_[p.name].empty()) missingReq = true;
    for (const auto& p : s.outputs) if (p.required && form_[p.name].empty()) missingReq = true;
    for (const auto& p : s.inputs) fileRow(p);
    for (const auto& p : s.params) {
        auto& v = form_[p.name];
        if (v.empty()) v = p.defValue;
        paramRow(p);
    }
    for (const auto& p : s.outputs) fileRow(p);

    bool running = false;
    {
        std::lock_guard<std::mutex> g(runMtx_);
        running = running_;
    }
    if (missingReq) ImGui::TextDisabled("必填数据集未指定。");
    if (ImGui::Button(running ? "运行中…" : "运行", ImVec2(-1.0f, 0)) && !running && !missingReq) {
        std::map<std::string, std::string> opts;
        auto collect = [&](const std::vector<ScriptParam>& ps) {
            for (const auto& p : ps)
                if (auto it = form_.find(p.name); it != form_.end() && !it->second.empty()) opts[p.name] = it->second;
        };
        collect(s.inputs);
        collect(s.params);
        collect(s.outputs);
        std::vector<std::string> argv;
        std::string err;
        bool ok = false;
        const ToolDef* td = registry_.find(s.tool);
        if (!td) {
            err = "脚本引用的工具不存在: " + s.tool;
        } else {
            ok = registry_.runArgs(*td, opts, &argv, &err);
        }
        if (!ok) {
            ImGui::TextWrapped("%s", err.c_str());
        } else {
            std::string exe = registry_.gdalExe();
            {
                std::lock_guard<std::mutex> g(runMtx_);
                running_ = true;
                exit_ = -1;
                out_.clear();
                err_.clear();
                loadable_.clear();
            }
            std::thread(&ToolboxPanel::runScript, this, exe, registry_, s, std::move(opts)).detach();
        }
    }
}

// 运行结果 + 输出加载
void ToolboxPanel::drawResult(UIState& ui) {
    bool running = false;
    int code = -1;
    {
        std::lock_guard<std::mutex> g(runMtx_);
        running = running_;
        code = exit_;
    }
    if (running) {
        ImGui::TextWrapped("运行中…(后台, 大文件需数秒)");
    } else if (code >= 0) {
        std::string out, err;
        {
            std::lock_guard<std::mutex> g(runMtx_);
            code = exit_;
            out = out_;
            err = err_;
        }
        ImGui::TextUnformatted(code == 0 ? "执行成功" : "执行失败");
        ImGui::SameLine();
        ImGui::Text("退出码 %d", code);
        if (!err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.5f, 0.4f, 1));
            ImGui::TextWrapped("%s", err.c_str());
            ImGui::PopStyleColor();
        }
        if (!out.empty()) ImGui::TextWrapped("%s", out.c_str());
        if (code == 0) {
            std::vector<std::string> loadable;
            {
                std::lock_guard<std::mutex> g(runMtx_);
                loadable = loadable_;
            }
            for (const std::string& p : loadable) {
                std::string lbl = "加载输出到地图: " + std::string(fs::path(p).filename().string());
                if (ImGui::Button(lbl.c_str())) {
                    ui.openPaths.push_back(p);
                    ui.openRequested = true;
                }
            }
            if (loadable.empty()) ImGui::TextDisabled("(无标记为可加载的输出)");
        }
    }
}

void ToolboxPanel::draw(MapScene& scene, UIState& ui) {
    (void)scene;
    ensureStarted();

    ImGui::TextUnformatted("工具箱");
    ImGui::Separator();

    if (loading_) {
        ImGui::TextWrapped("正在导入工具目录(gdal --json-usage)…");
        return;
    }
    if (!ready_) {
        ImGui::TextWrapped("工具目录不可用: %s", error_.empty() ? "未知错误" : error_.c_str());
        return;
    }

    ensureScriptsLoaded();

    // 搜索框
    ImGui::TextUnformatted("工具:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##tboxSearch", searchBuf_, sizeof(searchBuf_));

    std::string q = searchBuf_;
    for (auto& c : q) c = (char)tolower((unsigned char)c);
    ImGui::Separator();

    drawList(q);

    // 参数表单(脚本工具)
    const ScriptToolDef* ssel = findScript(selection_);
    if (!ssel) {
        ImGui::Separator();
        ImGui::TextDisabled("选择上方工具后在此填写参数。");
        return;
    }
    drawForm(*ssel);
    drawResult(ui);

    // 脚本目录(预置/用户 .lua)
    if (ImGui::CollapsingHeader("脚本")) {
        std::string dir = exeDir();
        if (!dir.empty()) dir += "/scripts";
        int n = 0;
        if (!dir.empty() && fs::exists(dir)) {
            for (const auto& e : fs::directory_iterator(dir)) {
                if (!e.is_regular_file()) continue;
                if (e.path().extension().string() != ".lua") continue;
                ImGui::BulletText("%s", e.path().filename().string().c_str());
                n++;
            }
        }
        if (n == 0) ImGui::TextDisabled("(scripts/ 目录无 .lua 脚本)");
    }
}