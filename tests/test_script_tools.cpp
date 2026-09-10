#include "doctest.h"
#include "toolbox/script_host.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string findScriptsDir() {
    std::string cwd = fs::current_path().string();
    if (fs::exists(cwd + "/assets/scripts")) return cwd + "/assets/scripts";
    return std::string();
}

TEST_CASE("script host: collectScriptTools loads preset scripts") {
    std::string dir = findScriptsDir();
    REQUIRE_FALSE(dir.empty());
    ScriptHost h;
    REQUIRE(h.init());
    std::vector<ScriptToolDef> tools;
    std::string err;
    REQUIRE(h.collectScriptTools(dir, &tools, &err));
    CHECK(err.empty());
    REQUIRE_FALSE(tools.empty());
    bool foundReproj = false, foundBuffer = false;
    for (const auto& t : tools) {
        CHECK_FALSE(t.name.empty());
        CHECK_FALSE(t.label.empty());
        if (t.name == "reproject_raster") {
            foundReproj = true;
            CHECK(t.tool == "gdal:raster:reproject");
            CHECK(t.onRunRef < 0);  // 预置脚本无 on_run 钩子 -> -2(LUA_NOREF)
            bool hasIn = false, hasOut = false;
            for (const auto& p : t.inputs)  if (p.name == "input")  { hasIn = true;  CHECK(p.required); }
            for (const auto& p : t.outputs) if (p.name == "output") { hasOut = true; CHECK(p.load); }
            CHECK(hasIn);
            CHECK(hasOut);
        }
        if (t.name == "buffer_vector") foundBuffer = true;
    }
    CHECK(foundReproj);
    CHECK(foundBuffer);
}

TEST_CASE("script host: runOnRun passes ctx through when no hook") {
    std::string dir = findScriptsDir();
    ScriptHost h;
    REQUIRE(h.init());
    std::vector<ScriptToolDef> tools;
    REQUIRE(h.collectScriptTools(dir, &tools, nullptr));
    const ScriptToolDef* t = nullptr;
    for (const auto& s : tools) if (s.name == "reproject_raster") { t = &s; break; }
    REQUIRE(t != nullptr);
    std::map<std::string, std::string> ctx;
    ctx["input"] = "in.tif";
    ctx["output"] = "out.tif";
    ctx["dst-crs"] = "EPSG:4326";
    ctx["overwrite"] = "true";
    std::string err;
    REQUIRE(h.runOnRun(*t, &ctx, &err));
    CHECK(err.empty());
    CHECK(ctx["input"] == "in.tif");
    CHECK(ctx["output"] == "out.tif");
}

TEST_CASE("script host: on_run may rewrite ctx via returned table") {
    fs::path tmp = fs::temp_directory_path() / "peek_script_tools_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    std::ofstream of((tmp / "rewrite.lua").string());
    of << "toolbox.def.label = '改写输出'\n"
          "toolbox.def.tool = 'gdal:raster:convert'\n"
          "toolbox.input('input', { required = true })\n"
          "toolbox.output('output', { load = true })\n"
          "function on_run(ctx)\n"
          "  ctx.output = ctx.output .. '.tif'\n"
          "  return ctx\n"
          "end\n";
    of.close();
    ScriptHost h;
    REQUIRE(h.init());
    std::vector<ScriptToolDef> tools;
    REQUIRE(h.collectScriptTools(tmp.string(), &tools, nullptr));
    REQUIRE(tools.size() == 1);
    CHECK(tools[0].inputs.size() == 1);
    CHECK(tools[0].outputs.size() == 1);
    if (!tools[0].inputs.empty()) CHECK(tools[0].inputs[0].name == "input");
    if (!tools[0].outputs.empty()) CHECK(tools[0].outputs[0].name == "output");
    std::map<std::string, std::string> ctx;
    ctx["input"] = "in.tif";
    ctx["output"] = "out";
    std::string err;
    REQUIRE(h.runOnRun(tools[0], &ctx, &err));
    CHECK(ctx["output"] == "out.tif");
    fs::remove_all(tmp);
}