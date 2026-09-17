#include "toolbox/script_host.h"

#include "platform/path_util.h"
#include "util/logger.h"

#include <lua.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

// ---- toolbox.* Lua API ----

// 确保 toolbox 与 toolbox.def 表存在, 返回后栈顶为 toolbox, def 置 -1。
void ScriptHost::pushDef(lua_State* L) {
    lua_getglobal(L, "toolbox");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "toolbox");
    }
    lua_getfield(L, -1, "def");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "def");
    }
}

// 取 def 的子表(不存在则建), 压栈返回。
void ScriptHost::pushDefSub(lua_State* L, const char* key) {
    pushDef(L);                  // [toolbox, def]
    lua_getfield(L, -1, key);    // [toolbox, def, sub]
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, key);
    }
}

// toolbox.input(name [, spec]): 声明输入数据集。spec: {title=, filter=, required=}
int ScriptHost::l_toolbox_input(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (lua_isnil(L, 2)) lua_newtable(L);   // 无 spec 也存空表(保留「声明了」这一事实)
    const int specIdx = lua_gettop(L);
    pushDefSub(L, "inputs");                 // [.., spec, toolbox, def, inputs]
    lua_pushvalue(L, 1);                     // key=name 先入栈...
    lua_pushvalue(L, specIdx);               // ...value=spec 在栈顶(lua_settable 取顶为 v, 次顶为 k)
    lua_settable(L, -3);                     // inputs[name]=spec
    lua_settop(L, 0);
    return 0;
}

// toolbox.param(name, default [, spec]): 声明一个字符串参数。spec: {title=}
int ScriptHost::l_toolbox_param(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* def = luaL_checkstring(L, 2);
    if (lua_isnil(L, 3)) lua_newtable(L);
    const int specIdx = lua_gettop(L);
    lua_pushstring(L, "default");
    lua_pushstring(L, def);
    lua_settable(L, specIdx);                // spec.default = def
    pushDefSub(L, "params");                 // [.., spec, toolbox, def, params]
    lua_pushvalue(L, 1);
    lua_pushvalue(L, specIdx);
    lua_settable(L, -3);                     // params[name]=spec
    lua_settop(L, 0);
    return 0;
}

// toolbox.output(name [, spec]): 声明输出数据集。spec: {title=, filter=, load=}
int ScriptHost::l_toolbox_output(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (lua_isnil(L, 2)) lua_newtable(L);
    const int specIdx = lua_gettop(L);
    pushDefSub(L, "outputs");                // [.., spec, toolbox, def, outputs]
    lua_pushvalue(L, 1);
    lua_pushvalue(L, specIdx);
    lua_settable(L, -3);                     // outputs[name]=spec
    lua_settop(L, 0);
    return 0;
}

// toolbox.echo(msg): 写一条 info 日志(无换行拼接, 原样透传)。
int ScriptHost::l_toolbox_echo(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    spdlog::info("[lua] {}", std::string(s, len));
    return 0;
}

// toolbox.log(level, msg): level 为 "debug"/"info"/"warn"/"error"(未知按 info)。
int ScriptHost::l_toolbox_log(lua_State* L) {
    const char* lvl = luaL_checkstring(L, 1);
    size_t len = 0;
    const char* s = luaL_checklstring(L, 2, &len);
    std::string msg(s, len);
    if (std::strcmp(lvl, "debug") == 0) spdlog::debug("[lua] {}", msg);
    else if (std::strcmp(lvl, "warn") == 0) spdlog::warn("[lua] {}", msg);
    else if (std::strcmp(lvl, "error") == 0) spdlog::error("[lua] {}", msg);
    else spdlog::info("[lua] {}", msg);
    return 0;
}

// toolbox.self_check(): 返回 LuaJIT 版本字符串(自检用)。
int ScriptHost::l_toolbox_self_check(lua_State* L) {
    lua_getglobal(L, "jit");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "version");
        if (lua_isstring(L, -1)) return 1;  // jit.version 即 "LuaJIT x.y.z..."
    }
    lua_pushstring(L, LUA_VERSION);
    return 1;
}

