#pragma once
#include <string>

struct AppConfig {
    std::string cache_dir = "cache";      // 相对 exe 目录
    long long cache_max_mb = 512;         // LRU 上限
    std::string default_crs = "auto";     // auto=用数据自身CRS, 否则写 EPSG 数字, 如 "4326"
    bool dpi_aware = true;
    std::string font_file = "fonts/LXGW.ttf"; // 霞鹜新晰黑

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};
