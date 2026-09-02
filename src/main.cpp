#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "map/map_scene.h"
#include "render/gl_backend.h"
#include "config/app_config.h"
#include "platform/dpi.h"
#include "app/ui.h"
#include "data/async_loader.h"
#include "data/gdal_datasource.h"
#include "data/attr_table.h"
#include "data/reproject.h"
#include "util/logger.h"
#include "platform/exe_path.h"

#include <cstdio>
#include <deque>
#include <stdexcept>
#include <cmath>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
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

// 异步属性识别共享态(双击时抓拍目标清单, 后台线程逐层查询, 主线程每帧取回)
struct IdentifyTarget {
    std::string path;
    int layerIdx = 0;
    int srcEpsg = 0;
};
static std::mutex g_identifyMtx;
static std::atomic<uint64_t> g_identifyGen{0};                 // 每次新请求递增, 旧线程结果作废
static std::vector<IdentifyHit> g_identifyGeo;                 // 命中结果(含属性, 锁保护)
static bool g_identifyGeoReady = false;
static int g_identifyRemaining = 0;     // 本次请求未完成的目标数(锁保护)
static bool g_identifyDone = false;     // 本次请求全部目标已结束(锁保护)

// 后台图层元数据预读(non-shp 多图层判断): worker -> 主线程
static std::mutex g_metaMtx;
static std::string g_metaPath;
static std::vector<LayerMeta> g_metaResult;
static bool g_metaOk = false;
static std::atomic<bool> g_metaDone{false};

// 后台属性表读取: 单任务(worker 先发 info+页, 最后才取总数, 让行先渲染出来)
static std::mutex g_attrMtx;
static bool g_attrBusy = false;                // 有后台任务在跑(锁保护)
static bool g_attrInfoReady = false;
static AttrLayerInfo g_attrFetchedInfo;
static bool g_attrPageReady = false;
static AttrPageData g_attrFetchedPage;
static bool g_attrCountReady = false;
static long long g_attrFetchedCount = -1;
static bool g_attrNeedCount = false;           // 任务是否含总数阶段(锁保护)
static int g_attrResultGen = -1;               // 结果所属代号(锁保护)
static std::atomic<int> g_attrGen{0};          // 当前有效代号; 打开/清空/卸载时递增, 使旧结果作废

struct AttrTaskSpec {
    std::string path;
    int layerIdx = 0;
    int page = 0;
    int rowsPerPage = 20;
    int enc = 0;
    int gen = 0;
    bool needCount = false;   // 仅打开(首页)任务取总数; 翻页任务沿用已有总数
};

// 发布一个新后台任务(已带锁检查过未在忙)。worker 完成后经主线程取回。
// 顺序: 1)打开(字段定义) 2)拉一页 3)取总数(可能慢, 放最后, 不阻塞行渲染)
static void launchAttrTask(AttrTaskSpec spec) {
    spec.gen = g_attrGen.load();
    {
        std::lock_guard<std::mutex> lk(g_attrMtx);
        if (g_attrBusy) return;
        g_attrBusy = true;
        g_attrInfoReady = false;
        g_attrPageReady = false;
        g_attrCountReady = false;
        g_attrNeedCount = false;
        g_attrResultGen = -1;
    }
    std::thread([spec]() {
        AttrLayerInfo info;
        bool infoOk = attrOpenLayer(spec.path, spec.layerIdx, info);
        {
            std::lock_guard<std::mutex> lk(g_attrMtx);
            g_attrInfoReady = true;
            g_attrFetchedInfo = infoOk ? info : AttrLayerInfo{};   // 拷贝: 后面 fetchPage 仍要用 info
            g_attrResultGen = spec.gen;
        }
        bool pok = false;
        if (infoOk) {
            AttrPageData pd;
            pok = attrFetchPage(spec.path, spec.layerIdx, spec.page, spec.rowsPerPage,
                                (TextEncoding)spec.enc, info, pd);
            {
                std::lock_guard<std::mutex> lk(g_attrMtx);
                g_attrPageReady = pok;
                g_attrFetchedPage = std::move(pd);
                g_attrNeedCount = spec.needCount;
                if (!spec.needCount) {
                    // 翻页等任务无需总数: 页到即释放槽位
                    g_attrCountReady = true;
                    g_attrFetchedCount = info.total;
                    g_attrBusy = false;
                }
            }
        }
        if (infoOk && spec.needCount) {
            // 总数放最后: 对已渲染的行不造成等待
            long long n = attrFeatureCount(spec.path, spec.layerIdx);
            {
                std::lock_guard<std::mutex> lk(g_attrMtx);
                g_attrCountReady = true;
                g_attrFetchedCount = n;
                g_attrBusy = false;
            }
        } else if (!infoOk) {
            std::lock_guard<std::mutex> lk(g_attrMtx);
            g_attrCountReady = true;
            g_attrFetchedCount = -1;
            g_attrBusy = false;
        }
    }).detach();
}

