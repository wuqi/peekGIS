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

    // 超 Lmax 动态直读: 视口期望层超出缓存最深层时, 允许以原始精度分块直读源数据(不抽稀)。
    // raw_over_max 总开关(默认开); raw_budget_ms 为单层单遍可见区扫描的读盘预算(毫秒),
    // 实测读盘效率(扫描要素数/耗时 EWMA)超预算则回退 Lmax 缓存。
    bool vt_raw_over_max = true;
    double vt_raw_budget_ms = 150.0;

    // 把 log_level 解析成 spdlog 等级(非法值返回 debug)
    int spdlogLevel() const;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};
