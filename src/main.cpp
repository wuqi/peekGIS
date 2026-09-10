// windows.h 必须先于 glfw3.h: 否则 glfw3.h 私自定义 APIENTRY=__stdcall,
// 之后 minwindef.h 再定义 =WINAPI 触发 C4005 告警。
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "config/app_config.h"
#include "platform/dpi.h"
#include "util/logger.h"
#include "platform/exe_path.h"
#include "toolbox/script_host.h"
#include "app/app.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

#ifdef _WIN32
static std::vector<std::string> utf8Args() {
    int n = 0;
    wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::string> out;
    if (!w) return out;
    for (int i = 0; i < n; i++) {
        int c = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(c > 0 ? c - 1 : 0, '\0');
        if (c > 0) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], c, nullptr, nullptr);
        out.push_back(s);
    }
    LocalFree(w);
    return out;
}
#endif

static UIState* g_ui = nullptr;
static void dropCallback(GLFWwindow*, int count, const char** paths) {
    if (count > 0 && g_ui) {
        for (int i = 0; i < count; i++) g_ui->openPaths.emplace_back(paths[i]);
        g_ui->openRequested = true;
    }
}

// 程序图标(RGBA, 程序化绘制, 与 assets/icon.svg 同款设计): 青绿色圆底 + 网格 + 白色取景环/十字准星
static std::vector<unsigned char> makeAppIcon(int n) {
    std::vector<unsigned char> px((size_t)n * n * 4, 0);
    const float half = n / 2.0f;
    const float bgR = 0.92f * half;
    const float ringR = 0.453f * half;
    const float ringT = std::max(std::round(0.035f * n), 1.0f);
    const float plusT = std::max(std::round(0.035f * n), 1.0f);
    const float plusLen = 0.25f * n;
    const int cell = std::max((int)std::round(n / 8.0f), 1);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float dx = x - half, dy = y - half;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > bgR) continue;   // 透明背景
            float t = 0.5f * (x / (float)n) + 0.5f * (y / (float)n);
            float R = 63.0f - (63.0f - 14.0f) * t;
            float G = 180.0f - (180.0f - 42.0f) * t;
            float B = 192.0f - (192.0f - 56.0f) * t;
            bool white = std::fabs(dist - ringR) <= ringT ||
                (std::fabs(dx) <= plusT / 2 && std::fabs(dy) <= plusLen) ||
                (std::fabs(dy) <= plusT / 2 && std::fabs(dx) <= plusLen);
            if (white) { R = G = B = 255; }
            else if (x % cell == 0 || y % cell == 0) {   // 网格与 18% 白混合
                R = R * 0.82f + 255.0f * 0.18f;
                G = G * 0.82f + 255.0f * 0.18f;
                B = B * 0.82f + 255.0f * 0.18f;
            }
            size_t i = ((size_t)y * n + x) * 4;
            px[i] = (unsigned char)std::round(R);
            px[i + 1] = (unsigned char)std::round(G);
            px[i + 2] = (unsigned char)std::round(B);
            px[i + 3] = 255;
        }
    }
    return px;
}

static void setWindowIcon(GLFWwindow* window) {
    static const int sizes[2] = { 64, 32 };
    static std::vector<std::vector<unsigned char>> store;
    std::vector<GLFWimage> imgs;
    for (int s : sizes) {
        store.emplace_back(makeAppIcon(s));
        GLFWimage img = { s, s, store.back().data() };
        imgs.push_back(img);
    }
    glfwSetWindowIcon(window, (int)imgs.size(), imgs.data());
}

int main(int argc, char** argv) {
    // 在最前面清掉继承自系统/用户的 PROJ_LIB/GDAL_DATA(常见是 PostgreSQL 附属的旧
    // proj.db), 否则 PROJ 会把它纳入搜索路径并在做 CRS 识别时刷 "another PROJ
    // installation" 噪音。真正的数据路径由 ensureGdal() 统一指向 exe 旁 share。
#ifdef _WIN32
    _putenv_s("PROJ_LIB", "");
    _putenv_s("GDAL_DATA", "");
#endif
    AppConfig cfg;
    cfg.load("config.toml");
    cfg.save("config.toml");  // 落盘默认配置(含 [log] level)
    initLogger(exeDir(), cfg.spdlogLevel());   // 日志等级从第一条日志起生效
    spdlog::info("peekGIS 启动");

    // GLFW 错误回调: 窗口/上下文创建失败时把真实原因写日志(否则默认只往 stderr 打一行, 无声无息)
    glfwSetErrorCallback([](int code, const char* desc) {
        spdlog::error("GLFW error {}: {}", code, desc ? desc : "(null)");
    });
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "peekGIS", nullptr, nullptr);
    if (!window) { fprintf(stderr, "window failed\n"); return 1; }
    setWindowIcon(window);   // 窗口/任务栏图标
    glfwMakeContextCurrent(window);
    glfwSwapInterval(std::getenv("PEEK_NO_VSYNC") ? 0 : 1);   // 关闭 vsync: 裸测渲染吞吐(性能自检)
    glfwSetDropCallback(window, dropCallback);

    if (!gladLoadGL(glfwGetProcAddress)) { fprintf(stderr, "gladLoadGL failed\n"); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    loadAppFont(io, getDpiScale(window), cfg.font_file);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    App app(cfg);
    g_ui = &app.ui;

    ScriptHost luaHost;   // 工具箱脚本宿主(阶段1: 仅自检/命令行脚本)
    luaHost.init();

#ifdef _WIN32
    std::vector<std::string> args = utf8Args();
#else
    std::vector<std::string> args(argv, argv + argc);
#endif
    std::string luaFile;   // --lua=<file> 指定启动即执行的脚本
    std::string luaSelf = R"(toolbox.echo("lua: " .. toolbox.self_check() .. " self-check ok"))";
    if (const char* e = std::getenv("PEEK_LUA")) luaFile = e;
    if (args.size() > 1) {  // 命令行直接打开矢量文件便于快速验证(支持中文路径, 可多文件)
        for (size_t ai = 1; ai < args.size(); ai++) {
            if (args[ai] == "--after" && ai + 1 < args.size()) {
                app.deferOpen(args[++ai]);   // 前一个文件加载完成后才打开(复现顺序加载)
                continue;
            }
            if (args[ai].rfind("--lua=", 0) == 0) {
                luaFile = args[ai].substr(6);
                continue;
            }
            app.ui.openPaths.push_back(args[ai]);
        }
        if (!app.ui.openPaths.empty()) app.ui.openRequested = true;
    }
    // 脚本宿主自检: --lua=<file> 或环境 PEEK_LUA 指向脚本时执行; 否则跑内置一行自检
    if (!luaFile.empty()) {
        std::string err;
        if (!luaHost.runFile(luaFile, &err)) spdlog::error("[lua] runFile failed: {}", err);
    } else if (std::getenv("PEEK_LUA_SELF")) {
        std::string err;
        if (!luaHost.runString(luaSelf, &err)) spdlog::error("[lua] self-check failed: {}", err);
    }

    while (!glfwWindowShouldClose(window)) {
        app.frame(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    app.shutdown();    // 停细节块/栅格 worker + join 后台线程
    luaHost.shutdown();
    return 0;
}