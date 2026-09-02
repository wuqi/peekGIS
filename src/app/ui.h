#pragma once
#include <string>
#include <vector>
#include "map/map_scene.h"
#include "render/gl_backend.h"
#include "config/app_config.h"
#include "data/gdal_datasource.h"
#include "data/attr_table.h"

struct UIState {
    std::vector<std::string> openPaths;  // 待打开的多个文件路径(由 main 消费)
    bool openRequested = false;
    bool clearRequested = false;  // File ▸ Clear Layers
    int displayCrsChoice = 0; // 0=Source, 1=EPSG:4326, 2=EPSG:3857, 3=EPSG:4490
    bool viewTouched = false;  // 用户在加载期间是否手动平移/缩放过视图
    std::string status;       // 状态栏提示(加载结果等)
    bool statusErr = false;   // 是否为错误提示
    bool showCacheManager = false; // 缓存管理窗口

    // 加载进度(由 main 每帧从 AsyncLoader::stats 填充, 状态栏显示)
    bool loadActive = false;      // 是否有进行中/排队加载
    float loadFraction = -1.0f;   // 精确进度(要素级); <0 表示未知
    int loadDoneFiles = 0;        // 已完成文件数
    int loadTotalFiles = 0;       // 总文件数

    // 多图层选择对话框
    bool showLayerDialog = false;
    std::vector<LayerMeta> layerMeta;  // 读取到的图层元数据
    std::string layerDialogPath;       // 正在选择的文件路径

    // 图层选择确认后的加载请求(由 main.cpp 消费)
    bool loadFilteredRequested = false;
    std::string loadFilteredPath;
    std::vector<int> loadFilteredIndices;
    std::vector<LayerMeta> loadFilteredMeta; // 确认选中的元数据(复用, 免二次扫描)

    // 后台图层元数据预读(non-shp 多图层判断用): 由 main.cpp 维护
    bool metaLoading = false;          // 后台读取中(状态栏提示)
    bool metaReady = false;            // 后台完成待消费

    // 属性识别(双击地图, 后台线程异步查询)
    bool identifyRequested = false;
    bool identifyPending = false;             // 查询进行中(右栏显示占位)
    double identifyX = 0, identifyY = 0;      // 双击点世界坐标(显示CRS)
    std::vector<IdentifyHit> identify;        // 完整结果(含属性, 查询完成后)
    std::vector<IdentifyHit> identifyGeo;     // 仅几何(异步线程先生发, 用于立即高亮)
    int identifyEncoding = 0;                 // 右栏属性显示编码(index into kEncodingNames)

    // 底部属性表面板
    int attrEncoding = 0;                     // 当前编码(index into kEncodingNames)
    bool attrTableOpen = false;               // 面板是否打开(有绑定图层)
    int attrBindLayer = -1;                   // 绑定的 scene 图层索引
    std::string attrPath;                     // 源文件路径(快照)
    int attrFileLayerIdx = 0;                 // 文件内图层索引(快照)
    int attrSrcEpsg = 0;                      // 源 CRS(快照)
    AttrLayerInfo attrInfo;                   // 图层信息(总数/字段)
    std::vector<char> attrShowCol;            // 每字段是否显示(1=显示); 空=全显示
    int attrRowsPerPage = 20;
    int attrCurrentPage = 0;                  // 当前页(0 基)
    std::vector<AttrPageData> attrPages;      // 已缓存页
    bool attrLoading = false;                 // 后台拉取中
    bool attrOpenRequested = false;           // 请求打开/切换图层(由 main 消费)
    int attrOpenLayerIdx = -1;                // 待打开图层(先写, 再置 openRequested)
    bool attrGotoRequested = false;           // 请求跳转到 attrCurrentPage
    // 双击定位高亮(几何为源 CRS)
    bool attrHlActive = false;
    int attrHlSrcEpsg = 0;
    std::vector<float> attrHlOutline, attrHlPoints, attrHlTris;
    // 双击行居中请求(由 main 消费; 目标点为源 CRS)
    bool attrLocateRequested = false;
    double attrLocateSrcX = 0, attrLocateSrcY = 0;

    // WKT 渲染: 弹输入框粘贴 WKT → 解析后创建新图层
    bool showWktDialog = false;
    char wktBuf[8192] = {0};                  // ImGui 输入缓冲
    bool wktRequested = false;                // 确认后由 main.cpp 消费解析
    std::string wktText;                      // 待解析的 WKT 内容
    std::string wktCrsHint;                   // 对话框中的坐标系提示

    // 卸载图层请求(由 main.cpp 消费, 按索引卸载)
    bool removeLayerRequested = false;
    int removeLayerIdx = -1;
};

void renderUI(MapScene& scene, GLBackend& backend, AppConfig& cfg, UIState& ui);
