#include "app_config.h"
#include <toml.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>

int AppConfig::spdlogLevel() const {
    if (log_level == "trace")  return 0;
    if (log_level == "debug")  return 1;
    if (log_level == "info")   return 2;
    if (log_level == "warn")   return 3;
    if (log_level == "error")  return 4;
    if (log_level == "critical") return 5;
    return 1;   // 默认 debug
}

bool AppConfig::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    try {
        // 注意: 用 toml::parse(path) 字符串重载(内部以二进制打开), 而不是 parse(f)。
        // toml11 的 parse(istream&) 在 Windows 文本模式下 tellg() 返回的偏移不是真实
        // 字节数, 读不满 buffer 会在尾部留下 NUL(报错"第N行全是 NUL")。
        auto data = toml::parse(path);
        if (data.contains("cache")) {
            auto& c = data.at("cache");
            if (c.contains("dir")) cache_dir = toml::find<std::string>(c, "dir");
            if (c.contains("max_size_mb")) cache_max_mb = toml::find<int64_t>(c, "max_size_mb");
        }
        if (data.contains("display")) {
            auto& d = data.at("display");
            if (d.contains("default_crs")) default_crs = toml::find<std::string>(d, "default_crs");
            if (d.contains("dpi_aware")) dpi_aware = toml::find<bool>(d, "dpi_aware");
        }
        if (data.contains("font")) {
            auto& ft = data.at("font");
            if (ft.contains("file")) font_file = toml::find<std::string>(ft, "file");
        }
        if (data.contains("log")) {
            auto& lg = data.at("log");
            if (lg.contains("level")) log_level = toml::find<std::string>(lg, "level");
        }
        if (data.contains("vt")) {
            auto& v = data.at("vt");
            if (v.contains("auto_build")) vt_auto_build = toml::find<bool>(v, "auto_build");
            if (v.contains("threshold_verts")) vt_threshold_verts = toml::find<int64_t>(v, "threshold_verts");
            if (v.contains("level_step")) vt_level_step = toml::find<int>(v, "level_step");
        }
        // 归一化: 隔层步长至少 1(0/负值会静默变成"每层都建"), 阈值为负则视为 0
        vt_level_step = std::max(1, vt_level_step);
        if (vt_threshold_verts < 0) vt_threshold_verts = 0;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[config] load failed: " << e.what() << "\n";
        std::remove(path.c_str());  // 损坏的配置直接删掉, 下一次 save 会写出干净版本
        return false;
    }
}

bool AppConfig::save(const std::string& path) const {
    try {
        toml::value data;
        data["cache"] = toml::table{
            {"dir", cache_dir},
            {"max_size_mb", cache_max_mb},
        };
        data["display"] = toml::table{
            {"default_crs", default_crs},
            {"dpi_aware", dpi_aware},
        };
        data["font"] = toml::table{
            {"file", font_file},
        };
        data["log"] = toml::table{
            {"level", log_level},
        };
        data["vt"] = toml::table{
            {"auto_build", vt_auto_build},
            {"threshold_verts", vt_threshold_verts},
            {"level_step", vt_level_step},
        };
        std::ofstream f(path);
        if (!f) return false;
        f << data;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[config] save failed: " << e.what() << "\n";
        return false;
    }
}
