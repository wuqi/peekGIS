-- xmake.lua (peekGIS) -- 全 vendored 依赖, 无需 xrepo
add_rules("mode.debug", "mode.release")

-- ===== 依赖路径配置 =====
-- GDAL/Zstd 由 vcpkg 提供。默认路径见下, 可覆盖:
--   xmake f --vcpkg_dir=D:/vcpkg/installed/x64-windows
option("vcpkg_dir")
    set_default("d:/dev/vcpkg/installed/x64-windows")
    set_showmenu(true)
    set_description("vcpkg 安装目录(installed 下的 x64-windows 平台目录)")

local vcpkg_dir = get_config("vcpkg_dir") or "d:/dev/vcpkg/installed/x64-windows"

target("peekgis")
    set_kind("binary")
    set_languages("c++20")

    add_includedirs("thirdparty/imgui")
    add_includedirs("thirdparty/imgui/backends")
    add_includedirs("thirdparty/glad/include")
    add_includedirs("thirdparty/glfw/include")   -- GLFW/glfw3.h + GL/wglext.h
    add_includedirs("thirdparty/glm")
    add_includedirs("thirdparty/spdlog/include")
    add_includedirs("thirdparty/toml11/include")
    add_includedirs("thirdparty/earcut")
    add_includedirs("src")
    add_cxflags("/utf-8")
    add_cxxflags("/utf-8")

    -- GDAL (来自 vcpkg, 已编译的动态库, x64-windows /MD)
    add_includedirs(vcpkg_dir .. "/include")
    add_linkdirs(vcpkg_dir .. "/lib")
    add_links("gdal", "zstd")

    -- Dear ImGui (docking, vendored)
    add_files("thirdparty/imgui/*.cpp")
    add_files("thirdparty/imgui/backends/imgui_impl_glfw.cpp")
    add_files("thirdparty/imgui/backends/imgui_impl_opengl3.cpp")

    -- GL 加载器 (glad, vendored)
    add_files("thirdparty/glad/src/gl.c")

    -- GLFW 从源码编译 (Windows/Win32/WGL)
    add_files("thirdparty/glfw/src/*.c")
    add_defines("_GLFW_WIN32", "_GLFW_WGL")

    -- 应用代码
    add_files("src/**.cpp")

    -- 应用图标(Windows 资源: rc.exe 编译, 嵌入 exe)
    add_files("assets/app.rc")

    add_defines("IMGUI_IMPL_OPENGL_LOADER_GLAD", "SPDLOG_HEADER_ONLY", "FMT_HEADER_ONLY")
    add_syslinks("user32", "gdi32", "shell32", "dwmapi", "winmm", "opengl32", "comdlg32")

    -- 构建后把 vcpkg 的 GDAL 运行期 DLL 与数据文件拷到 exe 旁
    -- 注意: 只拷非 boost 的 DLL —— vcpkg 实例里装了整套 boost 1.88(与本程序无关),
    -- 若用 *.dll 通配会把几十个 boost_*.dll 也拷进来(见下"boost 说明")
    -- 关键: 先把 share/(GDAL_DATA/PROJ_LIB 必需)拷过去, 再拷 DLL; 单个 DLL 被占用(如别的 GIS
    -- 软件正加载)时忽略, 不阻断整体拷贝, 否则 GDAL_DATA/PROJ_LIB 会指向不存在目录导致打不开文件。
    -- 只拷 GDAL/PROJ 的运行必需最小集(proj.db + gdal csv/wkt), 不拷编译期 .cmake/json/xsd 等。
    after_build(function (target)
        local out = target:targetdir()
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
        -- 拷贝字体文件到 exe 旁
        os.cp("fonts/LXGW.ttf", out .. "/fonts/LXGW.ttf")
        -- DLL 只在目标缺失时才拷, 避免被其它 GIS 软件占用时拷贝失败中断整个构建
        for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
            if not f:match("boost") then
                local name = path.filename(f)
                if not os.exists(path.join(out, name)) then os.cp(f, out) end
            end
        end
    end)

-- 单元测试 (doctest, vendored 单头文件)
target("tests")
    set_kind("binary")
    set_languages("c++20")
    add_includedirs("src")
    add_includedirs("thirdparty/doctest")
    add_includedirs("thirdparty/toml11/include")
    add_includedirs("thirdparty/spdlog/include")
    add_includedirs("thirdparty/earcut")
    add_cxflags("/utf-8")
    add_cxxflags("/utf-8")
    add_includedirs(vcpkg_dir .. "/include")
    add_linkdirs(vcpkg_dir .. "/lib")
    add_links("gdal", "zstd")
    add_files("tests/*.cpp")
    add_files("src/data/geom_cache.cpp")
    add_files("src/data/gdal_datasource.cpp")
    add_files("src/data/reproject.cpp")
    add_files("src/data/attr_table.cpp")
    add_files("src/map/map_scene.cpp")
    add_files("src/platform/exe_path.cpp")
    add_defines("SPDLOG_HEADER_ONLY", "FMT_HEADER_ONLY")
    add_syslinks("user32", "gdi32", "comdlg32", "shell32", "psapi")
    after_build(function (target)
        local out = target:targetdir()
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
            if not f:match("boost") then
                local name = path.filename(f)
                if not os.exists(path.join(out, name)) then os.cp(f, out) end
            end
        end
    end)

target("fix_shx")
    set_kind("binary")
    set_languages("c++20")
    add_cxflags("/utf-8"); add_cxxflags("/utf-8")
    add_includedirs(vcpkg_dir .. "/include")
    add_linkdirs(vcpkg_dir .. "/lib")
    add_links("gdal")
    add_files("tools/fix_shx.cpp")
    add_syslinks("user32", "gdi32", "comdlg32", "shell32")
    after_build(function (target)
        local out = target:targetdir()
        for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
            if not f:match("boost") then os.cp(f, out) end
        end
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
    end)

target("parcheck")
    set_kind("binary")
    set_languages("c++20")
    add_includedirs("src")
    add_includedirs("thirdparty/toml11/include")
    add_includedirs("thirdparty/earcut")
    add_cxflags("/utf-8"); add_cxxflags("/utf-8")
    add_includedirs(vcpkg_dir .. "/include")
    add_linkdirs(vcpkg_dir .. "/lib")
    add_links("gdal")
    add_files("tools/par_check.cpp")
    -- NOTE: 该目标链接偶发产出空 exe, 暂禁用; 保留源码供将来排查
    set_enabled(false)
    add_files("src/platform/exe_path.cpp")
    add_syslinks("user32", "gdi32", "comdlg32", "shell32")
    after_build(function (target)
        local out = target:targetdir()
        for _, f in ipairs(os.files(vcpkg_dir .. "/bin/*.dll")) do
            if not f:match("boost") then os.cp(f, out) end
        end
        os.mkdir(out .. "/share/gdal")
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.csv")) do os.cp(f, out .. "/share/gdal/") end
        for _, f in ipairs(os.files(vcpkg_dir .. "/share/gdal/*.wkt")) do os.cp(f, out .. "/share/gdal/") end
        os.mkdir(out .. "/share/proj")
        if os.exists(vcpkg_dir .. "/share/proj/proj.db") then os.cp(vcpkg_dir .. "/share/proj/proj.db", out .. "/share/proj/") end
        if os.exists(vcpkg_dir .. "/share/proj/proj.ini") then os.cp(vcpkg_dir .. "/share/proj/proj.ini", out .. "/share/proj/") end
    end)
