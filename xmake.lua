-- xmake.lua (peekGIS) -- 全 vendored 依赖, 无需 xrepo
add_rules("mode.debug", "mode.release")

-- ===== 依赖路径配置 =====
-- GDAL/Zstd 由 vcpkg 提供。默认按平台取, 可覆盖:
--   xmake f --vcpkg_dir=D:/vcpkg/installed/x64-windows    (Windows)
--   xmake f --vcpkg_dir=/opt/vcpkg/installed/x64-linux    (Linux)
-- 想用系统库(Linux 装着 gdal-dev/netcdf-dev/zstd-dev)时: --vcpkg_dir=none,
-- 会跳过 vcpkg 的 include/linkdir, 直接链系统 -lgdal -lzstd -lnetcdf。
-- 注意: 系统库方案下 after_build 拷不到 GDAL/PROJ 的 share 数据, 运行时需
-- 设 GDAL_DATA/PROJ_LIB 环境变量(或后续给 Linux 加系统数据目录检索)。
option("vcpkg_dir")
    set_default("")
    set_showmenu(true)
    set_description("vcpkg 安装目录(installed 下的平台目录); 留空按平台取默认")

local vcpkg_dir = get_config("vcpkg_dir")
if vcpkg_dir == nil or vcpkg_dir == "" then
    vcpkg_dir = is_plat("windows") and "d:/dev/vcpkg/installed/x64-windows" or "/opt/vcpkg/installed/x64-linux"
end
local vcpkg_ok = vcpkg_dir ~= "none" and os.exists(vcpkg_dir .. "/include")