// 将后台完成的一页合并进 ui(保留 attrInfo, 缓存页替换/追加)
static void applyAttrPage(UIState& ui, const AttrLayerInfo& info, AttrPageData pd) {
    if (!info.ok) return;
    if (info.layerName != ui.attrInfo.layerName) {
        // 绑定图层变更(通常不会在此发生)
        ui.attrInfo = info;
    }
    bool replaced = false;
    for (auto& p : ui.attrPages)
        if (p.page == pd.page) { p = std::move(pd); replaced = true; break; }
    if (!replaced) ui.attrPages.push_back(std::move(pd));
    // 缓存上限 ~5 页: 淘汰最久未用(此处仅按非当前页简单淘汰)
    while ((int)ui.attrPages.size() > 5) {
        int victim = -1;
        for (size_t i = 0; i < ui.attrPages.size(); i++)
            if (ui.attrPages[i].page != ui.attrCurrentPage) { victim = (int)i; break; }
        if (victim < 0) break;
        ui.attrPages.erase(ui.attrPages.begin() + victim);
    }
}

static std::string baseName(const std::string& p) {
    size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos + 1);
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
    initLogger(exeDir());
    spdlog::info("peekGIS 启动");
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "peekGIS", nullptr, nullptr);
    if (!window) { fprintf(stderr, "window failed\n"); return 1; }
    setWindowIcon(window);   // 窗口/任务栏图标
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, dropCallback);

    if (!gladLoadGL(glfwGetProcAddress)) { fprintf(stderr, "gladLoadGL failed\n"); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    AppConfig cfg;
    cfg.load("config.toml");
    cfg.save("config.toml");  // 落盘默认配置
    loadAppFont(io, getDpiScale(window), cfg.font_file);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    MapScene scene;
    GLBackend backend;
    backend.init();
    AsyncLoader loader;

    UIState ui;
    g_ui = &ui;

#ifdef _WIN32
    std::vector<std::string> args = utf8Args();
#else
    std::vector<std::string> args(argv, argv + argc);
#endif
    if (args.size() > 1) {  // 命令行直接打开矢量文件便于快速验证(支持中文路径)
        ui.openPaths.push_back(args[1]);
        ui.openRequested = true;
    }

    std::deque<std::string> pendingOpen;  // 多文件排队(对话框阻塞时暂存其余)

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        renderUI(scene, backend, cfg, ui);

        if (scene.needRefit && scene.view.vpW > 1) {
            scene.fitToView(scene.view.vpW, scene.view.vpH);
            scene.needRefit = false;
        }

        if (ui.openRequested) {
            pendingOpen.insert(pendingOpen.end(), ui.openPaths.begin(), ui.openPaths.end());
            ui.openPaths.clear();
            ui.openRequested = false;
        }

        // 主线程轮询: worker 完成元数据预读 -> 置 metaReady(复位 done 防止下一帧重复消费)
        if (!ui.metaReady && g_metaDone.load()) {
            ui.metaReady = true;
            g_metaDone.store(false);
        }
        // 消费后台完成的图层元数据预读(non-shp 多图层判断用)
        if (ui.metaReady) {
            ui.metaReady = false;
            ui.metaLoading = false;
            std::string p = g_metaPath;
            std::vector<LayerMeta> meta;
            bool ok;
            {
                std::lock_guard<std::mutex> lk(g_metaMtx);
                meta = std::move(g_metaResult);
                ok = g_metaOk;
            }
            if (ok && !meta.empty()) {
                if (meta.size() > 1) {
                    ui.layerMeta = std::move(meta);
                    ui.layerDialogPath = p;
                    ui.showLayerDialog = true;
                } else {
                    loader.enqueue(p, cfg, scene, backend, meta, {});
                    ui.viewTouched = false;
                }
            } else {
                LayerMeta m;
                m.name = baseName(p);
                meta.push_back(m);
                loader.enqueue(p, cfg, scene, backend, meta, {});
                ui.viewTouched = false;
                ui.status = "读取图层信息失败, 直接尝试加载";
                ui.statusErr = true;
            }
        }

        // 逐文件处理: 有进行中的预读则等它结束; shp 直接入队
        if (!ui.showLayerDialog && !pendingOpen.empty() && !ui.metaLoading && !ui.metaReady) {
            std::string p = pendingOpen.front();
            pendingOpen.pop_front();
            std::string ext = p.size() >= 4 ? p.substr(p.size() - 4) : p;
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (ext == ".shp") {
                // shp 永远是单图层: 跳过元数据预读, 直接异步入队(不卡主线程, 渐进渲染)
                LayerMeta m;
                m.name = baseName(p);
                m.featureCount = -1;   // 未知, 占位显示
                loader.enqueue(p, cfg, scene, backend, {m}, {});
                ui.viewTouched = false;
            } else {
                // 其他格式可能多图层: 后台线程读元数据(避免卡死), 完成后决定对话框/入库
                ui.metaLoading = true;
                ui.status = "正在读取图层信息: " + baseName(p);
                ui.statusErr = false;
                g_metaDone.store(false);
                std::thread([p]() {
                    std::vector<LayerMeta> m;
                    bool ok = readLayerMetadata(p, m);
                    {
                        std::lock_guard<std::mutex> lk(g_metaMtx);
                        g_metaResult = std::move(m);
                        g_metaOk = ok;
                        g_metaPath = p;
                    }
                    g_metaDone.store(true);
                }).detach();
            }
        }

        if (ui.clearRequested) {
            scene.clearLayers();
            backend.clearLayers();
            loader.cancel();
            ui.displayCrsChoice = 0;
            ui.status.clear();
            ui.statusErr = false;
            ui.loadActive = false;
            ui.attrTableOpen = false;
            ui.attrPages.clear();
            ui.attrBindLayer = -1;
            ui.attrHlActive = false;
            g_attrGen.fetch_add(1);    // 作废在途属性表任务
            ui.clearRequested = false;
        }

        // 卸载单个图层(右键菜单)
        if (ui.removeLayerRequested) {
            int idx = ui.removeLayerIdx;
            ui.removeLayerRequested = false;
            ui.removeLayerIdx = -1;
            if (idx >= 0 && idx < (int)scene.layers.size()) {
                std::string gone = scene.layers[idx].info.name;
                loader.cancel();              // 中止进行中加载, 避免块事件对错位图层追加
                if (idx == ui.attrBindLayer) {
                    ui.attrTableOpen = false;
                    ui.attrPages.clear();
                    ui.attrBindLayer = -1;
                    ui.attrHlActive = false;
                    g_attrGen.fetch_add(1);    // 作废在途属性表任务
                }
                backend.removeLayer(idx);
                scene.removeLayer(idx);
                ui.status = "已卸载图层: " + gone;
                ui.statusErr = false;
            }
        }

        // 从 WKT 创建新图层(File ▸ Render WKT... 对话框确认后)
        if (ui.wktRequested) {
            std::string wkt = ui.wktText;
            ui.wktText.clear();
            ui.wktRequested = false;

            static const char kAlpha[] =
                "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
            std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
            std::string name;
            for (int i = 0; i < 8; i++) name += kAlpha[rng() % (sizeof(kAlpha) - 1)];

            int base = scene.displayEpsg;
            if (base == 0 && !scene.layers.empty()) base = scene.layers[0].data.srcEpsg;
            if (base == 0) base = 4326;
            std::string crs = "EPSG:" + std::to_string(base);

            VectorData vd;
            if (loadWktToVectorData(name, wkt, crs, base, vd)) {
                scene.addLayer(vd);
                backend.addLayer(vd.vertices, vd.points, vd.triangles);
                ui.status = "已从 WKT 创建图层 " + name + " (" + crs + ")";
                ui.statusErr = false;
                ui.viewTouched = false;   // 立即适配到新图层
            } else {
                ui.status = "WKT 解析失败或无可绘制几何";
                ui.statusErr = true;
            }
        }

        if (ui.loadFilteredRequested) {
            loader.enqueue(ui.loadFilteredPath, cfg, scene, backend,
                           std::move(ui.loadFilteredMeta), ui.loadFilteredIndices);
            ui.viewTouched = false;
            ui.loadFilteredRequested = false;
        }

        // 底部属性表: 打开/切换图层(快照绑定, 后台拉取第 0 页)
        if (ui.attrOpenRequested) {
            ui.attrOpenRequested = false;
            int li = ui.attrOpenLayerIdx;
            ui.attrOpenLayerIdx = -1;
            if (li >= 0 && li < (int)scene.layers.size() && !scene.layers[li].sourcePath.empty()) {
                ui.attrBindLayer = li;
                ui.attrPath = scene.layers[li].sourcePath;
                ui.attrFileLayerIdx = scene.layers[li].sourceLayerIdx;
                ui.attrSrcEpsg = scene.layers[li].data.srcEpsg;
                ui.attrPages.clear();
                ui.attrInfo = AttrLayerInfo{};
                ui.attrShowCol.clear();
                ui.attrCurrentPage = 0;
                ui.attrHlActive = false;
                ui.attrLocateRequested = false;
                ui.attrLoading = true;
                ui.attrTableOpen = false;
                g_attrGen.fetch_add(1);    // 作废任何在途旧任务结果
                AttrTaskSpec spec;
                spec.path = ui.attrPath;
                spec.layerIdx = ui.attrFileLayerIdx;
                spec.page = 0;
                spec.rowsPerPage = ui.attrRowsPerPage;
                spec.enc = ui.attrEncoding;
                spec.needCount = true;         // 打开时需取总数
                launchAttrTask(spec);
            } else {
                ui.status = "该图层无源文件, 无法打开属性表";
                ui.statusErr = true;
            }
        }

        // 底部属性表: 翻页请求
        if (ui.attrGotoRequested) {
            ui.attrGotoRequested = false;
            if (ui.attrTableOpen && !ui.attrPath.empty() && ui.attrInfo.ok) {
                ui.attrLoading = true;
                AttrTaskSpec spec;
                spec.path = ui.attrPath;
                spec.layerIdx = ui.attrFileLayerIdx;
                spec.page = ui.attrCurrentPage;
                spec.rowsPerPage = ui.attrRowsPerPage;
                spec.enc = ui.attrEncoding;
                launchAttrTask(spec);
            }
        }

        // 底部属性表: 双击行居中(仅平移不缩放)
        if (ui.attrLocateRequested) {
            ui.attrLocateRequested = false;
            if (ui.attrHlActive) {
                double sx = ui.attrLocateSrcX, sy = ui.attrLocateSrcY;
                int targetEpsg = scene.displayEpsg;
                if (targetEpsg == 0) targetEpsg = ui.attrSrcEpsg;
                double dx = sx, dy = sy;
                if (targetEpsg != 0 && ui.attrSrcEpsg != 0 && targetEpsg != ui.attrSrcEpsg) {
                    std::vector<float> in = {(float)sx, (float)sy}, out;
                    if (reprojectVertices(in, ui.attrSrcEpsg, targetEpsg, out) && out.size() >= 2) {
                        dx = out[0]; dy = out[1];
                    }
                }
                double px = (scene.view.centerX - dx) / scene.view.scale;
                double py = (dy - scene.view.centerY) / scene.view.scale;
                scene.pan(px, py);
                ui.viewTouched = true;
            }
        }

        loader.update(scene, backend, ui);  // 每帧消费后台结果(渐进绘制)

        // 取回属性表后台任务结果
        {
            int curGen = g_attrGen.load();
            std::lock_guard<std::mutex> lk(g_attrMtx);
            if (g_attrInfoReady) {
                g_attrInfoReady = false;
                if (g_attrResultGen == curGen) {
                    ui.attrInfo = std::move(g_attrFetchedInfo);
                    if ((int)ui.attrShowCol.size() != (int)ui.attrInfo.fields.size())
                        ui.attrShowCol.assign(ui.attrInfo.fields.size(), 1);  // 默认全显示
                    ui.attrTableOpen = ui.attrInfo.ok;
                }
                // 代号不符(已重开/清空/卸载): 丢弃迟到结果
            }
            if (g_attrPageReady) {
                g_attrPageReady = false;
                if (g_attrResultGen == curGen && ui.attrTableOpen && ui.attrInfo.ok) {
                    applyAttrPage(ui, ui.attrInfo, std::move(g_attrFetchedPage));
                    if (!g_attrNeedCount) ui.attrLoading = false;   // 无需总数: 页到即完成
                }
            }
            if (g_attrCountReady) {
                g_attrCountReady = false;
                if (g_attrResultGen == curGen) {
                    if (g_attrNeedCount && ui.attrTableOpen && ui.attrInfo.ok)
                        ui.attrInfo.total = g_attrFetchedCount;   // 仅打开任务回填总数
                    ui.attrLoading = false;                    // 整任务完成
                }
            }
        }
        // 若加载完成后当前页与请求目标不符, 重新下发(极端连点翻页情形)
        if (!ui.attrLoading && ui.attrTableOpen && ui.attrInfo.ok) {
            bool haveCur = false;
            for (const auto& p : ui.attrPages)
                if (p.page == ui.attrCurrentPage) { haveCur = true; break; }
            if (!haveCur) {
                ui.attrLoading = true;
                AttrTaskSpec spec;
                spec.path = ui.attrPath;
                spec.layerIdx = ui.attrFileLayerIdx;
                spec.page = ui.attrCurrentPage;
                spec.rowsPerPage = ui.attrRowsPerPage;
                spec.enc = ui.attrEncoding;
                launchAttrTask(spec);
            }
        }

        // 取回异步识别 - 逐目标增量提交(几何+属性一起), 避免大文件占用时阻塞其他图层
        {
            std::lock_guard<std::mutex> lk(g_identifyMtx);
            if (g_identifyGeoReady) {
                g_identifyGeoReady = false;
                for (auto& h : g_identifyGeo) {
                    h.applyEncoding((TextEncoding)ui.identifyEncoding);  // 用用户保留的固定编码解释新命中
                    ui.identifyGeo.push_back(h);
                    ui.identify.push_back(std::move(h));
                }
                g_identifyGeo.clear();
            }
            if (g_identifyDone) {
                g_identifyDone = false;
                ui.identifyPending = false;
                if (g_identifyRemaining == 0 && ui.identify.empty()) {
                    ui.status = "未命中要素";
                    ui.statusErr = true;
                }
            }
        }
        if (ui.identifyPending && !ui.identifyGeo.empty()) {
            // 已有部分结果仍在查询中, 状态保持
            ui.status = "属性查询中...";
            ui.statusErr = false;
        }

        // 发起异步属性识别: 双击地图后抓拍可见图层快照, 后台线程逐层查询
        if (ui.identifyRequested) {
            ui.identifyRequested = false;
            ui.identify.clear();
            ui.identifyGeo.clear();
            ui.identifyPending = true;
            ui.status = "属性查询中...";
            ui.statusErr = false;
            std::vector<IdentifyTarget> targets;
            for (const auto& L : scene.layers) {
                if (!L.info.visible) continue;
                if (L.sourcePath.empty()) continue;   // WKT 图层无源文件, 跳过属性查询
                targets.push_back({L.sourcePath, L.sourceLayerIdx, L.data.srcEpsg});
            }
            double qx = ui.identifyX, qy = ui.identifyY;
            double tol = 10.0 * scene.view.scale;   // 10 像素点击容差(显示单位)
            int dispEpsg = scene.displayEpsg;
            if (targets.empty()) {
                ui.identifyPending = false;
                ui.status = "未命中要素";
                ui.statusErr = true;
            } else {
                uint64_t gen = g_identifyGen.fetch_add(1) + 1;
                ui.identify.clear();
                ui.identifyGeo.clear();
                ui.identifyPending = true;
                {
                    std::lock_guard<std::mutex> lk(g_identifyMtx);
                    g_identifyRemaining = (int)targets.size();
                    g_identifyDone = false;
                }
                // 每个目标一个线程: 某文件首次打开(几十秒)不阻塞其他图层的查询,
                // 每个命中独立、增量提交给主线程即时高亮
                for (const auto& t : targets) {
                    std::thread([gen, t, dispEpsg, qx, qy, tol]() {
                        IdentifyHit h;
                        bool ok = false;
                        if (identifyFeatures(t.path, t.layerIdx, qx, qy, tol,
                                             dispEpsg, t.srcEpsg, h))
                            ok = true;
                        {
                            std::lock_guard<std::mutex> lk(g_identifyMtx);
                            if (gen == g_identifyGen.load()) {
                                if (ok) {
                                    g_identifyGeo.push_back(std::move(h));
                                    g_identifyGeoReady = true;
                                }
                                g_identifyRemaining--;
                                if (g_identifyRemaining <= 0) {
                                    g_identifyRemaining = 0;
                                    g_identifyDone = true;
                                }
                            }
                        }
                    }).detach();
                }
            }
        }

        // 进度填充
        LoadStats st = loader.stats();
        if (st.activeFiles > 0) {
            ui.loadActive = true;
            ui.loadFraction = st.fraction();
            ui.loadDoneFiles = st.doneFiles;
            ui.loadTotalFiles = st.totalFiles;
        } else {
            ui.loadActive = false;
        }

        ImGui::Render();
        int w, h; glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
