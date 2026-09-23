#pragma once
#include <string>
#include <vector>
#include "map/map_scene.h"
#include "render/gl_backend.h"
#include "config/app_config.h"
#include "data/vector_reader.h"
#include "data/attr_table.h"

using peekg::data::LayerMeta;
using peekg::data::IdentifyHit;
using peekg::data::AttrLayerInfo;
using peekg::data::AttrPageData;

struct UIState {
    // ---- 全局/窗口显隐与布局 ----
    std::vector<std::string> openPaths;  // 待打开的多个文件路径(由 main 消费)
    bool openRequested = false;
    bool clearRequested = false;  // File ▸ Clear Layers
    int displayCrsChoice = 0; // 0=Source, 1=EPSG:4326, 2=EPSG:3857, 3=EPSG:4490
    bool viewTouched = false;  // 用户在加载期间是否手动平移/缩放过视图
    std::string status;       // 状态栏提示(加载结果等)
    bool statusErr = false;   // 是否为错误提示
    bool showCacheManager = false; // 缓存管理窗口
    int leftPanel = 0;             // 左侧栏当前面板: 0=图层树, 1=工具箱(全局唯一, 类似 VS Code activity bar)
    bool showAttributesPanel = true; // 右侧属性识别面板显隐
    bool showAttrTablePanel = true;  // 底部属性表面板显隐

    // ---- 多图层/子数据集选择对话框 ----
    bool showLayerDialog = false;
    std::vector<LayerMeta> layerMeta;  // 读取到的图层元数据
    std::string layerDialogPath;       // 正在选择的文件路径
    bool sdsDialog = false;            // 当前对话框是否为栅格子数据集选择(而非矢量多图层)
    // 图层选择确认后的加载请求(由 main.cpp 消费)
    bool loadFilteredRequested = false;
    std::string loadFilteredPath;
    std::vector<int> loadFilteredIndices;
    std::vector<LayerMeta> loadFilteredMeta; // 确认选中的元数据(复用, 免二次扫描)
    // 后台图层元数据预读(non-shp 多图层判断用): 由 main.cpp 维护
    bool metaLoading = false;          // 后台读取中(状态栏提示)
    bool metaReady = false;            // 后台完成待消费

    // ---- 加载进度(由 main 每帧从 AsyncLoader::stats 填充, 状态栏显示) ----
    bool loadActive = false;      // 是否有进行中/排队加载
    float loadFraction = -1.0f;   // 精确进度(要素级); <0 表示未知
    int loadDoneFiles = 0;        // 已完成文件数
    int loadTotalFiles = 0;       // 总文件数

    // ---- 属性识别(双击地图, 后台线程异步查询) ----
    struct Identify {
        bool requested = false;
        bool pending = false;                 // 查询进行中(右栏显示占位)
        double x = 0, y = 0;                  // 双击点世界坐标(显示CRS)
        std::vector<IdentifyHit> full;        // 完整结果(含属性, 查询完成后)
        std::vector<IdentifyHit> geo;         // 仅几何(异步线程先生发, 用于立即高亮)
        int encoding = 0;                     // 右栏属性显示编码(index into kEncodingNames)
    } identify;

    // ---- 底部属性表面板 ----
    struct AttrTable {
        int encoding = 0;                     // 当前编码(index into kEncodingNames)
        bool isOpen = false;                  // 面板是否打开(有绑定图层)
        int bindLayer = -1;                   // 绑定的 scene 图层索引
        std::string path;                     // 源文件路径(快照)
        int fileLayerIdx = 0;                 // 文件内图层索引(快照)
        int srcEpsg = 0;                      // 源 CRS(快照)
        AttrLayerInfo info;                   // 图层信息(总数/字段)
        std::vector<char> showCol;            // 每字段是否显示(1=显示); 空=全显示
        int rowsPerPage = 20;
        int currentPage = 0;                  // 当前页(0 基)
        std::vector<AttrPageData> pages;      // 已缓存页
        bool loading = false;                 // 后台拉取中
        bool openRequested = false;           // 请求打开/切换图层(由 main 消费)
        int openLayerIdx = -1;                // 待打开图层(先写, 再置 openRequested)
        bool gotoRequested = false;           // 请求跳转到 currentPage
        // 双击定位高亮(几何为源 CRS)
        bool hlActive = false;
        int hlSrcEpsg = 0;
        std::vector<float> hlOutline, hlPoints, hlTris;
        // 双击行居中请求(由 main 消费; 目标点为源 CRS)
        bool locateRequested = false;
        double locateSrcX = 0, locateSrcY = 0;
    } attr;

    // ---- WKT 渲染对话框 ----
    bool showWktDialog = false;
    char wktBuf[8192] = {0};                  // ImGui 输入缓冲
    bool wktRequested = false;                // 确认后由 main.cpp 消费解析
    std::string wktText;                      // 待解析的 WKT 内容
    std::string wktCrsHint;                   // 对话框中的坐标系提示

    // ---- PostgreSQL 打开对话框(File ▸ Open PostgreSQL...) ----
    bool showPgDialog = false;
    char pgHost[256] = "localhost";
    char pgPort[16] = "5432";
    char pgDb[256] = "";
    char pgUser[128] = "";
    char pgPwd[128] = "";
    int pgSslMode = 0;                        // 见 kPgSslModes: prefer/disable/allow/require/verify-ca/verify-full
    char pgExtra[512] = "";                   // 其它 libpq 参数(key=value&...)
    std::string pgError;                      // 组串校验错误提示

    // ---- 最近打开(最多 5 条): 文件路径 或 脱敏后的 PG 连接串(不含密码) ----
    std::vector<std::string> recent;
    bool recentDirty = false;                 // 列表变更, 由 App 落盘

    // ---- 图层操作请求(由 main.cpp 消费) ----
    bool removeLayerRequested = false;
    int removeLayerIdx = -1;

    // ---- 栅格图层渲染设置弹出窗口(右键栅格图层→栅格设置) ----
    bool rasterSettingsOpen = false;
    int rasterSettingsLayer = -1;          // 目标图层(scene.layers 索引)
    bool rasterSettingsDirty = false;      // 设置变更请求(由 main.cpp 消费, 重渲染)
};

void renderUI(MapScene& scene, GLBackend& backend, AppConfig& cfg, UIState& ui);

// 切换/统一显示CRS: 对每个图层源坐标单遍重投影后重传VBO(0 = 以首个已知源CRS为基准)
void applyDisplayCrs(MapScene& scene, GLBackend& backend, int dstEpsg);
