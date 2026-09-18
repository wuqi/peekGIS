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

    // 把 log_level 解析成 spdlog 等级(非法值返回 debug)
    int spdlogLevel() const;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};