-- 运行期输出布局(所有二进制进 bin/, GDAL 数据与资源分目录), 保持 exe 自包含:
--   bin/*.exe  +  bin/*.dll            程序与运行库
--   bin/share/gdal, bin/share/proj     GDAL_DATA/PROJ_LIB(运行时按 exe 目录定位)
--   bin/assets/fonts/LXGW.ttf          UI 字体
--   bin/assets/layer-group-solid.png   左侧图标栏图像
local bin_dir = "$(buildir)/$(plat)/$(arch)/$(mode)/bin"

target("peekgis")
    set_kind("binary")
    set_languages("c++20")
    set_targetdir(bin_dir)

    add_includedirs("thirdparty/imgui")
    add_includedirs("thirdparty/imgui/backends")
    add_includedirs("thirdparty/glad/include")
    add_includedirs("thirdparty/glfw/include")
    add_includedirs("thirdparty/glm")
    add_includedirs("thirdparty/spdlog/include")
    add_includedirs("thirdparty/toml11/include")
    add_includedirs("thirdparty/earcut")
    add_includedirs("thirdparty")
    add_includedirs("src")
    if is_plat("windows") then add_cxflags("/utf-8"); add_cxxflags("/utf-8") end

    if vcpkg_ok then
        add_includedirs(vcpkg_dir .. "/include/luajit")
        add_includedirs(vcpkg_dir .. "/include")
        add_linkdirs(vcpkg_dir .. "/lib")
    end
    add_links("gdal", "zstd", "netcdf", "lua51")

    add_files("thirdparty/imgui/*.cpp")
    add_files("thirdparty/imgui/backends/imgui_impl_glfw.cpp")
    add_files("thirdparty/imgui/backends/imgui_impl_opengl3.cpp")
    add_files("thirdparty/glad/src/gl.c")
    add_files("thirdparty/stb_image.cpp")
    add_files("thirdparty/glfw/src/*.c")
    if is_plat("windows") then
        add_defines("_GLFW_WIN32", "_GLFW_WGL")
    else
        add_defines("_GLFW_X11")
    end

    add_files("src/**.cpp")
    if is_plat("windows") then add_files("assets/app.rc") end

    add_defines("IMGUI_IMPL_OPENGL_LOADER_GLAD", "FMT_HEADER_ONLY")
    if is_plat("windows") then
        add_syslinks("user32", "gdi32", "shell32", "dwmapi", "winmm", "opengl32", "comdlg32")
    else
        -- GLFW(X11)+OpenGL 桌面构建在 Linux 下需要的链接库
        add_syslinks("dl", "m", "pthread", "X11", "Xrandr", "Xi", "Xinerama", "Xxf86vm", "Xcursor", "Xfixes")
        add_links("GL")
    end

    -- GUI 程序: 隐藏控制台(窗口启动不弹黑窗)。入口仍是 main() -> 需显式 /ENTRY:mainCRTStartup。
    -- 诊断工具(diag_*/fix_shx/tests)保持控制台子系统的默认行为。
    if is_plat("windows") then
        add_ldflags("/SUBSYSTEM:WINDOWS", {force = true})
        add_ldflags("/ENTRY:mainCRTStartup", {force = true})
    end

    -- 构建后: 拷 GDAL/PROJ 数据到 bin/share/, 字体/图标到 bin/assets/, DLL 到 bin/
    -- 注意: 只拷非 boost 的 DLL; 已有同名文件跳过(避免别的 GIS 进程占用时拷贝失败中断构建)。
    -- 说明: os.mkdir/cp/files 是 xmake build 作用域增强后的 API, 必须直接写在 after_build
    -- 回调体里, 不能包进顶层 local 函数(顶层只有标准 os 表, 没有 mkdir/cp 扩展)。
    after_build(function (target)
        local out = target:targetdir()
        if vcpkg_ok then
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
    end
        if vcpkg_ok then
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
    end
        if is_plat("windows") then
            for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
                if not f:match("boost") then
                    local name = path.filename(f)
                    if not os.exists(path.join(out, name)) then os.cp(f, out) end
                end
            end
        end
        os.cp("fonts/LXGW.ttf", out .. "/assets/fonts/LXGW.ttf")
        os.cp("assets/layer-group-solid.png", out .. "/assets/layer-group-solid.png")
        os.cp("assets/toolbox-solid.png", out .. "/assets/toolbox-solid.png")
        for _, f in ipairs(os.files("assets/scripts/*.lua")) do os.cp(f, out .. "/scripts/") end
        -- 在 bin 的上级目录(release 根)放一个启动脚本, 和 bin/ 并列, 便于直接双击启动。
        if is_plat("windows") then
            -- 用 .vbs 隐藏启动: 程序已是 GUI 子系统, 唯一会闪窗的来源是 cmd.exe 操控台本身,
            -- 因此启动脚本也用 VBScript(WScript.Shell.Run windowstate=0)做到无黑窗。
            local root = path.join(out, "..")
            io.writefile(path.join(root, "peekGIS.vbs"), [[
' peekGIS launcher (generated by xmake). flash-free: no console window.
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh  = CreateObject("WScript.Shell")
root = fso.GetParentFolderName(WScript.ScriptFullName)
exeDir = root & "\bin"
sh.CurrentDirectory = exeDir
sh.Run """" & exeDir & "\peekgis.exe""", 0, False
]])
            io.writefile(path.join(root, "peekGIS.cmd"), [[
@echo off
rem this launcher is generated by xmake. flash-free alternative: double-click peekGIS.vbs
start "" /D "%~dp0bin" "%~dp0bin\peekgis.exe"
]])
        elseif is_plat("linux") then
            -- Linux 从桌面/文件管理器启动本来就不弹终端, 无需隐藏窗口; 等价入口是 .desktop 文件。
            -- 生成到 release 根, 使用绝对路径; 想进应用菜单可拷到 ~/.local/share/applications/。
            local root = path.join(out, "..")
            local exe_abs = path.join(root, "bin", "peekgis")
            local icon_abs = path.join(root, "bin", "assets", "layer-group-solid.png")
            io.writefile(path.join(root, "peekGIS.desktop"), [[
[Desktop Entry]
Type=Application
Name=peekGIS
Comment=GIS raster/vector viewer (GDAL)
Exec=]] .. exe_abs .. [[
Icon=]] .. icon_abs .. [[
Terminal=false
Categories=Science;Geoscience;Graphics;
]])
        end
    end)

target("tests")
    set_kind("binary")
    set_languages("c++20")
    set_targetdir(bin_dir)
    add_includedirs("src")
    add_includedirs("thirdparty/doctest")
    add_includedirs("thirdparty/toml11/include")
    add_includedirs("thirdparty/spdlog/include")
    add_includedirs("thirdparty/earcut")
    if is_plat("windows") then add_cxflags("/utf-8"); add_cxxflags("/utf-8") end
    if vcpkg_ok then
        add_includedirs(vcpkg_dir .. "/include/luajit")
        add_includedirs(vcpkg_dir .. "/include")
        add_linkdirs(vcpkg_dir .. "/lib")
    end
    add_links("gdal", "zstd", "netcdf", "lua51")
    add_files("tests/*.cpp")
    add_files("src/toolbox/script_host.cpp")
    add_files("src/toolbox/gdal_proc.cpp")
    add_files("src/toolbox/tool_registry.cpp")
    add_files("src/data/geom_cache.cpp")
    add_files("src/data/vector_reader.cpp")
    add_files("src/data/raster_reader.cpp")
    add_files("src/data/geoloc.cpp")
    add_files("src/data/reproject.cpp")
    add_files("src/data/attr_table.cpp")
    add_files("src/vt/vt_cache.cpp")
    add_files("src/vt/vt_source.cpp")
    add_files("src/vt/vt_build.cpp")
    add_files("src/map/map_scene.cpp")
    add_files("src/platform/exe_path.cpp")
    add_defines("FMT_HEADER_ONLY")
    if is_plat("windows") then
        add_syslinks("user32", "gdi32", "comdlg32", "shell32")
    else
        add_syslinks("dl", "m", "pthread")
    end
    after_build(function (target)
        local out = target:targetdir()
        if vcpkg_ok then
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
    end
        if vcpkg_ok then
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
    end
        if is_plat("windows") then
            for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
                if not f:match("boost") then
                    local name = path.filename(f)
                    if not os.exists(path.join(out, name)) then os.cp(f, out) end
                end
            end
        end
    end)

