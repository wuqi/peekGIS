#pragma once
#include <map>
#include <string>
#include <vector>

struct lua_State;

// 脚本声明的输入/参数/输出项(来自 scripts/*.lua 的 toolbox.input/param/output)。
struct ScriptParam {
    std::string name;
    std::string title;        // 表单标签(未填则用 name)
    std::string filter;       // 文件对话框过滤说明(仅提示)
    std::string defValue;     // param 默认值
    bool required = false;    // input/output 必填
    bool load = false;        // output 成功后加载到地图
};

// 一个脚本工具: 预置脚本 = 工具路径 + 声明式参数 + 可选 on_run 钩子。
struct ScriptToolDef {
    std::string name;         // 文件名(去 .lua), 工具箱里用 "script:" + name 引用
    std::string label;
    std::string description;
    std::string tool;         // 对应的 gdal 工具 fullPath(如 "gdal:raster:reproject")
    std::string source;       // 脚本文件路径
    std::vector<ScriptParam> inputs;
    std::vector<ScriptParam> params;
    std::vector<ScriptParam> outputs;
    int onRunRef = -2;        // toolbox.def.on_run 的 Lua 注册表引用(LUA_NOREF=-2 表示无钩子)
};

// LuaJIT 脚本宿主: 承载工具箱 Lua 插件脚本。
// 生命周期: init() -> (runString/runFile 可多次) -> shutdown()。
class ScriptHost {
public:
    ScriptHost() = default;
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;
    ScriptHost(ScriptHost&&) = delete;
    ScriptHost& operator=(ScriptHost&&) = delete;

    // 创建 lua_State 并注册内置 API(toolbox.*)。失败返回 false(err 置原因)。
    bool init(std::string* err = nullptr);
    void shutdown();

    // 执行一段 Lua 代码 / 一个脚本文件。ok=true 表示无 Lua 错误(有返回值也算成功)。
    // 执行结果不会留在 lua 栈上。err 非空时收错误文本(不含则忽略)。
    bool runString(const std::string& code, std::string* err = nullptr);
    bool runFile(const std::string& path, std::string* err = nullptr);

    // 扫描 dir 下 *.lua: 逐个执行并收集声明为脚本工具(toolbox.def.tool 非空)的项。
    // 脚本声明驻留在宿主 Lua 状态中, on_run 引用随之有效。
    bool collectScriptTools(const std::string& dir, std::vector<ScriptToolDef>* out,
                            std::string* err = nullptr);

    // 调用某脚本的 on_run(ctx) 钩子(ctx 为参数名->值, 可在 Lua 内就地修改或返回新表)。
    // 无钩子直接成功。err 非空时收 Lua 错误。
    bool runOnRun(const ScriptToolDef& t, std::map<std::string, std::string>* ctx,
                  std::string* err = nullptr);

    lua_State* L() { return L_; }

private:
    bool runPcall(int base, std::string* err);

    // ---- toolbox.* Lua 回调(纯函数, 只依赖 lua_State) ----
    static void pushDef(lua_State* L);
    static void pushDefSub(lua_State* L, const char* key);
    static int l_toolbox_input(lua_State* L);
    static int l_toolbox_param(lua_State* L);
    static int l_toolbox_output(lua_State* L);
    static int l_toolbox_echo(lua_State* L);
    static int l_toolbox_log(lua_State* L);
    static int l_toolbox_self_check(lua_State* L);
    static void registerToolboxLib(lua_State* L);

    lua_State* L_ = nullptr;
    std::string err_;
};