#pragma once
#include <string>

struct AppConfig {
    std::string cache_dir = "cache";      // 相对 exe 目录
    long long cache_max_mb = 512;         // LRU 上限
    std::string default_crs = "auto";     // auto=用数据自身CRS, 否则写 EPSG 数字, 如 "4326"
    bool dpi_aware = true;
    std::string font_file = "fonts/LXGW.ttf"; // 霞鹜新晰黑
    std::string log_level = "debug";      // 日志等级: trace/debug/info/warn/error/critical
    long long bake_threshold_features = 500000;   // 要素数超此值才走烘焙(大数据); 否则走原矢量路径

    // 把 log_level 解析成 spdlog 等级(非法值返回 debug)
    int spdlogLevel() const;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};