target("fix_shx")
    set_kind("binary")
    set_languages("c++20")
    set_targetdir(bin_dir)
    if is_plat("windows") then add_cxflags("/utf-8"); add_cxxflags("/utf-8") end
    if vcpkg_ok then
        add_includedirs(vcpkg_dir .. "/include")
        add_linkdirs(vcpkg_dir .. "/lib")
    end
    add_links("gdal")
    add_files("tools/fix_shx.cpp")
    if is_plat("windows") then
        add_syslinks("user32", "gdi32", "comdlg32", "shell32")
    else
        add_syslinks("dl", "m", "pthread")
    end
    after_build(function (target)
        local out = target:targetdir()
        if is_plat("windows") then
            for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
                if not f:match("boost") then os.cp(f, out) end
            end
        end
        if vcpkg_ok then
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
    end
        if vcpkg_ok then
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
    end
    end)

target("diag_srs")
    set_kind("binary")
    set_languages("c++20")
    set_targetdir(bin_dir)
    add_includedirs("src")
    add_includedirs("thirdparty/spdlog/include")
    if is_plat("windows") then add_cxflags("/utf-8"); add_cxxflags("/utf-8") end
    if vcpkg_ok then
        add_includedirs(vcpkg_dir .. "/include")
        add_linkdirs(vcpkg_dir .. "/lib")
    end
    add_links("gdal", "zstd")
    add_files("tools/diag_srs.cpp")
    add_files("src/platform/exe_path.cpp")
    add_defines("FMT_HEADER_ONLY")
    if is_plat("windows") then
        add_syslinks("user32", "gdi32", "comdlg32", "shell32")
    else
        add_syslinks("dl", "m", "pthread")
    end
    after_build(function (target)
        local out = target:targetdir()
        if vcpkg_ok then
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
    end
        if vcpkg_ok then
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
    end
        if is_plat("windows") then
            for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
                if not f:match("boost") then
                    local name = path.filename(f)
                    if not os.exists(path.join(out, name)) then os.cp(f, out) end
                end
            end
        end
    end)

target("diag_geoloc")
    set_kind("binary")
    set_languages("c++20")
    set_targetdir(bin_dir)
    add_includedirs("src")
    add_includedirs("thirdparty/spdlog/include")
    add_includedirs("thirdparty/earcut")
    if is_plat("windows") then add_cxflags("/utf-8"); add_cxxflags("/utf-8") end
    if vcpkg_ok then
        add_includedirs(vcpkg_dir .. "/include")
        add_linkdirs(vcpkg_dir .. "/lib")
    end
    add_links("gdal", "zstd", "netcdf")
    add_files("tools/diag_geoloc.cpp")
    add_files("src/data/geoloc.cpp")
    add_files("src/data/vector_reader.cpp")
    add_files("src/data/raster_reader.cpp")
    add_files("src/data/reproject.cpp")
    add_files("src/platform/exe_path.cpp")
    add_defines("FMT_HEADER_ONLY")
    if is_plat("windows") then
        add_syslinks("user32", "gdi32", "comdlg32", "shell32")
    else
        add_syslinks("dl", "m", "pthread")
    end
    after_build(function (target)
        local out = target:targetdir()
        if vcpkg_ok then
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
    end
        if vcpkg_ok then
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
    end
        if is_plat("windows") then
            for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
                if not f:match("boost") then
                    local name = path.filename(f)
                    if not os.exists(path.join(out, name)) then os.cp(f, out) end
                end
            end
        end
    end)

target("toml_probe")
    set_kind("binary")
    set_languages("c++20")
    set_targetdir(bin_dir)
    add_includedirs("thirdparty/toml11/include")
    if is_plat("windows") then add_cxflags("/utf-8"); add_cxxflags("/utf-8") end
    add_files("tools/toml_probe.cpp")

