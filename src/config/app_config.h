#pragma once
#include <string>

struct AppConfig {
    std::string cache_dir = "cache";      // 相对 exe 目录
    long long cache_max_mb = 512;         // LRU 上限
    std::string default_crs = "auto";     // auto=用数据自身CRS, 否则写 EPSG 数字, 如 "4326"
    bool dpi_aware = true;
    std::string font_file = "fonts/LXGW.ttf"; // 霞鹜新晰黑
    std::string log_level = "debug";      // 日志等级: trace/debug/info/warn/error/critical

    // v2 矢量瓦片自动分流: 打开源文件无缓存且估算顶点数超阈值时, 后台生成瓦片缓存
    bool vt_auto_build = true;
    long long vt_threshold_verts = 10000000;   // 顶点数阈值(超过走 v2)
    int vt_level_step = 2;                      // 隔层构建: 每 step 层保留一层(从 L0 起)+最深层; 1=每层都建

    // 选层: 保留率判据。建到"能保留 target_keep 比例的源顶点"为止。
    // 1.0 = 按 target_keep; >1 更容易达标(更浅省空间); <1 更深。
    // 详见 docs/矢量缓存.md。
    double vt_target_error_factor = 1.0;
    double vt_target_keep = 0.5;             // 目标保留率(默认 50%)
    int vt_target_verts = 2048;                 // 每瓦片顶点数: 仅旧顶点数策略使用
    long long vt_max_total_verts = 100000000;   // 体积安全阀: 保留层总顶点预算(默认 1 亿)
    long long vt_max_verts_per_tile = 262144;   // 体积安全阀: 单片顶点数上限(默认 262144)
    int vt_levels = -1;                         // >=0 直接强制最深层, 跳过自动估算

    // 超 Lmax 动态直读: 视口期望层超出缓存最深层时, 允许以原始精度分块直读源数据(不抽稀)。
    // raw_over_max 总开关(默认开); raw_budget_ms 为单层单遍可见区扫描的读盘预算(毫秒)。
    // 扫描按 kRawChunkMs=8ms 分片投递 worker, 渐进出结果, 不阻塞界面, 所以这个预算是
    // "值不值得花这么多盘读"而非"卡不卡界面": 超了只是本次不进(继续用 Lmax 缓存),
    // 放大后区域变小会自动再进 —— 不要拿它当硬性交互预算(150ms 会把 48 号这种
    // 实测 3 万要素/s 的有索引源直接判死, 导致再放大也永远看不到原始数据)。
    bool vt_raw_over_max = true;
    double vt_raw_budget_ms = 2000.0;

    // 把 log_level 解析成 spdlog 等级(非法值返回 debug)
    int spdlogLevel() const;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};