void ScriptHost::registerToolboxLib(lua_State* L) {
    lua_createtable(L, 0, 8);                // toolbox
    lua_pushcfunction(L, l_toolbox_input);
    lua_setfield(L, -2, "input");
    lua_pushcfunction(L, l_toolbox_param);
    lua_setfield(L, -2, "param");
    lua_pushcfunction(L, l_toolbox_output);
    lua_setfield(L, -2, "output");
    lua_pushcfunction(L, l_toolbox_echo);
    lua_setfield(L, -2, "echo");
    lua_pushcfunction(L, l_toolbox_log);
    lua_setfield(L, -2, "log");
    lua_pushcfunction(L, l_toolbox_self_check);
    lua_setfield(L, -2, "self_check");
    lua_setglobal(L, "toolbox");
}

// ---- ScriptHost ----

ScriptHost::~ScriptHost() { shutdown(); }

bool ScriptHost::init(std::string* err) {
    if (L_) return true;
    L_ = luaL_newstate();
    if (!L_) {
        if (err) *err = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(L_);
    registerToolboxLib(L_);
    return true;
}

void ScriptHost::shutdown() {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
}

bool ScriptHost::runString(const std::string& code, std::string* err) {
    if (!L_) { if (err) *err = "host not initialized"; return false; }
    const int base = lua_gettop(L_);
    if (luaL_loadstring(L_, code.c_str()) != 0) {
        err_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "load error";
        lua_settop(L_, base);
        if (err) *err = err_;
        return false;
    }
    return runPcall(base, err);
}

bool ScriptHost::runFile(const std::string& path, std::string* err) {
    if (!L_) { if (err) *err = "host not initialized"; return false; }
    const int base = lua_gettop(L_);
    if (luaL_loadfile(L_, path.c_str()) != 0) {
        err_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "load error";
        lua_settop(L_, base);
        if (err) *err = err_;
        return false;
    }
    return runPcall(base, err);
}

bool ScriptHost::runPcall(int base, std::string* err) {
    if (lua_pcall(L_, 0, LUA_MULTRET, 0) != 0) {
        err_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "unknown lua error";
        lua_settop(L_, base);
        if (err) *err = err_;
        return false;
    }
    // 丢弃返回值(脚本改为通过 toolbox.* 与宿主交互, 不依赖函数返回值)
    lua_settop(L_, base);
    if (err) err->clear();
    return true;
}

bool ScriptHost::collectScriptTools(const std::string& dir,
                                    std::vector<ScriptToolDef>* out,
                                    std::string* err) {
    if (!L_) { if (err) *err = "host not initialized"; return false; }
    out->clear();

    std::error_code ec;
    fs::directory_iterator it(toFsPath(dir), ec), end;
    if (ec) { if (err) *err = "无法打开脚本目录: " + ec.message(); return false; }
    std::vector<std::string> files;
    for (; it != end; ++it) {
        if (!it->is_regular_file()) continue;
        if (it->path().extension() != ".lua") continue;
        files.push_back(it->path().string());
    }
    std::sort(files.begin(), files.end());

    for (const std::string& f : files) {
        // 每脚本重置 def, 避免上一个脚本声明的 label/tool/on_run/inputs 污染下一个
        lua_getglobal(L_, "toolbox");
        if (lua_istable(L_, -1)) {
lua_newtable(L_);
            lua_setfield(L_, -2, "def");
        }
        lua_settop(L_, 0);

        std::string runErr;
        if (!runFile(f, &runErr)) {
            // 单个脚本坏了不拖垮整个工具箱(日志提示, 跳过)
            spdlog::warn("[lua] 脚本 {} 加载失败: {}", f, runErr);
            continue;
        }
        ScriptToolDef def;
        def.name = fs::path(f).stem().string();
        def.source = f;

        lua_getglobal(L_, "toolbox");
        if (!lua_istable(L_, -1)) { lua_settop(L_, 0); continue; }
        lua_getfield(L_, -1, "def");
        if (!lua_istable(L_, -1)) { lua_settop(L_, 0); continue; }

        auto getStr = [&](const char* k, std::string* dst) {
            lua_getfield(L_, -1, k);
            if (const char* s = lua_tostring(L_, -1)) *dst = s;
            lua_pop(L_, 1);
        };
        getStr("label", &def.label);
        getStr("description", &def.description);
        getStr("tool", &def.tool);

        lua_getfield(L_, -1, "on_run");
        if (!lua_isfunction(L_, -1)) {
            lua_pop(L_, 1);
            lua_getglobal(L_, "on_run");   // 未显式挂 def.on_run 时, 自动绑定全局函数 on_run
        }
        if (lua_isfunction(L_, -1)) def.onRunRef = luaL_ref(L_, LUA_REGISTRYINDEX);
        else lua_pop(L_, 1);

        auto collectNamed = [&](const char* key, std::vector<ScriptParam>* vec) {
            lua_getfield(L_, -1, key);
            if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
            lua_pushnil(L_);
            while (lua_next(L_, -2) != 0) {
                if (lua_isstring(L_, -2)) {
                    ScriptParam p;
                    p.name = lua_tostring(L_, -2);
                    if (lua_istable(L_, -1)) {
                        lua_getfield(L_, -1, "title");
                        if (const char* s = lua_tostring(L_, -1)) p.title = s;
                        lua_pop(L_, 1);
                        lua_getfield(L_, -1, "filter");
                        if (const char* s = lua_tostring(L_, -1)) p.filter = s;
                        lua_pop(L_, 1);
                        lua_getfield(L_, -1, "required");
                        p.required = (lua_toboolean(L_, -1) != 0);
                        lua_pop(L_, 1);
                        lua_getfield(L_, -1, "load");
                        p.load = (lua_toboolean(L_, -1) != 0);
                        lua_pop(L_, 1);
                        if (key[0] == 'p') {   // params: 读默认值
                            lua_getfield(L_, -1, "default");
                            if (const char* s = lua_tostring(L_, -1)) p.defValue = s;
                            lua_pop(L_, 1);
                        }
                    }
                    vec->push_back(std::move(p));
                }
                lua_pop(L_, 1);   // 弹出值, 保留 key
            }
            lua_pop(L_, 1);       // 弹出子表
        };
        collectNamed("inputs", &def.inputs);
        collectNamed("params", &def.params);
        collectNamed("outputs", &def.outputs);
        lua_settop(L_, 0);

        if (def.tool.empty() && def.label.empty() && def.description.empty()) continue;
        if (def.label.empty()) def.label = def.name;
        out->push_back(std::move(def));
    }
    if (err) err->clear();
    return true;
}

bool ScriptHost::runOnRun(const ScriptToolDef& t, std::map<std::string, std::string>* ctx,
                          std::string* err) {
    if (!L_) { if (err) *err = "host not initialized"; return false; }
    if (t.onRunRef < 0) { if (err) err->clear(); return true; }
    const int base = lua_gettop(L_);

    lua_rawgeti(L_, LUA_REGISTRYINDEX, t.onRunRef);
    if (!lua_isfunction(L_, -1)) { lua_settop(L_, base); if (err) err->clear(); return true; }
    lua_newtable(L_);
    for (const auto& kv : *ctx) {
        lua_pushstring(L_, kv.first.c_str());
        lua_pushstring(L_, kv.second.c_str());
        lua_settable(L_, -3);
    }
    // 约定: on_run(ctx) 返回(可能被修改的)ctx 表; 覆盖原参数。
    if (lua_pcall(L_, 1, 1, 0) != 0) {
        err_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "unknown lua error";
        lua_settop(L_, base);
        if (err) *err = err_;
        return false;
    }
    if (lua_istable(L_, -1)) {
        ctx->clear();
        lua_pushnil(L_);
        while (lua_next(L_, -2) != 0) {
            if (lua_isstring(L_, -2)) {
                const char* k = lua_tostring(L_, -2);
                size_t len = 0;
                const char* v = lua_tolstring(L_, -1, &len);
                if (k) (*ctx)[k] = v ? std::string(v, len) : "";
            }
            lua_pop(L_, 1);
        }
    }
    lua_settop(L_, base);
    if (err) err->clear();
    return true;
}