-- vt 模块独立单测(不依赖 lua/GL, 便于在缺 lua 的环境验证 v2 管线)
target("vt_tests")
    set_kind("binary")
    set_languages("c++20")
    set_targetdir(bin_dir)
    add_includedirs("src")
    add_includedirs("thirdparty/doctest")
    add_includedirs("thirdparty/spdlog/include")
    add_includedirs("thirdparty/earcut")
    add_includedirs("thirdparty/glad/include")
    if is_plat("windows") then add_cxflags("/utf-8"); add_cxxflags("/utf-8") end
    if vcpkg_ok then
        add_includedirs(vcpkg_dir .. "/include")
        add_linkdirs(vcpkg_dir .. "/lib")
    end
    add_links("gdal", "zstd")
    add_files("tests/test_main.cpp")
    add_files("tests/test_vt.cpp")
    add_files("src/vt/vt_cache.cpp")
    add_files("src/vt/vt_source.cpp")
    add_files("src/vt/vt_build.cpp")
    add_files("src/render/vt_render.cpp")
    add_files("thirdparty/glad/src/gl.c")
    add_files("src/data/reproject.cpp")
    add_files("src/platform/exe_path.cpp")
    add_defines("FMT_HEADER_ONLY")
    if is_plat("windows") then
        add_syslinks("user32", "gdi32", "comdlg32", "shell32", "opengl32")
    else
        add_syslinks("dl", "m", "pthread")
        add_links("GL")
    end
    after_build(function (target)
        local out = target:targetdir()
        if vcpkg_ok then
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
    end
        if vcpkg_ok then
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
    end
        if is_plat("windows") then
            for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
                if not f:match("boost") then
                    local name = path.filename(f)
                    if not os.exists(path.join(out, name)) then os.cp(f, out) end
                end
            end
        end
    end)

-- 构建 v2 矢量瓦片缓存(CLI): tools/vt_build.cpp + src/vt/*
target("vt_build")
    set_kind("binary")
    set_languages("c++20")
    set_targetdir(bin_dir)
    add_includedirs("src")
    add_includedirs("thirdparty/spdlog/include")
    add_includedirs("thirdparty/earcut")
    if is_plat("windows") then add_cxflags("/utf-8"); add_cxxflags("/utf-8") end
    if vcpkg_ok then
        add_includedirs(vcpkg_dir .. "/include")
        add_linkdirs(vcpkg_dir .. "/lib")
    end
    add_links("gdal", "zstd")
    add_files("tools/vt_build.cpp")
    add_files("src/vt/vt_cache.cpp")
    add_files("src/vt/vt_source.cpp")
    add_files("src/vt/vt_build.cpp")
    add_files("src/data/reproject.cpp")
    add_files("src/platform/exe_path.cpp")
    add_defines("FMT_HEADER_ONLY")
    if is_plat("windows") then
        add_syslinks("user32", "gdi32", "comdlg32", "shell32")
    else
        add_syslinks("dl", "m", "pthread")
    end
    after_build(function (target)
        local out = target:targetdir()
        if vcpkg_ok then
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
    end
        if vcpkg_ok then
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
    end
        if is_plat("windows") then
            for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
                if not f:match("boost") then
                    local name = path.filename(f)
                    if not os.exists(path.join(out, name)) then os.cp(f, out) end
                end
            end
        end
    end)

-- 矢量金字塔快速估算(只读): 采样估 N / 各层点数 / 选最细层 L* / 路由建议。
-- 详见 docs/矢量缓存方案.md 与 tools/vt_estimate.cpp。
target("vt_estimate")
    set_kind("binary")
    set_languages("c++20")
    set_targetdir(bin_dir)
    add_includedirs("src")
    add_includedirs("thirdparty/spdlog/include")
    if is_plat("windows") then add_cxflags("/utf-8"); add_cxxflags("/utf-8") end
    if vcpkg_ok then
        add_includedirs(vcpkg_dir .. "/include")
        add_linkdirs(vcpkg_dir .. "/lib")
    end
    add_links("gdal", "zstd")
    add_files("tools/vt_estimate.cpp")
    add_files("src/platform/exe_path.cpp")
    add_defines("FMT_HEADER_ONLY")
    if is_plat("windows") then
        add_syslinks("user32", "gdi32", "comdlg32", "shell32")
    else
        add_syslinks("dl", "m", "pthread")
    end
    after_build(function (target)
        local out = target:targetdir()
        if vcpkg_ok then
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
    end
        if vcpkg_ok then
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
    end
        if is_plat("windows") then
            for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
                if not f:match("boost") then
                    local name = path.filename(f)
                    if not os.exists(path.join(out, name)) then os.cp(f, out) end
                end
            end
        end
    end)