#include "doctest.h"
#include "toolbox/script_host.h"

#include <lua.hpp>
#include <string>

TEST_CASE("lua host: init + basic arithmetic") {
    ScriptHost h;
    std::string err;
    REQUIRE(h.init(&err));
    CHECK(err.empty());
    CHECK(h.runString("tb_res = 1 + 2"));
    lua_State* L = h.L();
    REQUIRE(L != nullptr);
    lua_getglobal(L, "tb_res");
    CHECK(lua_tointeger(L, -1) == 3);
    lua_pop(L, 1);
    h.shutdown();
}

TEST_CASE("lua host: state persists across runs") {
    ScriptHost h;
    REQUIRE(h.init());
    CHECK(h.runString("tb_acc = (tb_acc or 0) + 10"));
    CHECK(h.runString("tb_acc = tb_acc + 5"));
    lua_getglobal(h.L(), "tb_acc");
    CHECK(lua_tointeger(h.L(), -1) == 15);
    lua_pop(h.L(), 1);
}

TEST_CASE("lua host: toolbox API registered") {
    ScriptHost h;
    REQUIRE(h.init());
    lua_getglobal(h.L(), "toolbox");
    CHECK(lua_istable(h.L(), -1));
    lua_pop(h.L(), 1);
    // self_check 返回 LuaJIT 版本字符串
    CHECK(h.runString("tb_ver = toolbox.self_check()"));
    lua_getglobal(h.L(), "tb_ver");
    CHECK(lua_isstring(h.L(), -1));
    std::string ver = lua_tostring(h.L(), -1) ? lua_tostring(h.L(), -1) : "";
    CHECK(ver.find("LuaJIT") != std::string::npos);
    lua_pop(h.L(), 1);
    // echo/log 不崩溃
    CHECK(h.runString("toolbox.echo('echo ok'); toolbox.log('info', 'log ok')"));
}

TEST_CASE("lua host: runtime error captured") {
    ScriptHost h;
    REQUIRE(h.init());
    std::string err;
    CHECK_FALSE(h.runString("error('boom')", &err));
    CHECK(err.find("boom") != std::string::npos);
}

TEST_CASE("lua host: syntax error captured") {
    ScriptHost h;
    REQUIRE(h.init());
    std::string err;
    CHECK_FALSE(h.runString("this is not lua <<", &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("lua host: runFile missing file") {
    ScriptHost h;
    REQUIRE(h.init());
    std::string err;
    CHECK_FALSE(h.runFile("no/such/file.lua", &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("lua host: uninitialized host") {
    ScriptHost h;
    std::string err;
    CHECK_FALSE(h.runString("return 1", &err));
    CHECK_FALSE(err.empty());
}